// Standalone BC7 sparse compatibility diagnostic. Build/run: see README.md in this folder.
// Public Vulkan APIs only; no runtime, guest address space, VMA, shaders or application data.
// Binding and transfer ordering follows https://docs.vulkan.org/spec/latest/chapters/sparsemem.html.
// Each queue operation has a timeline dependency AND an independently observed, bounded fence wait.
// build.py discovers all runtime C files: keep this diagnostic's main out of normal runtime links.
#ifdef MKW_STANDALONE_SPARSE_PROBE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define MIB (1024ull * 1024)
#define CAP (128 * MIB)
#define MAX_REGIONS (68 * 14)
#define MAX_BINDS 16384
#define TIMEOUT_NS 5000000000ull

static atomic_uint errors, warnings;
static const char *checkpoint = "startup", *scenario = "device";
static const char *failure_kind = "PROBE_CHECK";
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkPhysicalDeviceProperties props;
static VkPhysicalDeviceMemoryProperties mem;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkSemaphore timeline;
static uint64_t serial, live, peak, heap_live[VK_MAX_MEMORY_HEAPS];
static int has_budget;

static void fail(const char *why) {
    fprintf(stderr, "FAIL [%s/%s] kind=%s %s; validation errors=%u warnings=%u; live=%llu peak=%llu bytes\n",
            scenario, checkpoint, failure_kind, why, atomic_load(&errors), atomic_load(&warnings),
            (unsigned long long)live, (unsigned long long)peak);
    // On device loss/timeout, do not issue more work or wait indefinitely in device destruction.
    // OS process teardown releases resources, including partially constructed resources.
    exit(1);
}

static void require(int condition, const char *why) { if (!condition) fail(why); }
static void conformance_require(int condition, const char *why) {
    if (condition) return;
    failure_kind = "REQUIREMENTS_CONFORMANCE";
    fprintf(stderr, "CONFORMANCE_FAILURE [%s/%s] incompatible public Vulkan requirements; "
            "not evidence of a failed GPU operation; queue_operations_issued=%llu\n",
            scenario, checkpoint, (unsigned long long)serial);
    fail(why);
}
static void result(VkResult r, const char *api) {
    printf("API [%s/%s] %s -> %d\n", scenario, checkpoint, api, r);
    if (r != VK_SUCCESS) {
        failure_kind = r == VK_ERROR_DEVICE_LOST || r == VK_TIMEOUT || strstr(api, "vkQueue") ||
            strstr(api, "vkWaitForFences") || strstr(api, "vkGetFenceStatus") ? "GPU_OPERATION" : "API_RESULT";
        fail(api);
    }
    if (atomic_load(&errors)) failure_kind = "VALIDATION";
    require(!atomic_load(&errors), "validation error");
}
#define VK(call) result((call), #call)
static void start(const char *name) {
    checkpoint = name;
    printf("BEGIN [%s/%s]\n", scenario, checkpoint);
}
static void pass(void) {
    if (atomic_load(&errors)) failure_kind = "VALIDATION";
    require(!atomic_load(&errors), "validation error");
    printf("PASS [%s/%s]\n", scenario, checkpoint);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type; (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) atomic_fetch_add(&errors, 1);
    else atomic_fetch_add(&warnings, 1);
    fprintf(stderr, "VALIDATION [%s/%s] %s: %s\n", scenario, checkpoint,
            data->pMessageIdName ? data->pMessageIdName : "unknown", data->pMessage);
    return VK_FALSE;
}

static VkPhysicalDeviceMemoryBudgetPropertiesEXT budget(void) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                         .pNext = has_budget ? &b : NULL};
    vkGetPhysicalDeviceMemoryProperties2(gpu, &p);
    return b;
}

static void memory_log(void) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = budget();
    printf("MEMORY live=%llu peak=%llu cap=%llu bytes (requested allocations; excludes driver overhead)\n",
           (unsigned long long)live, (unsigned long long)peak, (unsigned long long)CAP);
    for (uint32_t h = 0; h < mem.memoryHeapCount; h++)
        printf("HEAP %u size=%llu flags=%#x budget=%llu usage=%llu probe=%llu bytes\n", h,
               (unsigned long long)mem.memoryHeaps[h].size, mem.memoryHeaps[h].flags,
               (unsigned long long)b.heapBudget[h], (unsigned long long)b.heapUsage[h],
               (unsigned long long)heap_live[h]);
}

typedef struct { VkDeviceMemory handle; VkDeviceSize size; uint32_t type; } Allocation;
typedef struct { VkBuffer handle; Allocation allocation; uint8_t *map; } Buffer;
typedef struct {
    VkImage handle;
    VkMemoryRequirements req;
    VkSparseImageMemoryRequirements sparse[8];
    uint32_t nsparse, width, layers, mips, ntiles, ntails, nregions;
    VkSparseImageMemoryBind tiles[MAX_BINDS];
    VkSparseMemoryBind tails[MAX_REGIONS];
    VkBufferImageCopy regions[MAX_REGIONS];
    VkDeviceSize backing, bytes;
    Allocation allocation;
    VkBuffer backing_buffer;
    Buffer upload, readback;
} Image;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer) {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & need) != need) continue;
        if ((f & prefer) == prefer) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    require(fallback != UINT32_MAX, "no compatible memory type");
    return fallback;
}

static Allocation allocate(VkDeviceSize bytes, uint32_t type) {
    uint32_t h = mem.memoryTypes[type].heapIndex;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = budget();
    require(bytes && bytes <= CAP - live, "128 MiB allocation cap");
    require(heap_live[h] + bytes <= mem.memoryHeaps[h].size / 10, "heap safety limit (10%)");
    if (has_budget) {
        VkDeviceSize available = b.heapBudget[h] > b.heapUsage[h] ? b.heapBudget[h] - b.heapUsage[h] : 0;
        require(bytes <= available / 4, "insufficient reported heap budget (75% headroom)");
    }
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = bytes, .memoryTypeIndex = type};
    Allocation a = {.size = bytes, .type = type};
    printf("ALLOC bytes=%llu type=%u heap=%u properties=%#x\n", (unsigned long long)bytes, type, h,
           mem.memoryTypes[type].propertyFlags);
    VK(vkAllocateMemory(device, &ai, NULL, &a.handle));
    live += bytes; heap_live[h] += bytes;
    if (live > peak) peak = live;
    return a;
}

static void release_allocation(Allocation a) {
    if (!a.handle) return;
    vkFreeMemory(device, a.handle, NULL);
    live -= a.size;
    heap_live[mem.memoryTypes[a.type].heapIndex] -= a.size;
}

static Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b = {0};
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK(vkCreateBuffer(device, &ci, NULL, &b.handle));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(device, b.handle, &r);
    b.allocation = allocate(r.size, memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    VK(vkBindBufferMemory(device, b.handle, b.allocation.handle, 0));
    void *map = NULL;
    VK(vkMapMemory(device, b.allocation.handle, 0, VK_WHOLE_SIZE, 0, &map));
    b.map = map;
    return b;
}

static void release_buffer(Buffer b) {
    vkUnmapMemory(device, b.allocation.handle);
    vkDestroyBuffer(device, b.handle, NULL);
    release_allocation(b.allocation);
}

static void wait_checkpoint(void) {
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, TIMEOUT_NS));
    VK(vkGetFenceStatus(device, fence));
    uint64_t value = 0;
    VK(vkGetSemaphoreCounterValue(device, timeline, &value));
    require(value == serial, "unexpected timeline completion value");
    VK(vkResetFences(device, 1, &fence));
    pass();
}

static void begin(void) {
    VK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK(vkBeginCommandBuffer(cmd, &bi));
}

static void submit(void) {
    VK(vkEndCommandBuffer(cmd));
    uint64_t previous = serial, next = ++serial;
    VkTimelineSemaphoreSubmitInfo tl = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = previous ? 1u : 0u, .pWaitSemaphoreValues = &previous,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &next};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tl,
        .waitSemaphoreCount = previous ? 1u : 0u, .pWaitSemaphores = &timeline, .pWaitDstStageMask = &stage,
        .commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &timeline};
    VK(vkQueueSubmit(queue, 1, &si, fence));
    wait_checkpoint();
}

