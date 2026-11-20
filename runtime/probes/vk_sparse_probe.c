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
    fprintf(stderr, "FAIL [%s/%s] %s; validation errors=%u warnings=%u; live=%llu peak=%llu bytes\n",
            scenario, checkpoint, why, atomic_load(&errors), atomic_load(&warnings),
            (unsigned long long)live, (unsigned long long)peak);
    // On device loss/timeout, do not issue more work or wait indefinitely in device destruction.
    // OS process teardown releases resources, including partially constructed resources.
    exit(1);
}

static void require(int condition, const char *why) { if (!condition) fail(why); }
static void result(VkResult r, const char *api) {
    printf("API [%s/%s] %s -> %d\n", scenario, checkpoint, api, r);
    if (r != VK_SUCCESS) fail(api);
    require(!atomic_load(&errors), "validation error");
}
#define VK(call) result((call), #call)
static void start(const char *name) {
    checkpoint = name;
    printf("BEGIN [%s/%s]\n", scenario, checkpoint);
}
static void pass(void) {
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

static void bind_image(Image *i, int tails) {
    start(tails ? "bind-mip-tails-metadata" : "bind-tiles");
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

static uint32_t dim(uint32_t w, uint32_t mip) { return w >> mip ? w >> mip : 1; }
static uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static VkDeviceSize aligned(VkDeviceSize n, VkDeviceSize a) { return (n + a - 1) / a * a; }

static void validate_requirements(Image *i) {
    start("requirements-consistency");
    // A second public query separates a malformed driver result from legacy-query handling.
    VkImageSparseMemoryRequirementsInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2, .image = i->handle};
    VkSparseImageMemoryRequirements2 check[8] = {0};
    uint32_t count = 8;
    for (uint32_t n = 0; n < 8; n++) check[n].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2;
    vkGetImageSparseMemoryRequirements2(device, &info, &count, check);
    require(count == i->nsparse, "legacy/requirements2 count differs");
    for (uint32_t n = 0; n < count; n++) {
        const VkSparseImageMemoryRequirements *a = &i->sparse[n], *b = &check[n].memoryRequirements;
        printf("REQUIREMENTS2 aspect=%#x granularity=%ux%ux%u flags=%#x tailFirstLod=%u offset=%#llx size=%llu stride=%llu\n",
            b->formatProperties.aspectMask, b->formatProperties.imageGranularity.width,
            b->formatProperties.imageGranularity.height, b->formatProperties.imageGranularity.depth,
            b->formatProperties.flags, b->imageMipTailFirstLod, (unsigned long long)b->imageMipTailOffset,
            (unsigned long long)b->imageMipTailSize, (unsigned long long)b->imageMipTailStride);
        require(a->formatProperties.aspectMask == b->formatProperties.aspectMask &&
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
            require(!layer || stride <= (UINT64_MAX - a->imageMipTailOffset) / layer, "mip-tail offset overflow");
            VkDeviceSize offset = a->imageMipTailOffset + layer * stride;
            if (offset >= i->req.size || a->imageMipTailSize > i->req.size - offset) {
                fprintf(stderr, "INVALID_REQUIREMENTS aspect=%#x layer=%u tailOffset=%#llx tailSize=%llu imageSize=%llu; "
                    "opaque bind would violate VUID-VkSparseMemoryBind-resourceOffset-01099/size-01100; no bind submitted\n",
                    a->formatProperties.aspectMask, layer, (unsigned long long)offset,
                    (unsigned long long)a->imageMipTailSize, (unsigned long long)i->req.size);
                fail("driver-reported mip-tail range is outside image memory requirements");
            }
            require(!(offset % i->req.alignment) && !(a->imageMipTailSize % i->req.alignment), "unaligned mip-tail range");
        }
    }
    pass();
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
        large ? "one tile per non-tail mip/layer, complete mip tails; other tiles untouched" : "fully resident, all texels transferred");
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
    scenario = large ? "8192x8192x68" : tiled_control ? "512x512x4-tiled-control" : "512x512x4";
    Image *i = calloc(1, sizeof *i);
    require(i != NULL, "host allocation");
    i->width = width; i->layers = layers;
    for (uint32_t w = width; w; w >>= 1) i->mips++;
    // Independent control: still BC7 + multiple layers/mips, without requiring mip-tail residency.
    // This is an explicit separate run, never an automatic fallback after a failed checkpoint.
    if (tiled_control) i->mips = 2;
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
        (width > fp.maxExtent.width || width > fp.maxExtent.height || layers > fp.maxArrayLayers || i->mips > fp.maxMipLevels))) {
        printf("SKIP [%s] requested sparse BC7 shape unsupported\n", scenario); free(i); return 77;
    }
    result(r, "vkGetPhysicalDeviceImageFormatProperties");
    printf("IMAGE format=%d flags=%#x usage=%#x mips=%u maxResourceSize=%llu\n", ci.format, ci.flags, ci.usage,
           i->mips, (unsigned long long)fp.maxResourceSize);
    pass(); start("create-image");
    VK(vkCreateImage(device, &ci, NULL, &i->handle)); pass();
    start("memory-requirements");
    vkGetImageMemoryRequirements(device, i->handle, &i->req);
    vkGetImageSparseMemoryRequirements(device, i->handle, &i->nsparse, NULL);
    require(i->nsparse && i->nsparse <= 8 && i->req.alignment && i->req.memoryTypeBits, "invalid memory requirements");
    vkGetImageSparseMemoryRequirements(device, i->handle, &i->nsparse, i->sparse);
    printf("REQUIREMENTS virtual=%llu alignment=%llu types=%#x sparseCount=%u addressSpaceLimit=%llu\n",
        (unsigned long long)i->req.size, (unsigned long long)i->req.alignment, i->req.memoryTypeBits, i->nsparse,
        (unsigned long long)props.limits.sparseAddressSpaceSize);
    pass(); validate_requirements(i);
    start("plan-residency"); plan(i, large); pass();
    // Conservative preflight includes alignment slack for the two staging buffers and backing buffer.
    if (i->backing > CAP / 2 || i->bytes > (CAP - i->backing) / 2 - MIB) {
        puts("SKIP residency/transfers: planned memory exceeds conservative 128 MiB budget");
        vkDestroyImage(device, i->handle, NULL); free(i); return 77;
    }
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
    bind_image(i, 0); bind_image(i, 1);
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

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--application-shape") && strcmp(argv[1], "--tiled-control"))) {
        fprintf(stderr, "usage: %s [--application-shape | --tiled-control]\n"
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
    int status = run_image(512, 4, 0, tiled_control);
    if (!status && argc == 2 && !tiled_control) status = run_image(8192, 68, 1, 0);
    scenario = "device"; start("teardown");
    vkDestroySemaphore(device, timeline, NULL); vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL); vkDestroyDevice(device, NULL);
    destroy(instance, messenger, NULL); vkDestroyInstance(instance, NULL); pass();
    printf("RESULT %s validation_errors=%u warnings=%u peak_allocated=%llu bytes\n", status ? "SKIP" : "PASS",
           atomic_load(&errors), atomic_load(&warnings), (unsigned long long)peak);
    return status;
}
#endif
