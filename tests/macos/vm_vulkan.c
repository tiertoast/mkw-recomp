// Full upload when the OS cannot list dirty pages; fenced readback of a known GPU-written subrange.
#include "host.h"
#include "platform/platform.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

enum { BYTES = 1 << 20, ROUNDS = 32 };
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(1); } } while (0)
#define VK(call) do { VkResult result = (call); if (result != VK_SUCCESS) { \
    fprintf(stderr, "line %d: %s -> %d\n", __LINE__, #call, result); exit(1); } } while (0)
static atomic_uint errors;

static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)severity; (void)type; (void)user;
    atomic_fetch_add(&errors, 1);
    fprintf(stderr, "%s\n", data->pMessage);
    return VK_FALSE;
}

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *map;
    VkMemoryPropertyFlags properties;
} Buffer;

static Buffer new_buffer(VkDevice dev, VkPhysicalDevice gpu, VkBufferUsageFlags use, int host) {
    Buffer b = {0};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BYTES, .usage = use,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK(vkCreateBuffer(dev, &bi, NULL, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t selected = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        VkMemoryPropertyFlags props = mem.memoryTypes[i].propertyFlags;
        if (!(req.memoryTypeBits & (1u << i)) || (props & want) != want) continue;
        if (selected == UINT32_MAX) selected = i;
        if (host && !(props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { selected = i; break; }
    }
    CHECK(selected != UINT32_MAX);
    b.properties = mem.memoryTypes[selected].propertyFlags;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = selected};
    VK(vkAllocateMemory(dev, &ai, NULL, &b.memory));
    VK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
    if (host) {
        void *map = NULL;
        VK(vkMapMemory(dev, b.memory, 0, VK_WHOLE_SIZE, 0, &map));
        b.map = map;
    }
    return b;
}

static void release(VkDevice dev, Buffer b) {
    if (b.map) vkUnmapMemory(dev, b.memory);
    vkDestroyBuffer(dev, b.buffer, NULL);
    vkFreeMemory(dev, b.memory, NULL);
}

static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags dst, VkAccessFlags access) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                         .dstAccessMask = access};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst, 0, 1, &b, 0, NULL, 0, NULL);
}

int main(void) {
    CHECK(!setenv("VK_DRIVER_FILES", "/Library/GPUBundles/nvmtl/nvk_icd.json", 1));
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = message};
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &debug,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &sync};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &validation, .pApplicationInfo = &app,
        .enabledLayerCount = 1, .ppEnabledLayerNames = &layer, .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    VkInstance instance;
    VK(vkCreateInstance(&ii, NULL, &instance));
    PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerEXT messenger;
    CHECK(create && destroy);
    VK(create(instance, &debug, NULL, &messenger));

    uint32_t count = 0;
    VK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    CHECK(count);
    VkPhysicalDevice *gpus = calloc(count, sizeof *gpus), gpu = VK_NULL_HANDLE;
    CHECK(gpus);
    VK(vkEnumeratePhysicalDevices(instance, &count, gpus));
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
        vkGetPhysicalDeviceProperties2(gpus[i], &props);
        if (driver.driverID == VK_DRIVER_ID_MESA_NVK && props.properties.vendorID == 0x10de) {
            gpu = gpus[i];
            printf("%s, %s\n", props.properties.deviceName, driver.driverInfo);
            break;
        }
    }
    free(gpus);
    CHECK(gpu);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof *families);
    CHECK(families);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    free(families);
    CHECK(family < count);
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                  .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    VkDevice dev;
    VK(vkCreateDevice(gpu, &di, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, family, 0, &queue);
    Buffer upload = new_buffer(dev, gpu, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
    Buffer local = new_buffer(dev, gpu, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    Buffer readback = new_buffer(dev, gpu, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    printf("HOST_COHERENT upload=%d readback=%d\n",
           !!(upload.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), !!(readback.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;
    VK(vkCreateCommandPool(dev, &pi, NULL, &pool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    VK(vkAllocateCommandBuffers(dev, &ai, &cmd));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK(vkCreateFence(dev, &fi, NULL, &fence));

    mem_init();   // Reserve the complete layout with the NVK driver and its buffers already mapped.
    uint64_t addr = mem_layout()->alias.base;
    CHECK(!mem_map_physical(addr, BYTES));
    uint8_t *guest = (uint8_t *)(uintptr_t)addr;
    uint8_t *expected = malloc(BYTES);
    CHECK(expected);
    for (size_t i = 0; i < BYTES; i++) guest[i] = (uint8_t)(i * 97 + (i >> 12));
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .size = VK_WHOLE_SIZE};
    uint64_t start = plat_now_ns();
    for (unsigned round = 0; round < ROUNDS; round++) {
        unsigned offset = round * 4096, word = 0xb4c00000u + round;
        guest[BYTES - round - 1] ^= 0x5b;
        uint64_t page;
        CHECK(mem_written_pages(addr, BYTES, &page, 1) == UINT64_MAX);
        memcpy(upload.map, guest, BYTES);
        memcpy(expected, guest, BYTES);
        range.memory = upload.memory;
        VK(vkFlushMappedMemoryRanges(dev, 1, &range));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(cmd, &begin));
        VkBufferCopy copy = {.size = BYTES};
        vkCmdCopyBuffer(cmd, upload.buffer, local.buffer, 1, &copy);
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, local.buffer, offset, sizeof word, word);
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdCopyBuffer(cmd, local.buffer, readback.buffer, 1, &copy);
        barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        VK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
        VK(vkQueueSubmit(queue, 1, &submit, fence));
        guest[offset + 19] ^= 0xf7;  // A CPU write after submission on the same guest page as the GPU word.
        VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
        range.memory = readback.memory;
        VK(vkInvalidateMappedMemoryRanges(dev, 1, &range));
        memcpy(expected + offset, &word, sizeof word);
        CHECK(!memcmp(readback.map, expected, BYTES));
        CHECK(!mem_trap(addr + offset, sizeof word, readback.map + offset));
        memcpy(guest + offset, readback.map + offset, sizeof word);
        expected[offset + 19] ^= 0xf7;
        CHECK(!memcmp(guest, expected, BYTES));
        VK(vkResetFences(dev, 1, &fence));
    }
    printf("%d rounds, %u bytes uploaded and read back per round, %.3f ms including host comparisons\n",
           ROUNDS, BYTES, (plat_now_ns() - start) / 1e6);
    CHECK(!mem_unmap_physical(addr, BYTES) && !plat_mem_readable(addr, BYTES));
    free(expected);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    release(dev, readback);
    release(dev, local);
    release(dev, upload);
    vkDestroyDevice(dev, NULL);
    destroy(instance, messenger, NULL);
    vkDestroyInstance(instance, NULL);
    CHECK(!atomic_load(&errors));
    puts("validation errors: 0");
    return 0;
}