static void bind_named(Image *i, int tails, const char *name) {
    start(name);
    if (!(tails ? i->ntails : i->ntiles)) { puts("SKIP no bindings of this kind"); return; }
    uint64_t previous = serial, next = ++serial;
    VkTimelineSemaphoreSubmitInfo tl = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = previous ? 1u : 0u, .pWaitSemaphoreValues = &previous,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &next};
    VkSparseImageMemoryBindInfo ib = {.image = i->handle, .bindCount = i->ntiles, .pBinds = i->tiles};
    VkSparseImageOpaqueMemoryBindInfo ob = {.image = i->handle, .bindCount = i->ntails, .pBinds = i->tails};
    VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .pNext = &tl,
        .waitSemaphoreCount = previous ? 1u : 0u, .pWaitSemaphores = &timeline,
        .imageBindCount = tails ? 0u : 1u, .pImageBinds = &ib,
        .imageOpaqueBindCount = tails ? 1u : 0u, .pImageOpaqueBinds = &ob,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &timeline};
    VK(vkQueueBindSparse(queue, 1, &bi, fence));
    wait_checkpoint();
}
static void bind_image(Image *i, int tails) { bind_named(i, tails, tails ? "bind-mip-tails-metadata" : "bind-tiles"); }

static uint32_t dim(uint32_t w, uint32_t mip) { return w >> mip ? w >> mip : 1; }
static uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static VkDeviceSize aligned(VkDeviceSize n, VkDeviceSize a) { return (n + a - 1) / a * a; }

static VkDeviceSize bc7_image_bytes(const Image *i) {
    VkDeviceSize bytes = 0;
    // Tight BC7 payload for this square 2D array, including every requested mip and layer.
    // Sparse residency does not reduce this resource-size comparison to the device's limit.
    for (uint32_t mip = 0; mip < i->mips; mip++) {
        VkDeviceSize blocks = ((VkDeviceSize)dim(i->width, mip) + 3) / 4;
        bytes += blocks * blocks * 16;
    }
    return bytes * i->layers;
}

static void validate_requirements(Image *i) {
    start("requirements-consistency");
    // A second public query separates a malformed driver result from legacy-query handling.
    VkImageSparseMemoryRequirementsInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2, .image = i->handle};
    VkSparseImageMemoryRequirements2 check[8] = {0};
    uint32_t count = 8;
    for (uint32_t n = 0; n < 8; n++) check[n].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2;
    vkGetImageSparseMemoryRequirements2(device, &info, &count, check);
    conformance_require(count == i->nsparse, "legacy/requirements2 count differs");
    for (uint32_t n = 0; n < count; n++) {
        const VkSparseImageMemoryRequirements *a = &i->sparse[n], *b = &check[n].memoryRequirements;
        printf("REQUIREMENTS2 aspect=%#x granularity=%ux%ux%u flags=%#x tailFirstLod=%u offset=%#llx size=%llu stride=%llu\n",
            b->formatProperties.aspectMask, b->formatProperties.imageGranularity.width,
            b->formatProperties.imageGranularity.height, b->formatProperties.imageGranularity.depth,
            b->formatProperties.flags, b->imageMipTailFirstLod, (unsigned long long)b->imageMipTailOffset,
            (unsigned long long)b->imageMipTailSize, (unsigned long long)b->imageMipTailStride);
        conformance_require(a->formatProperties.aspectMask == b->formatProperties.aspectMask &&
            a->formatProperties.imageGranularity.width == b->formatProperties.imageGranularity.width &&
            a->formatProperties.imageGranularity.height == b->formatProperties.imageGranularity.height &&
            a->formatProperties.imageGranularity.depth == b->formatProperties.imageGranularity.depth &&
            a->formatProperties.flags == b->formatProperties.flags && a->imageMipTailFirstLod == b->imageMipTailFirstLod &&
            a->imageMipTailOffset == b->imageMipTailOffset && a->imageMipTailSize == b->imageMipTailSize &&
            ((a->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) || a->imageMipTailStride == b->imageMipTailStride),
            "legacy/requirements2 sparse properties differ");
        if (!a->imageMipTailSize || a->imageMipTailFirstLod >= i->mips) continue;
        uint32_t tails = a->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? 1 : i->layers;
        for (uint32_t layer = 0; layer < tails; layer++) {
            VkDeviceSize stride = tails == 1 ? 0 : a->imageMipTailStride;
            conformance_require(!layer || stride <= (UINT64_MAX - a->imageMipTailOffset) / layer, "mip-tail offset overflow");
            VkDeviceSize offset = a->imageMipTailOffset + layer * stride;
            if (offset >= i->req.size || a->imageMipTailSize > i->req.size - offset) {
                fprintf(stderr, "INVALID_REQUIREMENTS aspect=%#x layer=%u tailOffset=%#llx tailSize=%llu imageSize=%llu; "
                    "opaque bind would violate VUID-VkSparseMemoryBind-resourceOffset-01099/size-01100; no bind submitted\n",
                    a->formatProperties.aspectMask, layer, (unsigned long long)offset,
                    (unsigned long long)a->imageMipTailSize, (unsigned long long)i->req.size);
                conformance_require(0, "driver-reported mip-tail range is outside image memory requirements");
            }
            conformance_require(!(offset % i->req.alignment) && !(a->imageMipTailSize % i->req.alignment), "unaligned mip-tail range");
        }
    }
    pass();
}

static int tiled_eligible(const Image *i) {
    start("tiled-only-eligibility");
    uint32_t colors = 0;
    for (uint32_t n = 0; n < i->nsparse; n++) {
        const VkSparseImageMemoryRequirements *s = &i->sparse[n];
        printf("TILED_REQUIREMENT aspect=%#x requestedMips=%u tailFirstLod=%u tailSize=%llu tailOffset=%#llx\n",
            s->formatProperties.aspectMask, i->mips, s->imageMipTailFirstLod,
            (unsigned long long)s->imageMipTailSize, (unsigned long long)s->imageMipTailOffset);
        if (s->formatProperties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
            colors++;
            if (s->imageMipTailFirstLod >= i->mips) continue;
            printf("SKIP [%s/%s] requested mip %u needs opaque tail binding; no allocation or queue work\n",
                scenario, checkpoint, s->imageMipTailFirstLod);
            return 0;
        }
        if (s->formatProperties.aspectMask == VK_IMAGE_ASPECT_METADATA_BIT && !s->imageMipTailSize) continue;
        printf("SKIP [%s/%s] aspect %#x requires opaque metadata backing or is unsupported by tiled-only control\n",
            scenario, checkpoint, s->formatProperties.aspectMask);
        return 0;
    }
    conformance_require(colors == 1, "expected one color sparse requirement");
    puts("TILED_ONLY all requested mips use tiles; opaque bindings unnecessary; unused tail offsets are not bound");
    pass(); return 1;
}

static void plan(Image *i, int large) {
    const VkSparseImageMemoryRequirements *color = NULL;
    for (uint32_t n = 0; n < i->nsparse; n++) {
        VkSparseImageMemoryRequirements *s = &i->sparse[n];
        printf("SPARSE aspect=%#x granularity=%ux%ux%u flags=%#x tailFirstLod=%u offset=%llu size=%llu stride=%llu\n",
            s->formatProperties.aspectMask, s->formatProperties.imageGranularity.width,
            s->formatProperties.imageGranularity.height, s->formatProperties.imageGranularity.depth,
            s->formatProperties.flags, s->imageMipTailFirstLod, (unsigned long long)s->imageMipTailOffset,
            (unsigned long long)s->imageMipTailSize, (unsigned long long)s->imageMipTailStride);
        if (s->formatProperties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
            require(!color, "multiple color sparse requirements"); color = s;
        } else require(s->formatProperties.aspectMask == VK_IMAGE_ASPECT_METADATA_BIT, "unexpected sparse aspect");
        if (!s->imageMipTailSize || s->imageMipTailFirstLod >= i->mips) continue;
        uint32_t tails = s->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? 1 : i->layers;
        for (uint32_t layer = 0; layer < tails; layer++) {
            require(i->ntails < MAX_REGIONS, "too many tail binds");
            i->tails[i->ntails++] = (VkSparseMemoryBind){
                .resourceOffset = s->imageMipTailOffset + (tails == 1 ? 0 : layer * s->imageMipTailStride),
                .size = s->imageMipTailSize, .memoryOffset = i->backing,
                .flags = s->formatProperties.aspectMask == VK_IMAGE_ASPECT_METADATA_BIT ? VK_SPARSE_MEMORY_BIND_METADATA_BIT : 0};
            i->backing += aligned(s->imageMipTailSize, i->req.alignment);
        }
    }
    require(color != NULL, "no color sparse requirements");
    VkExtent3D g = color->formatProperties.imageGranularity;
    require(g.width && g.height && g.depth == 1 && !(g.width % 4) && !(g.height % 4), "unsupported granularity");
    // Use the public API's granularity verbatim, unlike the runtime's vendor compatibility conversion.
    for (uint32_t layer = 0; layer < i->layers; layer++) {
        for (uint32_t mip = 0; mip < i->mips; mip++) {
            uint32_t w = dim(i->width, mip), h = w;
            if (mip < color->imageMipTailFirstLod) {
                for (uint32_t y = 0; y < h; y += g.height) {
                    for (uint32_t x = 0; x < w; x += g.width) {
                        require(i->ntiles < MAX_BINDS, "too many tile binds");
                        i->tiles[i->ntiles++] = (VkSparseImageMemoryBind){
                            .subresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer},
                            .offset = {(int32_t)x, (int32_t)y, 0},
                            .extent = {min32(w - x, g.width), min32(h - y, g.height), 1}, .memoryOffset = i->backing};
                        i->backing += i->req.alignment;
                        if (large) break;
                    }
                    if (large) break;
                }
                // Large arrays reserve the application's shape, but touch a bounded patch of one resident tile.
                if (large) w = min32(w, min32(g.width, 32)), h = min32(h, min32(g.height, 32));
            }
            require(i->nregions < MAX_REGIONS, "too many transfer regions");
            i->regions[i->nregions++] = (VkBufferImageCopy){.bufferOffset = i->bytes,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1}, .imageExtent = {w, h, 1}};
            i->bytes += (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * 16;
        }
    }
    printf("PLAN tileBinds=%u opaqueBinds=%u backing=%llu transfer=%llu bytes; %s\n", i->ntiles, i->ntails,
        (unsigned long long)i->backing, (unsigned long long)i->bytes,
        large ? (i->ntails ? "one tile per non-tail mip/layer, complete mip tails; other tiles untouched" :
                            "one tile per mip/layer, no opaque backing; other tiles untouched") :
                "fully resident, all texels transferred");
}

