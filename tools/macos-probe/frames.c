#include "probe.h"
#include <stdlib.h>
#include <string.h>

static const uint64_t timeout_ns = 3000000000ull;
enum { sample_bytes = 12 }; // First, middle and last pixel, tightly packed.

static bool pick_format(Context *ctx, VkFormat *out) {
    uint32_t count;
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->gpu, ctx->surface, &count, NULL));
    VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
    REQUIRE(formats);
    VkResult r = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->gpu, ctx->surface, &count, formats);
    const VkFormat preferred[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    *out = VK_FORMAT_UNDEFINED;
    if (r == VK_SUCCESS) {
        for (unsigned p = 0; p < COUNT(preferred) && *out == VK_FORMAT_UNDEFINED; ++p)
            for (uint32_t i = 0; i < count; ++i)
                if (formats[i].format == preferred[p] && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    *out = preferred[p];
    }
    free(formats);
    VK_TRY(r);
    REQUIRE(*out != VK_FORMAT_UNDEFINED);
    return true;
}

bool open_chain(Context *ctx, Swapchain *sc, VkExtent2D pixels, VkSwapchainKHR old) {
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->gpu, ctx->surface, &caps));
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    REQUIRE((caps.supportedUsageFlags & usage) == usage);
    REQUIRE(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    if (!pick_format(ctx, &sc->format)) return false;
    sc->extent = caps.currentExtent.width == UINT32_MAX ? pixels : caps.currentExtent;
    REQUIRE(sc->extent.width == pixels.width && sc->extent.height == pixels.height);
    REQUIRE(pixels.width >= caps.minImageExtent.width && pixels.width <= caps.maxImageExtent.width);
    REQUIRE(pixels.height >= caps.minImageExtent.height && pixels.height <= caps.maxImageExtent.height);
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = ctx->surface,
        .minImageCount = count, .imageFormat = sc->format, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = sc->extent, .imageArrayLayers = 1, .imageUsage = usage, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_FALSE, .oldSwapchain = old};
    VK_TRY(vkCreateSwapchainKHR(ctx->device, &info, NULL, &sc->chain));
    VK_TRY(vkGetSwapchainImagesKHR(ctx->device, sc->chain, &sc->count, NULL));
    sc->images = calloc(sc->count, sizeof(*sc->images));
    REQUIRE(sc->images);
    VK_TRY(vkGetSwapchainImagesKHR(ctx->device, sc->chain, &sc->count, sc->images));
    printf("FIFO %s UNORM nonlinear sRGB, %ux%u, %u images, usage %#x\n",
        sc->format == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : "RGBA8", pixels.width, pixels.height, sc->count, usage);
    return true;
}

void close_chain(Context *ctx, Swapchain *sc) {
    vkDestroySwapchainKHR(ctx->device, sc->chain, NULL);
    free(sc->images);
    *sc = (Swapchain){0};
}

bool open_frames(Context *ctx, Frames *f) {
    VkDevice d = ctx->device;
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = ctx->family};
    VK_TRY(vkCreateCommandPool(d, &pool, NULL, &f->pool));
    VkCommandBufferAllocateInfo cmd = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = f->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK_TRY(vkAllocateCommandBuffers(d, &cmd, &f->cmd));
    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_TRY(vkCreateSemaphore(d, &sem, NULL, &f->acquired));
    VK_TRY(vkCreateSemaphore(d, &sem, NULL, &f->ready));
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    sem.pNext = &type;
    VK_TRY(vkCreateSemaphore(d, &sem, NULL, &f->timeline));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_TRY(vkCreateFence(d, &fence, NULL, &f->present_fence));

    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sample_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_TRY(vkCreateBuffer(d, &buffer, NULL, &f->buffer));
    VkMemoryRequirements required;
    vkGetBufferMemoryRequirements(d, f->buffer, &required);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(ctx->gpu, &memory);
    uint32_t i;
    for (i = 0; i < memory.memoryTypeCount; ++i)
        if ((required.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) break;
    REQUIRE(i < memory.memoryTypeCount);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = required.size, .memoryTypeIndex = i};
    VK_TRY(vkAllocateMemory(d, &alloc, NULL, &f->memory));
    VK_TRY(vkBindBufferMemory(d, f->buffer, f->memory, 0));
    void *mapped;
    VK_TRY(vkMapMemory(d, f->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    f->mapped = mapped;
    return true;
}

void close_frames(Context *ctx, Frames *f) {
    VkDevice d = ctx->device;
    if (f->mapped) vkUnmapMemory(d, f->memory);
    vkDestroyBuffer(d, f->buffer, NULL);
    vkFreeMemory(d, f->memory, NULL);
    vkDestroySemaphore(d, f->timeline, NULL);
    vkDestroySemaphore(d, f->ready, NULL);
    vkDestroySemaphore(d, f->acquired, NULL);
    vkDestroyFence(d, f->present_fence, NULL);
    vkDestroyCommandPool(d, f->pool, NULL);
}

static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
        VkAccessFlags2 before, VkAccessFlags2 after) {
    VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .oldLayout = from, .newLayout = to, .srcAccessMask = before, .dstAccessMask = after,
        .srcStageMask = before ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = after ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cmd, &dep);
}

