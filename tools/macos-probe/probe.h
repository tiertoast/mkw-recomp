#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdatomic.h>
#include <stdio.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define REQUIRE(expr) do { if (!(expr)) { fprintf(stderr, "failed: %s\n", #expr); return false; } } while (0)
#define VK_TRY(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "%s returned %s (%d)\n", #expr, string_VkResult(r_), r_); return false; } } while (0)
#define SDL_TRY(expr) do { if (!(expr)) { fprintf(stderr, "%s: %s\n", #expr, SDL_GetError()); return false; } } while (0)

typedef struct {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice gpu;
    uint32_t family;
    VkDevice device;
    VkQueue queue;
    PFN_vkWaitForPresentKHR wait_for_present;
    atomic_uint errors;
} Context;

typedef struct {
    VkSwapchainKHR chain;
    VkFormat format;
    VkExtent2D extent;
    VkImage *images;
    uint32_t count;
} Swapchain;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore acquired, ready, timeline;
    VkFence present_fence;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    uint64_t serial;
} Frames;

bool open_instance(Context *ctx);
bool open_device(Context *ctx);
void close_context(Context *ctx);
bool open_frames(Context *ctx, Frames *frames);
void close_frames(Context *ctx, Frames *frames);
bool open_chain(Context *ctx, Swapchain *sc, VkExtent2D pixels, VkSwapchainKHR old);
void close_chain(Context *ctx, Swapchain *sc);
bool draw(Context *ctx, Frames *frames, Swapchain *sc, bool *rebuild);