static void transition(Image *i, VkImageLayout old, VkImageLayout next, const char *name) {
    start(name); begin();
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = old == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old, .newLayout = next, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = i->handle,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, i->mips, 0, i->layers}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    submit();
}

static void synthetic(Image *i, int pattern) {
    for (VkDeviceSize offset = 0; offset < i->bytes; offset += 16) {
        for (unsigned b = 0; b < 16; b++) {
            uint64_t x = (offset / 16 + 1) * 0x9e3779b97f4a7c15ull + b * 0x100000001b3ull;
            i->upload.map[offset + b] = pattern ? (uint8_t)((x ^ (x >> 23) ^ (x >> 47)) >> (b % 8)) : 0;
        }
        // BC7 mode 6: six zero bits then one. Remaining bits encode endpoints, p-bits and indices.
        i->upload.map[offset] = (i->upload.map[offset] & 0x80) | 0x40;
    }
    VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = i->upload.allocation.handle, .size = VK_WHOLE_SIZE};
    VK(vkFlushMappedMemoryRanges(device, 1, &r));
}

static void transfers(Image *i, int read, int pattern) {
    for (uint32_t mip = 0; mip < i->mips; mip++) {
        char name[80];
        snprintf(name, sizeof name, "%s-%s-mip-%u", pattern ? "pattern" : "initialize", read ? "readback" : "upload", mip);
        start(name); begin();
        VkBufferImageCopy regions[68]; uint32_t count = 0;
        for (uint32_t n = 0; n < i->nregions; n++)
            if (i->regions[n].imageSubresource.mipLevel == mip) regions[count++] = i->regions[n];
        if (read) {
            vkCmdCopyImageToBuffer(cmd, i->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i->readback.handle, count, regions);
            VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, NULL, 0, NULL);
        } else vkCmdCopyBufferToImage(cmd, i->upload.handle, i->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, regions);
        submit();
        if (!read) continue;
        VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = i->readback.allocation.handle, .size = VK_WHOLE_SIZE};
        VK(vkInvalidateMappedMemoryRanges(device, 1, &r));
        uint64_t compared = 0;
        for (uint32_t n = 0; n < count; n++) {
            VkBufferImageCopy *c = &regions[n];
            VkDeviceSize bytes = (VkDeviceSize)((c->imageExtent.width + 3) / 4) * ((c->imageExtent.height + 3) / 4) * 16;
            for (VkDeviceSize b = 0; b < bytes; b++) {
                VkDeviceSize offset = c->bufferOffset + b;
                if (i->readback.map[offset] != i->upload.map[offset]) {
                    fprintf(stderr, "MISMATCH mip=%u layer=%u byte=%llu got=%02x expected=%02x\n", mip,
                        c->imageSubresource.baseArrayLayer, (unsigned long long)b, i->readback.map[offset], i->upload.map[offset]);
                    failure_kind = "GPU_DATA_MISMATCH";
                    fail("synthetic BC7 byte comparison");
                }
            }
            compared += bytes;
        }
        printf("PASS [%s/%s-verify] layers=%u bytes=%llu exact\n", scenario, checkpoint, count, (unsigned long long)compared);
    }
    checkpoint = "transfers-complete"; // The per-mip name above has automatic storage duration.
}

