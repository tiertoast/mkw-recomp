#include "probe.h"
#include <stdlib.h>
#include <string.h>

static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *msg, void *user) {
    (void)type;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        atomic_fetch_add(&((Context *)user)->errors, 1);
    fprintf(stderr, "validation: %s\n", msg->pMessage);
    return VK_FALSE;
}

bool open_instance(Context *ctx) {
    uint32_t count;
    const char *const *sdl = SDL_Vulkan_GetInstanceExtensions(&count);
    SDL_TRY(sdl);
    const char *extra[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
    const char **extensions = calloc(count + COUNT(extra), sizeof(*extensions));
    REQUIRE(extensions);
    memcpy(extensions, sdl, count * sizeof(*extensions));
    memcpy(extensions + count, extra, sizeof(extra));
    const char *layer = "VK_LAYER_KHRONOS_validation";
    VkBool32 enable = VK_TRUE;
    VkLayerSettingEXT sync = {layer, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enable};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = validation, .pUserData = ctx};
    VkLayerSettingsCreateInfoEXT settings = {.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT, .pNext = &debug,
        .settingCount = 1, .pSettings = &sync};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "mkw G1 probe",
        .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .pNext = &settings, .enabledExtensionCount = count + COUNT(extra), .ppEnabledExtensionNames = extensions,
        .enabledLayerCount = 1, .ppEnabledLayerNames = &layer};
    VkResult result = vkCreateInstance(&info, NULL, &ctx->instance);
    free(extensions);
    if (result != VK_SUCCESS) fprintf(stderr, "vkCreateInstance: %d (requires %s)\n", result, layer);
    VK_TRY(result);
    PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(ctx->instance, "vkCreateDebugUtilsMessengerEXT");
    REQUIRE(create);
    VK_TRY(create(ctx->instance, &debug, NULL, &ctx->messenger));
    puts("instance Vulkan 1.3, VK_LAYER_KHRONOS_validation, validate_sync=true");
    return true;
}

static bool pick_queue(Context *ctx) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->gpu, &count, NULL);
    VkQueueFamilyProperties *props = calloc(count, sizeof(*props));
    REQUIRE(props);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->gpu, &count, props);
    const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    ctx->family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        if ((props[i].queueFlags & required) != required) continue;
        VkBool32 present;
        VkResult r = vkGetPhysicalDeviceSurfaceSupportKHR(ctx->gpu, i, ctx->surface, &present);
        if (r != VK_SUCCESS) { free(props); VK_TRY(r); }
        if (present) { ctx->family = i; break; }
    }
    free(props);
    REQUIRE(ctx->family != UINT32_MAX);
    printf("queue family %u supports graphics, compute and SDL surface present\n", ctx->family);
    return true;
}

bool open_device(Context *ctx) {
    uint32_t count = 1;
    VK_TRY(vkEnumeratePhysicalDevices(ctx->instance, &count, &ctx->gpu));
    REQUIRE(count == 1);
    VkPhysicalDevicePushDescriptorProperties push = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &push};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(ctx->gpu, &props);
    printf("GPU %s %04x:%04x driverID=%d %s, Vulkan %u.%u.%u\n", props.properties.deviceName,
        props.properties.vendorID, props.properties.deviceID, driver.driverID, driver.driverInfo,
        VK_API_VERSION_MAJOR(props.properties.apiVersion), VK_API_VERSION_MINOR(props.properties.apiVersion),
        VK_API_VERSION_PATCH(props.properties.apiVersion));
    printf("limits: maxPushDescriptors=%u minStorageBufferOffsetAlignment=%llu\n",
        push.maxPushDescriptors, (unsigned long long)props.properties.limits.minStorageBufferOffsetAlignment);
    REQUIRE(driver.driverID == VK_DRIVER_ID_MESA_NVK && props.properties.vendorID == 0x10de);
    REQUIRE(props.properties.apiVersion >= VK_API_VERSION_1_3);
    if (!pick_queue(ctx)) return false;

    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDevicePresentWaitFeaturesKHR displayed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
        .pNext = &mesh};
    VkPhysicalDevicePresentIdFeaturesKHR ids = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
        .pNext = &displayed};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, .pNext = &ids};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, .pNext = &maintenance};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &clip};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(ctx->gpu, &f2);
    printf("optional: taskShader=%u meshShader=%u bufferDeviceAddress=%u\n",
        mesh.taskShader, mesh.meshShader, f12.bufferDeviceAddress);
    displayed.pNext = NULL;
    // Keep this list in step with the required-feature check in vk_device.c.
    REQUIRE(f12.timelineSemaphore); REQUIRE(f12.drawIndirectCount);
    REQUIRE(f13.dynamicRendering); REQUIRE(f13.synchronization2); REQUIRE(f13.shaderDemoteToHelperInvocation);
    REQUIRE(clip.depthClipControl); REQUIRE(f2.features.shaderStorageImageWriteWithoutFormat);
    REQUIRE(f2.features.shaderInt64); REQUIRE(f2.features.samplerAnisotropy); REQUIRE(f2.features.independentBlend);
    REQUIRE(f2.features.geometryShader); REQUIRE(f2.features.textureCompressionBC);
    REQUIRE(maintenance.swapchainMaintenance1); // Present resource lifetime in this probe.
    REQUIRE(ids.presentId && displayed.presentWait); // Observe presentation before changing Cocoa window state.
    f2.features = (VkPhysicalDeviceFeatures){.shaderStorageImageWriteWithoutFormat = 1, .shaderInt64 = 1,
        .samplerAnisotropy = 1, .independentBlend = 1, .geometryShader = 1, .textureCompressionBC = 1};
    f12 = (VkPhysicalDeviceVulkan12Features){.sType = f12.sType, .pNext = &f13, .timelineSemaphore = 1, .drawIndirectCount = 1};
    f13 = (VkPhysicalDeviceVulkan13Features){.sType = f13.sType, .pNext = &clip, .dynamicRendering = 1,
        .synchronization2 = 1, .shaderDemoteToHelperInvocation = 1};
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_KHR_PRESENT_WAIT_EXTENSION_NAME};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue, .enabledExtensionCount = COUNT(extensions),
        .ppEnabledExtensionNames = extensions};
    VK_TRY(vkCreateDevice(ctx->gpu, &device, NULL, &ctx->device));
    ctx->wait_for_present = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(ctx->device, "vkWaitForPresentKHR");
    REQUIRE(ctx->wait_for_present);
    vkGetDeviceQueue(ctx->device, ctx->family, 0, &ctx->queue);
    puts("device created with 12 runtime features, swapchain, push_descriptor and depth_clip_control");
    return true;
}

void close_context(Context *ctx) {
    if (ctx->device) vkDestroyDevice(ctx->device, NULL);
    if (ctx->surface) SDL_Vulkan_DestroySurface(ctx->instance, ctx->surface, NULL);
    if (ctx->messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(ctx->instance, "vkDestroyDebugUtilsMessengerEXT");
        destroy(ctx->instance, ctx->messenger, NULL);
    }
    if (ctx->instance) vkDestroyInstance(ctx->instance, NULL);
}