static bool record(Context *ctx, Frames *f, Swapchain *sc, VkImage image, bool alternate) {
    VK_TRY(vkResetCommandPool(ctx->device, f->pool, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_TRY(vkBeginCommandBuffer(f->cmd, &begin));
    transition(f->cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkClearColorValue color = {.float32 = {(alternate ? 52 : 48) / 255.0f, 60 / 255.0f, 72 / 255.0f, 1}};
    vkCmdClearColorImage(f->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    transition(f->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copies[3];
    for (unsigned i = 0; i < COUNT(copies); ++i)
        copies[i] = (VkBufferImageCopy){.bufferOffset = i * 4,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {(int32_t)((sc->extent.width - 1) * i / 2), (int32_t)((sc->extent.height - 1) * i / 2), 0},
            .imageExtent = {1, 1, 1}};
    vkCmdCopyImageToBuffer(f->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f->buffer, COUNT(copies), copies);
    VkMemoryBarrier2 host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &host};
    vkCmdPipelineBarrier2(f->cmd, &dep);
    transition(f->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_2_TRANSFER_READ_BIT, 0);
    VK_TRY(vkEndCommandBuffer(f->cmd));
    return true;
}

static bool submit(Context *ctx, Frames *f) {
    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = f->acquired,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSemaphoreSubmitInfo signals[] = {
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = f->ready, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = f->timeline, .value = ++f->serial,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkCommandBufferSubmitInfo cmd = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = f->cmd};
    VkSubmitInfo2 info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd,
        .signalSemaphoreInfoCount = COUNT(signals), .pSignalSemaphoreInfos = signals};
    VK_TRY(vkQueueSubmit2(ctx->queue, 1, &info, VK_NULL_HANDLE));
    return true;
}

bool draw(Context *ctx, Frames *f, Swapchain *sc, bool *rebuild) {
    uint32_t index;
    VkResult acquire = vkAcquireNextImageKHR(ctx->device, sc->chain, timeout_ns, f->acquired, VK_NULL_HANDLE, &index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) { *rebuild = true; return true; }
    if (acquire != VK_SUBOPTIMAL_KHR) VK_TRY(acquire);
    bool alternate = (f->serial & 1) != 0;
    if (!record(ctx, f, sc, sc->images[index], alternate) || !submit(ctx, f)) return false;

    VkPresentIdKHR id = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &f->serial};
    VkSwapchainPresentFenceInfoKHR retired = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .pNext = &id, .swapchainCount = 1, .pFences = &f->present_fence};
    VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &retired,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &f->ready, .swapchainCount = 1,
        .pSwapchains = &sc->chain, .pImageIndices = &index};
    VkResult present = vkQueuePresentKHR(ctx->queue, &info);
    // Even a present rejected by WSI can have enqueued its semaphore wait.
    if (present != VK_SUBOPTIMAL_KHR && present != VK_ERROR_OUT_OF_DATE_KHR && present != VK_ERROR_SURFACE_LOST_KHR)
        VK_TRY(present);
    VK_TRY(vkWaitForFences(ctx->device, 1, &f->present_fence, VK_TRUE, timeout_ns));
    VK_TRY(vkResetFences(ctx->device, 1, &f->present_fence));
    if (present == VK_ERROR_SURFACE_LOST_KHR) VK_TRY(present);
    if (present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR) {
        VkResult shown = ctx->wait_for_present(ctx->device, sc->chain, f->serial, timeout_ns);
        if (shown == VK_ERROR_OUT_OF_DATE_KHR) present = shown;
        else VK_TRY(shown);
    }
    VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
        .pSemaphores = &f->timeline, .pValues = &f->serial};
    VK_TRY(vkWaitSemaphores(ctx->device, &wait, timeout_ns));
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = f->memory, .size = VK_WHOLE_SIZE};
    VK_TRY(vkInvalidateMappedMemoryRanges(ctx->device, 1, &range));
    bool bgra = sc->format == VK_FORMAT_B8G8R8A8_UNORM;
    uint8_t red = alternate ? 52 : 48;
    uint8_t expected[4] = {bgra ? 72 : red, 60, bgra ? red : 72, 255};
    for (unsigned i = 0; i < sample_bytes; i += sizeof(expected)) {
        if (memcmp(f->mapped + i, expected, sizeof(expected))) {
            fprintf(stderr, "readback frame %llu sample %u: %u %u %u %u, expected %u 60 %u 255\n",
                (unsigned long long)f->serial, i / 4, f->mapped[i], f->mapped[i+1], f->mapped[i+2], f->mapped[i+3],
                expected[0], expected[2]);
            return false;
        }
    }
    *rebuild = acquire != VK_SUCCESS || present != VK_SUCCESS;
    return true;
}