static int run_image(uint32_t width, uint32_t layers, int large, int tiled_control) {
    scenario = large ? (tiled_control ? "8192x8192x68-large-tiled-control" : "8192x8192x68") :
        tiled_control ? "512x512x4-tiled-control" : "512x512x4";
    if (large && tiled_control && layers == 32) scenario = "8192x8192x32-resource-size-control";
    Image *i = calloc(1, sizeof *i);
    require(i != NULL, "host allocation");
    i->width = width; i->layers = layers;
    for (uint32_t w = width; w; w >>= 1) i->mips++;
    // Independent control: still BC7 + multiple layers/mips, without requiring mip-tail residency.
    // This is an explicit separate run, never an automatic fallback after a failed checkpoint.
    if (tiled_control) i->mips = large ? 5 : 2;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_BC7_UNORM_BLOCK, .extent = {width, width, 1},
        .mipLevels = i->mips, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    start("format-limits");
    VkImageFormatProperties fp;
    VkResult r = vkGetPhysicalDeviceImageFormatProperties(gpu, ci.format, ci.imageType, ci.tiling, ci.usage, ci.flags, &fp);
    printf("API vkGetPhysicalDeviceImageFormatProperties -> %d\n", r);
    if (r == VK_ERROR_FORMAT_NOT_SUPPORTED || (r == VK_SUCCESS &&
        (width > fp.maxExtent.width || width > fp.maxExtent.height || layers > fp.maxArrayLayers ||
         i->mips > fp.maxMipLevels || !(fp.sampleCounts & ci.samples)))) {
        printf("SKIP [%s] requested sparse BC7 shape unsupported\n", scenario); free(i); return 77;
    }
    result(r, "vkGetPhysicalDeviceImageFormatProperties");
    printf("IMAGE format=%d flags=%#x usage=%#x mips=%u maxResourceSize=%llu\n", ci.format, ci.flags, ci.usage,
           i->mips, (unsigned long long)fp.maxResourceSize);
    printf("LIMITS maxExtent=%ux%ux%u maxLayers=%u maxMips=%u sampleCounts=%#x\n", fp.maxExtent.width,
        fp.maxExtent.height, fp.maxExtent.depth, fp.maxArrayLayers, fp.maxMipLevels, fp.sampleCounts);
    pass(); start("resource-size-preflight");
    VkDeviceSize theoretical = bc7_image_bytes(i);
    printf("RESOURCE_SIZE maxResourceSize=%llu theoreticalBC7=%llu vulkanRequirement=not-yet-queried bytes\n",
        (unsigned long long)fp.maxResourceSize, (unsigned long long)theoretical);
    if (theoretical > fp.maxResourceSize || theoretical > props.limits.sparseAddressSpaceSize) {
        printf("SKIP [%s/%s] theoretical BC7 resource exceeds declared resource/address-space limit; "
               "image not created, no allocation or queue work\n", scenario, checkpoint);
        free(i); return 77;
    }
    pass(); start("create-image");
    VK(vkCreateImage(device, &ci, NULL, &i->handle)); pass();
    start("memory-requirements");
    vkGetImageMemoryRequirements(device, i->handle, &i->req);
    vkGetImageSparseMemoryRequirements(device, i->handle, &i->nsparse, NULL);
    conformance_require(i->nsparse && i->nsparse <= 8 && i->req.alignment && i->req.memoryTypeBits, "invalid memory requirements");
    vkGetImageSparseMemoryRequirements(device, i->handle, &i->nsparse, i->sparse);
    printf("REQUIREMENTS virtual=%llu alignment=%llu types=%#x sparseCount=%u addressSpaceLimit=%llu\n",
        (unsigned long long)i->req.size, (unsigned long long)i->req.alignment, i->req.memoryTypeBits, i->nsparse,
        (unsigned long long)props.limits.sparseAddressSpaceSize);
    pass();
    start("resource-size-requirements");
    printf("RESOURCE_SIZE maxResourceSize=%llu theoreticalBC7=%llu vulkanRequirement=%llu bytes\n",
        (unsigned long long)fp.maxResourceSize, (unsigned long long)theoretical, (unsigned long long)i->req.size);
    if (i->req.size > fp.maxResourceSize || i->req.size > props.limits.sparseAddressSpaceSize) {
        printf("SKIP [%s/%s] Vulkan memory requirements exceed declared resource/address-space limit; "
               "no allocation or queue work\n", scenario, checkpoint);
        vkDestroyImage(device, i->handle, NULL); free(i); return 77;
    }
    pass();
    if (large && tiled_control && !tiled_eligible(i)) {
        vkDestroyImage(device, i->handle, NULL); free(i); return 77;
    }
    validate_requirements(i);
    start("plan-residency"); plan(i, large); pass();
    if (large && tiled_control) conformance_require(!i->ntails, "tiled-only plan unexpectedly contains opaque binds");
    start("allocation-budget-preflight");
    printf("BUDGET plannedBacking=%llu plannedStagingEach=%llu plannedTotal=%llu cap=%llu bytes; "
           "heap/free-budget checks also enforced before every allocation\n", (unsigned long long)i->backing,
           (unsigned long long)i->bytes, (unsigned long long)(i->backing + 2 * i->bytes), (unsigned long long)CAP);
    // Conservative preflight includes alignment slack for the two staging buffers and backing buffer.
    if (i->backing > CAP / 2 || i->bytes > (CAP - i->backing) / 2 - MIB) {
        puts("SKIP residency/transfers: planned memory exceeds conservative 128 MiB budget");
        vkDestroyImage(device, i->handle, NULL); free(i); return 77;
    }
    pass();
    start("allocate-backing");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = i->backing,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK(vkCreateBuffer(device, &bi, NULL, &i->backing_buffer));
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(device, i->backing_buffer, &br);
    uint32_t common = br.memoryTypeBits & i->req.memoryTypeBits;
    printf("BACKING bufferTypes=%#x imageTypes=%#x common=%#x bufferRequirement=%llu\n", br.memoryTypeBits,
        i->req.memoryTypeBits, common, (unsigned long long)br.size);
    if (!common) { vkDestroyBuffer(device, i->backing_buffer, NULL); i->backing_buffer = VK_NULL_HANDLE; }
    i->allocation = allocate(common ? br.size : i->backing,
        memory_type(common ? common : i->req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    if (common) VK(vkBindBufferMemory(device, i->backing_buffer, i->allocation.handle, 0));
    for (uint32_t n = 0; n < i->ntiles; n++) i->tiles[n].memory = i->allocation.handle;
    for (uint32_t n = 0; n < i->ntails; n++) i->tails[n].memory = i->allocation.handle;
    i->upload = buffer(i->bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    i->readback = buffer(i->bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memset(i->readback.map, 0xcd, (size_t)i->bytes);
    VkMappedMemoryRange host = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = i->readback.allocation.handle, .size = VK_WHOLE_SIZE};
    VK(vkFlushMappedMemoryRanges(device, 1, &host));
    memory_log(); pass();
    start("initialize-backing-buffer");
    if (common) {
        begin(); vkCmdFillBuffer(cmd, i->backing_buffer, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        submit();
    } else puts("SKIP backing-buffer initialization: disjoint memory types; image initialized by upload");
    bind_image(i, 0);
    if (large && tiled_control) puts("TILED_ONLY imageOpaqueBindCount=0 throughout this run");
    else bind_image(i, 1);
    transition(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "layout-undefined-to-dst");
    start("synthetic-initial-data"); synthetic(i, 0); pass();
    transfers(i, 0, 0);
    transition(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "layout-initial-dst-to-src");
    transfers(i, 1, 0);
    transition(i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, "layout-src-to-general");
    transition(i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "layout-general-to-dst");
    start("synthetic-pattern"); synthetic(i, 1); pass();
    transfers(i, 0, 1);
    transition(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "layout-pattern-dst-to-src");
    transfers(i, 1, 1);
    transition(i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "layout-src-to-sampled");
    start("release-completed-resources");
    vkDestroyImage(device, i->handle, NULL);
    if (i->backing_buffer) vkDestroyBuffer(device, i->backing_buffer, NULL);
    release_allocation(i->allocation); release_buffer(i->upload); release_buffer(i->readback);
    free(i); memory_log(); pass(); return 0;
}

// --wrap-discriminator (G5-L audit): tests one hypothesis for the G5-J device loss. Mesa NVK 26.3.0-rc3 computes
// each copy-engine layer address as (z + baseArrayLayer) * array_stride with every operand 32-bit
// (src/nouveau/vulkan/nvk_cmd_copy.c: `uint32_t array_stride` in struct nouveau_copy_buffer, nouveau_copy_rect),
// while its sparse binds use 64-bit offsets (nil level_layer_offset_B). If that is what runs, a copy into the first
// layer whose offset reaches 2^32 writes at (layer * stride) mod 2^32 inside the image, not into that layer's tile.
// G5-J's fault 0x325140000 = 0x320000000 + (49 * 0x5540000 mod 2^32) fits this, but its base VA was never logged.
// The probe binds the control tile (last layer below 4 GiB), the wrap tile (first layer at/after 4 GiB) and every
// tile of the level holding the predicted wrapped address, copies a distinct 32x32 marker into each of the two
// layers, and finds the markers by copying the backing memory through a buffer: no image addressing is involved in
// the oracle. If the prediction holds, nothing is written to an unbound tile, so the run separates the wrap from a
// fault on unbound pages (--unbound-access). The image intentionally exceeds maxResourceSize, which NVK reports as
// UINT32_MAX with a TODO (nvk_image.c); G5-J created the same shape.
#define WRAP_PATCH 32              // texels a side: 8x8 BC7 blocks, inside one tile
#define WRAP_BLOCKS 64
#define WRAP_MIPS 5
typedef struct { uint32_t layer, mip, tx, ty; } WrapSlot;   // what backing slot n (n * alignment) is bound to
static WrapSlot wrap_slots[MAX_BINDS];
typedef struct { uint32_t found, slot_first, slots; VkDeviceSize first; } WrapFound;

static void wrap_tile(Image *i, uint32_t layer, uint32_t mip, uint32_t tx, uint32_t ty, VkExtent3D g) {
    require(i->ntiles < MAX_BINDS, "too many tile binds");
    uint32_t w = dim(i->width, mip);
    wrap_slots[i->ntiles] = (WrapSlot){layer, mip, tx, ty};
    i->tiles[i->ntiles++] = (VkSparseImageMemoryBind){.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer},
        .offset = {(int32_t)(tx * g.width), (int32_t)(ty * g.height), 0},
        .extent = {min32(w - tx * g.width, g.width), min32(w - ty * g.height, g.height), 1}, .memoryOffset = i->backing};
    i->backing += i->req.alignment;
}

// 16 bytes, BC7 mode 6 (0x40), tagged "G5L", the region (0 control, 1 wrap), the block and the destination layer.
static void wrap_marker(uint8_t *p, uint32_t region, uint32_t layer, uint32_t block) {
    p[0] = 0x40, p[1] = 'G', p[2] = '5', p[3] = 'L', p[4] = (uint8_t)region, p[5] = (uint8_t)block;
    p[6] = (uint8_t)layer, p[7] = (uint8_t)(layer >> 8);
    for (unsigned b = 8; b < 16; b++) p[b] = (uint8_t)(0xa5 ^ (region * 16 + b) ^ block);
}

static int wrap_is_marker(const uint8_t *p) { return p[0] == 0x40 && p[1] == 'G' && p[2] == '5' && p[3] == 'L'; }

static WrapFound wrap_locate(const Image *i, uint32_t region) {
    WrapFound f = {.slot_first = UINT32_MAX};
    uint32_t last = UINT32_MAX;
    for (VkDeviceSize o = 0; o + 16 <= i->backing; o += 16) {
        const uint8_t *p = i->readback.map + o;
        if (!wrap_is_marker(p) || p[4] != region) continue;
        uint32_t s = (uint32_t)(o / i->req.alignment);
        if (!f.found) f.first = o, f.slot_first = s;
        if (s != last) f.slots++, last = s;
        f.found++;
    }
    return f;
}

static uint64_t wrap_stray(const Image *i) {   // nonzero 16-byte blocks that are not markers
    uint64_t n = 0;
    for (VkDeviceSize o = 0; o + 16 <= i->backing; o += 16) {
        const uint8_t *p = i->readback.map + o;
        if (wrap_is_marker(p)) continue;
        for (unsigned b = 0; b < 16; b++) if (p[b]) { n++; break; }
    }
    return n;
}

static void wrap_report(const Image *i, const char *what, WrapFound f) {
    if (!f.found) {
        printf("LOCATE %s markers=0 of %u: in no bound tile's memory\n", what, WRAP_BLOCKS);
        return;
    }
    const WrapSlot *s = &wrap_slots[f.slot_first];
    printf("LOCATE %s markers=%u of %u in %u slot(s); first at backing+%#llx = slot %u (layer %u mip %u tile %u,%u) +%#llx\n",
        what, f.found, WRAP_BLOCKS, f.slots, (unsigned long long)f.first, f.slot_first, s->layer, s->mip, s->tx, s->ty,
        (unsigned long long)(f.first % i->req.alignment));
}

// The whole backing memory to the host through the buffer bound to it, after every earlier queue operation.
static void wrap_readback(Image *i, const char *name) {
    start(name); begin();
    VkMemoryBarrier pre = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &pre, 0, NULL, 0, NULL);
    VkBufferCopy c = {0, 0, i->backing};
    vkCmdCopyBuffer(cmd, i->backing_buffer, i->readback.handle, 1, &c);
    VkMemoryBarrier post = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &post, 0, NULL, 0, NULL);
    submit();
    VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = i->readback.allocation.handle, .size = VK_WHOLE_SIZE};
    VK(vkInvalidateMappedMemoryRanges(device, 1, &r));
}

static void wrap_upload(Image *i, uint32_t region, const char *name) {
    start(name); begin();
    vkCmdCopyBufferToImage(cmd, i->upload.handle, i->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &i->regions[region]);
    submit();
}

static int wrap_skip(Image *i, const char *why) {
    printf("SKIP [%s/%s] %s; no allocation or queue work\n", scenario, checkpoint, why);
    if (i->handle) vkDestroyImage(device, i->handle, NULL);
    free(i); return 77;
}

// Backing memory for the tile binds, also bound to a buffer so the host reads it with no image addressing, plus the
// upload (i->bytes) and readback (i->backing) buffers. Returns 0, having created nothing, when no memory type serves
// both the image and a buffer.
static int oracle_backing(Image *i) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = i->backing,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK(vkCreateBuffer(device, &bi, NULL, &i->backing_buffer));
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(device, i->backing_buffer, &br);
    uint32_t common = br.memoryTypeBits & i->req.memoryTypeBits;
    printf("BACKING bufferTypes=%#x imageTypes=%#x common=%#x bufferRequirement=%llu\n", br.memoryTypeBits,
        i->req.memoryTypeBits, common, (unsigned long long)br.size);
    if (!common) {
        vkDestroyBuffer(device, i->backing_buffer, NULL);
        i->backing_buffer = VK_NULL_HANDLE;
        return 0;
    }
    i->allocation = allocate(br.size, memory_type(common, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    VK(vkBindBufferMemory(device, i->backing_buffer, i->allocation.handle, 0));
    for (uint32_t n = 0; n < i->ntiles; n++) i->tiles[n].memory = i->allocation.handle;
    i->upload = buffer(i->bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    i->readback = buffer(i->backing, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    return 1;
}

static void oracle_fill(Image *i) {   // zeroes the backing before any bind, so every later nonzero byte was written
    start("initialize-backing-buffer");
    begin(); vkCmdFillBuffer(cmd, i->backing_buffer, 0, VK_WHOLE_SIZE, 0);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    submit();
}

static int run_wrap_discriminator(void) {
    scenario = "8192x8192x68-wrap-discriminator";
    Image *i = calloc(1, sizeof *i);
    require(i != NULL, "host allocation");
    i->width = 8192; i->layers = 68; i->mips = WRAP_MIPS;   // G5-J's shape: every mip before the mip tail
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_BC7_UNORM_BLOCK, .extent = {i->width, i->width, 1},
        .mipLevels = i->mips, .arrayLayers = i->layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    start("format-limits");
    VkImageFormatProperties fp;
    VkResult r = vkGetPhysicalDeviceImageFormatProperties(gpu, ci.format, ci.imageType, ci.tiling, ci.usage, ci.flags, &fp);
    printf("API vkGetPhysicalDeviceImageFormatProperties -> %d\n", r);
    if (r == VK_ERROR_FORMAT_NOT_SUPPORTED || (r == VK_SUCCESS && (i->width > fp.maxExtent.width ||
        i->width > fp.maxExtent.height || i->layers > fp.maxArrayLayers || i->mips > fp.maxMipLevels)))
        return wrap_skip(i, "requested sparse BC7 shape unsupported");
    result(r, "vkGetPhysicalDeviceImageFormatProperties");
    printf("IMAGE format=%d flags=%#x usage=%#x mips=%u maxResourceSize=%llu\n", ci.format, ci.flags, ci.usage,
           i->mips, (unsigned long long)fp.maxResourceSize);
    pass(); start("resource-size-preflight");
    VkDeviceSize theoretical = bc7_image_bytes(i);
    printf("RESOURCE_SIZE maxResourceSize=%llu theoreticalBC7=%llu sparseAddressSpace=%llu bytes\n",
        (unsigned long long)fp.maxResourceSize, (unsigned long long)theoretical,
        (unsigned long long)props.limits.sparseAddressSpaceSize);
    if (theoretical > props.limits.sparseAddressSpaceSize) return wrap_skip(i, "theoretical size exceeds the sparse address space");
    if (theoretical > fp.maxResourceSize)
        printf("INTENTIONAL_OVER_LIMIT theoretical size exceeds maxResourceSize by %llu bytes: this mode tests whether "
               "that limit stands for a 32-bit copy offset; GPU work past the limit is two %u-byte copies\n",
               (unsigned long long)(theoretical - fp.maxResourceSize), WRAP_BLOCKS * 16);
    const char *dbg = getenv("NVK_DEBUG");
    printf("NVK_DEBUG=%s%s\n", dbg ? dbg : "(unset)",
           dbg && strstr(dbg, "vm") ? "" : ": set NVK_DEBUG=vm to log the image's base VA (\"alloc va [..) sparse\")");
    pass();
    start("create-image");
    VK(vkCreateImage(device, &ci, NULL, &i->handle)); pass();
    start("memory-requirements");
    vkGetImageMemoryRequirements(device, i->handle, &i->req);
    vkGetImageSparseMemoryRequirements(device, i->handle, &i->nsparse, NULL);
    conformance_require(i->nsparse && i->nsparse <= 8 && i->req.alignment && i->req.memoryTypeBits, "invalid memory requirements");
    vkGetImageSparseMemoryRequirements(device, i->handle, &i->nsparse, i->sparse);
    printf("REQUIREMENTS virtual=%llu alignment=%llu types=%#x sparseCount=%u\n", (unsigned long long)i->req.size,
        (unsigned long long)i->req.alignment, i->req.memoryTypeBits, i->nsparse);
    if (i->req.size > props.limits.sparseAddressSpaceSize) return wrap_skip(i, "requirements exceed the sparse address space");
    pass();
    if (!tiled_eligible(i)) { vkDestroyImage(device, i->handle, NULL); free(i); return 77; }
    validate_requirements(i);

    // The prediction needs the layer layout: a layer is its levels in whole tiles, back to back, and the layer stride
    // is the requirements' size over the layers (NIL's layout for these mips, the one G5-J's size matched). Checked,
    // not assumed: a mismatch skips before any GPU work.
    start("layout-assumption");
    const VkSparseImageMemoryRequirements *color = NULL;
    for (uint32_t n = 0; n < i->nsparse; n++)
        if (i->sparse[n].formatProperties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) color = &i->sparse[n];
    require(color != NULL, "no color sparse requirements");
    VkExtent3D g = color->formatProperties.imageGranularity;
    require(g.width && g.height && g.depth == 1, "unsupported granularity");
    VkDeviceSize tile = i->req.alignment, stride = i->req.size / i->layers, level[WRAP_MIPS], sum = 0;
    uint32_t tiles_x[WRAP_MIPS], tiles_y[WRAP_MIPS];
    for (uint32_t mip = 0; mip < i->mips; mip++) {
        uint32_t w = dim(i->width, mip);
        tiles_x[mip] = (w + g.width - 1) / g.width, tiles_y[mip] = (w + g.height - 1) / g.height;
        level[mip] = sum, sum += (VkDeviceSize)tiles_x[mip] * tiles_y[mip] * tile;
    }
    printf("LAYOUT stride=%#llx levels=%#llx,%#llx,%#llx,%#llx,%#llx tile=%llu granularity=%ux%u layerSum=%#llx\n",
        (unsigned long long)stride, (unsigned long long)level[0], (unsigned long long)level[1],
        (unsigned long long)level[2], (unsigned long long)level[3], (unsigned long long)level[4],
        (unsigned long long)tile, g.width, g.height, (unsigned long long)sum);
    if (i->req.size % i->layers || sum != stride)
        return wrap_skip(i, "layer stride is not the levels' whole tiles back to back: the wrapped address cannot be predicted");
    uint32_t wrap = UINT32_MAX;
    for (uint32_t l = 0; l < i->layers && wrap == UINT32_MAX; l++) if ((uint64_t)l * stride >= (1ull << 32)) wrap = l;
    if (wrap == UINT32_MAX) return wrap_skip(i, "no layer starts at or past 4 GiB");
    uint32_t control = wrap - 1;
    VkDeviceSize predicted = (uint32_t)((uint64_t)wrap * stride);   // the offset 32-bit arithmetic gives
    uint32_t target = (uint32_t)(predicted / stride), tmip = 0;
    VkDeviceSize rel = predicted % stride;
    while (tmip + 1 < i->mips && rel >= level[tmip + 1]) tmip++;
    VkDeviceSize k = (rel - level[tmip]) / tile;
    printf("PREDICT wrapLayer=%u offset=%#llx controlLayer=%u offset=%#llx; 32-bit offset of the wrap layer %#llx = "
           "layer %u mip %u tile %llu,%llu +%#llx\n", wrap, (unsigned long long)((uint64_t)wrap * stride), control,
           (unsigned long long)((uint64_t)control * stride), (unsigned long long)predicted, target, tmip,
           (unsigned long long)(k % tiles_x[tmip]), (unsigned long long)(k / tiles_x[tmip]),
           (unsigned long long)((rel - level[tmip]) % tile));
    printf("PREDICT with NVK_DEBUG=vm base B of this image: wrapped write at B+%#llx, unwrapped at B+%#llx\n",
           (unsigned long long)predicted, (unsigned long long)((uint64_t)wrap * stride));
    if (target == control || target == wrap) return wrap_skip(i, "the predicted target is the control or wrap layer");
    pass();

    start("plan-residency");
    // The whole predicted level of the target layer, whatever the order of tiles inside it: the wrapped write lands in
    // bound memory. The whole layer would be 1364 tiles (85 MiB) for G5-J's shape, past the cap with its readback.
    for (uint32_t ty = 0; ty < tiles_y[tmip]; ty++)
        for (uint32_t tx = 0; tx < tiles_x[tmip]; tx++) wrap_tile(i, target, tmip, tx, ty, g);
    uint32_t control_slot = i->ntiles, wrap_slot = i->ntiles + 1;
    wrap_tile(i, control, 0, 0, 0, g);
    wrap_tile(i, wrap, 0, 0, 0, g);
    uint32_t pw = min32(WRAP_PATCH, g.width), ph = min32(WRAP_PATCH, g.height);
    uint32_t blocks = (pw / 4) * (ph / 4);
    require(blocks == WRAP_BLOCKS, "marker patch is not 8x8 BC7 blocks");
    for (uint32_t n = 0; n < 2; n++)
        i->regions[n] = (VkBufferImageCopy){.bufferOffset = (VkDeviceSize)n * WRAP_BLOCKS * 16,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, n ? wrap : control, 1}, .imageExtent = {pw, ph, 1}};
    i->nregions = 2, i->bytes = 2 * WRAP_BLOCKS * 16;
    printf("PLAN tileBinds=%u (layer %u mip %u whole, control slot %u, wrap slot %u) opaqueBinds=0 backing=%llu "
           "transfer=%llu readback=%llu bytes\n", i->ntiles, target, tmip, control_slot, wrap_slot, (unsigned long long)i->backing,
           (unsigned long long)i->bytes, (unsigned long long)i->backing);
    if (2 * i->backing + i->bytes > CAP - 2 * MIB) return wrap_skip(i, "planned memory exceeds the 128 MiB cap");
    pass();

    start("allocate-backing");
    if (!oracle_backing(i))
        return wrap_skip(i, "no memory type serves both the image and a buffer: the oracle reads the backing through one");
    for (uint32_t n = 0; n < 2; n++)
        for (uint32_t b = 0; b < WRAP_BLOCKS; b++)
            wrap_marker(i->upload.map + (n * WRAP_BLOCKS + b) * 16, n, n ? wrap : control, b);
    VkMappedMemoryRange up = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = i->upload.allocation.handle, .size = VK_WHOLE_SIZE};
    VK(vkFlushMappedMemoryRanges(device, 1, &up));
    memory_log(); pass();

    oracle_fill(i);
    bind_image(i, 0);
    puts("TILED_ONLY imageOpaqueBindCount=0 throughout this run");
    transition(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "layout-undefined-to-dst");
    wrap_readback(i, "readback-baseline");
    printf("BASELINE nonzero 16-byte blocks=%llu (expected 0 after the fill)\n", (unsigned long long)wrap_stray(i));

    // Control: the last layer whose offset fits 32 bits. Its marker must land in its own tile, or the oracle (layout
    // or slot mapping) is wrong and the wrap copy could not be classified: stop before it.
    wrap_upload(i, 0, "upload-control-layer");
    wrap_readback(i, "readback-after-control");
    start("locate-control");
    WrapFound c = wrap_locate(i, 0);
    wrap_report(i, "control", c);
    if (c.found != WRAP_BLOCKS || c.slots != 1 || c.slot_first != control_slot) {
        failure_kind = "ORACLE";
        fail("control marker not exactly in the control layer's tile: the wrap copy could not be classified");
    }
    pass();

    printf("PREDICT next: 32-bit layer offset -> wrap markers in layer %u's memory (mip %u), no fault; no wrap -> in "
           "slot %u (layer %u); device loss -> the copy reached memory outside every bound tile\n", target, tmip,
           wrap_slot, wrap);
    wrap_upload(i, 1, "upload-wrap-layer");
    wrap_readback(i, "readback-after-wrap");
    start("verdict");
    WrapFound w = wrap_locate(i, 1);
    c = wrap_locate(i, 0);
    wrap_report(i, "control", c);
    wrap_report(i, "wrap", w);
    printf("STRAY nonzero 16-byte blocks outside markers=%llu\n", (unsigned long long)wrap_stray(i));
    const WrapSlot *ws = w.found ? &wrap_slots[w.slot_first] : NULL;
    if (w.found == WRAP_BLOCKS && w.slots == 1 && ws->layer == target)
        printf("VERDICT WRAPPED_LAYER_OFFSET copy to layer %u wrote layer %u mip %u tile %u,%u (predicted mip %u tile "
               "%llu,%llu: %s), the 32-bit offset %#llx\n", wrap, ws->layer, ws->mip, ws->tx, ws->ty, tmip,
               (unsigned long long)(k % tiles_x[tmip]), (unsigned long long)(k / tiles_x[tmip]),
               ws->mip == tmip && ws->tx == k % tiles_x[tmip] && ws->ty == k / tiles_x[tmip] ? "match" : "differs",
               (unsigned long long)predicted);
    else if (w.found == WRAP_BLOCKS && w.slots == 1 && w.slot_first == wrap_slot)
        printf("VERDICT NO_WRAP copy to layer %u landed in its own tile: the 32-bit offset hypothesis is refuted here\n", wrap);
    else if (!w.found)
        printf("VERDICT NOT_FOUND copy to layer %u reached no bound tile and did not fault\n", wrap);
    else
        printf("VERDICT UNEXPECTED %u wrap markers in %u slot(s)\n", w.found, w.slots);
    if (c.found != WRAP_BLOCKS || c.slot_first != control_slot) puts("NOTE the control marker changed after the wrap copy");
    pass();

    transition(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "layout-dst-to-sampled");
    start("release-completed-resources");
    vkDestroyImage(device, i->handle, NULL);
    vkDestroyBuffer(device, i->backing_buffer, NULL);
    release_allocation(i->allocation); release_buffer(i->upload); release_buffer(i->readback);
    free(i); memory_log(); pass(); return 0;
}

// --unbound-access (G5-L, hypothesis H2): does a copy that touches an unbound tile of a partially resident image fault?
// Vulkan allows such accesses on SPARSE_RESIDENCY images: with residencyNonResidentStrict they read as zero and writes
// are discarded, without it they are "considered safe" and reads return poison (sparsemem.html, "Sparse Resource
// Features"), so a device loss is out of spec either way. In G5-J the 32-bit wrapped write reached an
// unbound tile and the GPU raised FAULT_PTE although NVK reports the property, which points at the page tables of
// NullMoth's private nvkmd backend (upstream nouveau maps them with DRM_NOUVEAU_VM_BIND_SPARSE). Here the image is
// 512x512, one mip, four tiles: no 4 GiB offset, no mip tail, nothing near maxResourceSize. Tile (0,0) is bound and
// written as a control; then, each in its own submission so a device loss's checkpoint names it: a read and a write
// of never-bound tile (1,1), the same tile read again, tile (0,0) unbound, then read and written. The backing is read
// through a buffer as in --wrap-discriminator. Copy engine only: this probe has no shaders, so sampling an unbound
// tile (what the game's terrain does) goes through the same page tables but is not exercised.
#define UNBOUND_PATCH_BYTES (WRAP_BLOCKS * 16)

static void unbound_write(Image *i, VkExtent3D g, uint32_t tx, uint32_t ty, uint32_t marker, const char *name) {
    start(name); begin();
    VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)marker * UNBOUND_PATCH_BYTES,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {(int32_t)(tx * g.width), (int32_t)(ty * g.height), 0}, .imageExtent = {WRAP_PATCH, WRAP_PATCH, 1}};
    vkCmdCopyBufferToImage(cmd, i->upload.handle, i->handle, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    submit();
}

// ZERO (what strict residency requires of an unbound tile), MARKER (data some tile's memory holds), UNWRITTEN (the copy
// left the host's 0xcc fill) or OTHER.
static const char *unbound_read(Image *i, Buffer *out, VkExtent3D g, uint32_t tx, uint32_t ty, const char *name) {
    memset(out->map, 0xcc, UNBOUND_PATCH_BYTES);
    VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = out->allocation.handle,
        .size = VK_WHOLE_SIZE};
    VK(vkFlushMappedMemoryRanges(device, 1, &r));
    start(name); begin();
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {(int32_t)(tx * g.width), (int32_t)(ty * g.height), 0}, .imageExtent = {WRAP_PATCH, WRAP_PATCH, 1}};
    vkCmdCopyImageToBuffer(cmd, i->handle, VK_IMAGE_LAYOUT_GENERAL, out->handle, 1, &c);
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, NULL, 0, NULL);
    submit();
    VK(vkInvalidateMappedMemoryRanges(device, 1, &r));
    uint32_t zero = 0, fill = 0, markers = 0;
    for (uint32_t n = 0; n < UNBOUND_PATCH_BYTES; n++) zero += !out->map[n], fill += out->map[n] == 0xcc;
    for (uint32_t n = 0; n < UNBOUND_PATCH_BYTES; n += 16) markers += wrap_is_marker(out->map + n) ? 1u : 0u;
    const char *what = zero == UNBOUND_PATCH_BYTES ? "ZERO" : markers ? "MARKER" :
                       fill == UNBOUND_PATCH_BYTES ? "UNWRITTEN" : "OTHER";
    printf("READ [%s] tile %u,%u: zeroBytes=%u untouchedBytes=%u markerBlocks=%u of %u -> %s\n", name, tx, ty, zero,
        fill, markers, WRAP_BLOCKS, what);
    return what;
}

static int run_unbound_access(void) {
    scenario = "512x512-unbound-access";
    Image *i = calloc(1, sizeof *i);
    require(i != NULL, "host allocation");
    i->width = 512; i->layers = 1; i->mips = 1;
    VkPhysicalDeviceSparseProperties sp = props.sparseProperties;
    printf("SPARSE_PROPERTIES residencyNonResidentStrict=%u residencyStandard2DBlockShape=%u residencyAlignedMipSize=%u\n",
        sp.residencyNonResidentStrict, sp.residencyStandard2DBlockShape, sp.residencyAlignedMipSize);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_BC7_UNORM_BLOCK, .extent = {i->width, i->width, 1},
        .mipLevels = i->mips, .arrayLayers = i->layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    start("create-image");
    VK(vkCreateImage(device, &ci, NULL, &i->handle)); pass();
    start("memory-requirements");
    vkGetImageMemoryRequirements(device, i->handle, &i->req);
    vkGetImageSparseMemoryRequirements(device, i->handle, &i->nsparse, NULL);
    conformance_require(i->nsparse && i->nsparse <= 8 && i->req.alignment && i->req.memoryTypeBits, "invalid memory requirements");
    vkGetImageSparseMemoryRequirements(device, i->handle, &i->nsparse, i->sparse);
    printf("REQUIREMENTS virtual=%llu alignment=%llu types=%#x sparseCount=%u\n", (unsigned long long)i->req.size,
        (unsigned long long)i->req.alignment, i->req.memoryTypeBits, i->nsparse);
    pass();
    if (!tiled_eligible(i)) { vkDestroyImage(device, i->handle, NULL); free(i); return 77; }
    validate_requirements(i);

    start("plan-residency");
    const VkSparseImageMemoryRequirements *color = NULL;
    for (uint32_t n = 0; n < i->nsparse; n++)
        if (i->sparse[n].formatProperties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) color = &i->sparse[n];
    require(color != NULL, "no color sparse requirements");
    VkExtent3D g = color->formatProperties.imageGranularity;
    require(g.width && g.height && g.depth == 1, "unsupported granularity");
    if (2 * g.width > i->width || 2 * g.height > i->width || g.width < WRAP_PATCH || g.height < WRAP_PATCH)
        return wrap_skip(i, "the image is not at least 2x2 tiles of 32x32 texels or more");
    wrap_tile(i, 0, 0, 0, 0, g);   // slot 0: tile (0,0); (1,0), (0,1) and (1,1) are never bound
    i->bytes = 3 * UNBOUND_PATCH_BYTES;   // markers: 0 control at (0,0), 1 to never-bound (1,1), 2 to (0,0) once unbound
    printf("PLAN tileBinds=1 (tile 0,0) neverBound=3 opaqueBinds=0 backing=%llu transfer=%llu bytes\n",
        (unsigned long long)i->backing, (unsigned long long)i->bytes);
    pass();

    start("allocate-backing");
    if (!oracle_backing(i))
        return wrap_skip(i, "no memory type serves both the image and a buffer: the oracle reads the backing through one");
    for (uint32_t n = 0; n < 3; n++)
        for (uint32_t b = 0; b < WRAP_BLOCKS; b++) wrap_marker(i->upload.map + (n * WRAP_BLOCKS + b) * 16, n, 0, b);
    VkMappedMemoryRange up = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = i->upload.allocation.handle, .size = VK_WHOLE_SIZE};
    VK(vkFlushMappedMemoryRanges(device, 1, &up));
    Buffer out = buffer(UNBOUND_PATCH_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memory_log(); pass();

    oracle_fill(i);
    bind_image(i, 0);
    transition(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, "layout-undefined-to-general");

    // Controls: a bound tile is written and read back correctly, through both the backing and the image.
    unbound_write(i, g, 0, 0, 0, "write-bound-control");
    wrap_readback(i, "readback-after-control");
    start("locate-control");
    WrapFound c = wrap_locate(i, 0);
    wrap_report(i, "control", c);
    if (c.found != WRAP_BLOCKS || c.slots != 1 || c.slot_first != 0) {
        failure_kind = "ORACLE";
        fail("control marker not exactly in the bound tile");
    }
    pass();
    if (strcmp(unbound_read(i, &out, g, 0, 0, "read-bound-control"), "MARKER")) {
        failure_kind = "ORACLE";
        fail("the bound tile did not read back its marker");
    }

    puts("PREDICT next: strict residency -> ZERO, no fault; a device loss at read-never-bound or write-never-bound -> H2 "
         "(no sparse page-table entries for unbound pages)");
    const char *never_read = unbound_read(i, &out, g, 1, 1, "read-never-bound");
    unbound_write(i, g, 1, 1, 1, "write-never-bound");
    wrap_readback(i, "readback-after-never-bound-write");
    start("locate-never-bound-write");
    WrapFound leak = wrap_locate(i, 1);
    wrap_report(i, "never-bound write", leak);
    c = wrap_locate(i, 0);
    pass();
    const char *never_reread = unbound_read(i, &out, g, 1, 1, "reread-never-bound");

    // Unbinding is what the renderer does when NVN unmaps a pool page: the old memory must stop being reachable.
    i->tiles[0].memory = VK_NULL_HANDLE;
    i->tiles[0].memoryOffset = 0;
    bind_named(i, 0, "unbind-control-tile");
    puts("PREDICT next: strict residency -> ZERO (not the control marker) and the write discarded; a device loss at "
         "read-unbound or write-unbound -> H2 for unbound-after-bound pages");
    const char *unbound_reread = unbound_read(i, &out, g, 0, 0, "read-unbound");
    unbound_write(i, g, 0, 0, 2, "write-unbound");
    wrap_readback(i, "readback-after-unbound-write");
    start("verdict");
    WrapFound stale = wrap_locate(i, 2), after = wrap_locate(i, 0);
    wrap_report(i, "unbound write", stale);
    wrap_report(i, "control after unbind", after);
    printf("STRAY nonzero 16-byte blocks outside markers=%llu\n", (unsigned long long)wrap_stray(i));
    int reads_zero = !strcmp(never_read, "ZERO") && !strcmp(never_reread, "ZERO") && !strcmp(unbound_reread, "ZERO");
    int writes_discarded = !leak.found && !stale.found && c.found == WRAP_BLOCKS && after.found == WRAP_BLOCKS;
    printf("RESULT_DETAIL readNeverBound=%s rereadNeverBound=%s readUnbound=%s neverBoundWriteInBacking=%u "
           "unboundWriteInBacking=%u controlIntact=%d\n", never_read, never_reread, unbound_reread, leak.found,
           stale.found, after.found == WRAP_BLOCKS);
    if (reads_zero && writes_discarded)
        puts("VERDICT UNBOUND_ACCESS_STRICT no fault; unbound reads are zero and unbound writes are discarded: H2 refuted "
             "for copy-engine access to a small image");
    else if (sp.residencyNonResidentStrict)
        printf("VERDICT UNBOUND_ACCESS_VIOLATES_STRICT no fault, but residencyNonResidentStrict=1 and %s%s\n",
            reads_zero ? "" : "an unbound read was not zero ", writes_discarded ? "" : "an unbound write reached memory");
    else
        puts("VERDICT UNBOUND_ACCESS_NO_FAULT no fault; values differ from strict residency, which this device does not claim");
    if (!strcmp(unbound_reread, "MARKER") || stale.found)
        puts("NOTE the unbound tile still reached its former memory: the unbind did not reach the page tables");
    pass();

    start("release-completed-resources");
    vkDestroyImage(device, i->handle, NULL);
    vkDestroyBuffer(device, i->backing_buffer, NULL);
    release_allocation(i->allocation); release_buffer(i->upload); release_buffer(i->readback); release_buffer(out);
    free(i); memory_log(); pass(); return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--application-shape") && strcmp(argv[1], "--tiled-control") &&
        strcmp(argv[1], "--large-tiled-control") && strcmp(argv[1], "--resource-size-control") &&
        strcmp(argv[1], "--wrap-discriminator") && strcmp(argv[1], "--unbound-access"))) {
        fprintf(stderr, "usage: %s [--application-shape | --tiled-control | --large-tiled-control | --resource-size-control |"
            " --wrap-discriminator | --unbound-access]\n"
            "Set VK_DRIVER_FILES externally to the installed NVK manifest.\n", argv[0]);
        return 2;
    }
    require(getenv("VK_DRIVER_FILES") && *getenv("VK_DRIVER_FILES"), "explicit VK_DRIVER_FILES required; ICD is never changed by probe");
    printf("ICD VK_DRIVER_FILES=%s\n", getenv("VK_DRIVER_FILES"));
    start("instance-validation");
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = message};
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &debug,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &sync};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "mkw sparse compatibility",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &validation,
        .pApplicationInfo = &app, .enabledLayerCount = 1, .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    VK(vkCreateInstance(&ii, NULL, &instance));
    PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    require(create && destroy, "debug utils entry points");
    VkDebugUtilsMessengerEXT messenger;
    VK(create(instance, &debug, NULL, &messenger)); pass();
    start("select-nvk-device");
    uint32_t count = 0; VkPhysicalDevice gpus[16];
    VK(vkEnumeratePhysicalDevices(instance, &count, NULL)); require(count && count <= 16, "device count");
    VK(vkEnumeratePhysicalDevices(instance, &count, gpus));
    for (uint32_t n = 0; n < count; n++) {
        VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
        vkGetPhysicalDeviceProperties2(gpus[n], &p);
        printf("DEVICE %s driver=%s info=%s driverID=%d api=%u.%u.%u\n", p.properties.deviceName,
            driver.driverName, driver.driverInfo, driver.driverID, VK_API_VERSION_MAJOR(p.properties.apiVersion),
            VK_API_VERSION_MINOR(p.properties.apiVersion), VK_API_VERSION_PATCH(p.properties.apiVersion));
        if (driver.driverID == VK_DRIVER_ID_MESA_NVK && p.properties.vendorID == 0x10de && !gpu) {
            gpu = gpus[n]; props = p.properties;
        }
    }
    require(gpu != VK_NULL_HANDLE, "installed NVK device not found");
    require(props.apiVersion >= VK_API_VERSION_1_2, "Vulkan 1.2 required");
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(gpu, &f);
    printf("FEATURES sparseBinding=%u sparseResidencyImage2D=%u sparseResidencyAliased=%u textureCompressionBC=%u timeline=%u\n",
        f.features.sparseBinding, f.features.sparseResidencyImage2D, f.features.sparseResidencyAliased,
        f.features.textureCompressionBC, f12.timelineSemaphore);
    if (!f.features.sparseBinding || !f.features.sparseResidencyImage2D || !f.features.sparseResidencyAliased ||
        !f.features.textureCompressionBC || !f12.timelineSemaphore) { puts("SKIP required device feature unavailable"); return 77; }
    uint32_t nfmt = 0; VkSparseImageFormatProperties formats[8];
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkGetPhysicalDeviceSparseImageFormatProperties(gpu, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D,
        VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_TILING_OPTIMAL, &nfmt, NULL);
    if (!nfmt) { puts("SKIP no sparse BC7 format properties"); return 77; }
    require(nfmt <= 8, "sparse format property count");
    vkGetPhysicalDeviceSparseImageFormatProperties(gpu, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D,
        VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_TILING_OPTIMAL, &nfmt, formats);
    for (uint32_t n = 0; n < nfmt; n++) printf("FORMAT aspect=%#x granularity=%ux%ux%u flags=%#x\n",
        formats[n].aspectMask, formats[n].imageGranularity.width, formats[n].imageGranularity.height,
        formats[n].imageGranularity.depth, formats[n].flags);
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    VkExtensionProperties exts[512]; uint32_t ne = 512;
    VK(vkEnumerateDeviceExtensionProperties(gpu, NULL, &ne, exts));
    for (uint32_t n = 0; n < ne; n++) if (!strcmp(exts[n].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) has_budget = 1;
    printf("MEMORY_BUDGET extension=%d (if absent only heap sizes and allocation accounting are available)\n", has_budget);
    memory_log(); pass();
    start("device-queue");
    VkQueueFamilyProperties families[32]; count = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t n = 0; n < count; n++) {
        printf("QUEUE family=%u flags=%#x count=%u\n", n, families[n].queueFlags, families[n].queueCount);
        if ((families[n].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_SPARSE_BINDING_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_SPARSE_BINDING_BIT) && family == UINT32_MAX) family = n;
    }
    require(family != UINT32_MAX, "combined graphics+sparse queue required (runtime contract)");
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures enabled = {.sparseBinding = VK_TRUE, .sparseResidencyImage2D = VK_TRUE,
        .sparseResidencyAliased = VK_TRUE, .textureCompressionBC = VK_TRUE};
    VkPhysicalDeviceVulkan12Features enable12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                               .timelineSemaphore = VK_TRUE};
    const char *budget_ext = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enable12,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .pEnabledFeatures = &enabled,
        .enabledExtensionCount = has_budget ? 1u : 0u, .ppEnabledExtensionNames = &budget_ext};
    VK(vkCreateDevice(gpu, &di, NULL, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VK(vkCreateCommandPool(device, &pi, NULL, &pool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK(vkAllocateCommandBuffers(device, &ai, &cmd));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(device, &fi, NULL, &fence));
    VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                   .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
    VK(vkCreateSemaphore(device, &si, NULL, &timeline)); pass();
    int tiled_control = argc == 2 && !strcmp(argv[1], "--tiled-control");
    int large_tiled_control = argc == 2 && !strcmp(argv[1], "--large-tiled-control");
    int resource_size_control = argc == 2 && !strcmp(argv[1], "--resource-size-control");
    int wrap_discriminator = argc == 2 && !strcmp(argv[1], "--wrap-discriminator");
    int unbound_access = argc == 2 && !strcmp(argv[1], "--unbound-access");
    int status = unbound_access ? run_unbound_access() : wrap_discriminator ? run_wrap_discriminator() :
        resource_size_control ? run_image(8192, 32, 1, 1) :
        large_tiled_control ? run_image(8192, 68, 1, 1) : run_image(512, 4, 0, tiled_control);
    if (!status && argc == 2 && !tiled_control && !large_tiled_control && !resource_size_control && !wrap_discriminator &&
        !unbound_access)
        status = run_image(8192, 68, 1, 0);
    scenario = "device"; start("teardown");
    vkDestroySemaphore(device, timeline, NULL); vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL); vkDestroyDevice(device, NULL);
    destroy(instance, messenger, NULL); vkDestroyInstance(instance, NULL); pass();
    printf("RESULT %s validation_errors=%u warnings=%u peak_allocated=%llu bytes queue_operations_completed=%llu\n",
           status ? "SKIP" : "PASS", atomic_load(&errors), atomic_load(&warnings), (unsigned long long)peak,
           (unsigned long long)serial);
    return status;
}
#endif
