#include "probe.h"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <errno.h>

static const char manifest[] = "/Library/GPUBundles/nvmtl/nvk_icd.json";
static const char nvk[] = "/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib";

static bool driver_loaded(void) {
    bool found = false;
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char *path = _dyld_get_image_name(i);
        if (strcasestr(path, "MoltenVK")) {
            fprintf(stderr, "unexpected driver: %s\n", path);
            return false;
        }
        if (strcmp(path, nvk) == 0) found = true;
    }
    REQUIRE(found);
    printf("ICD image %s\n", nvk);
    return true;
}

typedef struct {
    Context *ctx;
    Swapchain *sc;
    Frames *frames;
    unsigned target, presented;
    atomic_bool done, stop;
    bool rebuild;
} Batch;

static int SDLCALL render(void *user) {
    Batch *batch = user;
    bool ok = true;
    while (batch->presented < batch->target && !atomic_load(&batch->stop)) {
        ok = draw(batch->ctx, batch->frames, batch->sc, &batch->rebuild);
        if (!ok || batch->rebuild) break;
        ++batch->presented;
    }
    atomic_store(&batch->done, true);
    return ok ? 0 : 1;
}

static bool pump(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            fputs("window closed before probe completed\n", stderr);
            return false;
        }
    }
    return true;
}

// All SDL video calls and swapchain transitions run on the Cocoa main thread.
// The worker exclusively owns the queue and command pool until joined.
static bool run_worker(Batch *batch) {
    SDL_Thread *worker = SDL_CreateThread(render, "g1-present", batch);
    SDL_TRY(worker);
    while (!atomic_load(&batch->done)) {
        if (!pump()) atomic_store(&batch->stop, true);
        SDL_Delay(1);
    }
    int status;
    SDL_WaitThread(worker, &status);
    return status == 0 && !atomic_load(&batch->stop);
}

static bool batch_run(Context *ctx, SDL_Window *window, Frames *frames, Swapchain *sc,
                      unsigned target, unsigned *total) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (!pump()) return false;
        int w, h;
        SDL_TRY(SDL_GetWindowSizeInPixels(window, &w, &h));
        REQUIRE(w > 0 && h > 0 && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED));
        Swapchain next = {0};
        bool ok = open_chain(ctx, &next, (VkExtent2D){(uint32_t)w, (uint32_t)h}, sc->chain);
        // Creating a replacement can retire the old chain even on failure.
        close_chain(ctx, sc);
        *sc = next;
        if (!ok) return false;
        Batch batch = {.ctx = ctx, .frames = frames, .sc = sc, .target = target};
        ok = run_worker(&batch);
        *total += batch.presented;
        if (!ok) return false;
        if (!batch.rebuild) return batch.presented == target;
        puts("WSI changed; restarting batch with a new swapchain");
    }
    fputs("surface failed to stabilize\n", stderr);
    return false;
}

static bool wait_window_state(SDL_Window *window, bool minimized) {
    const SDL_EventType wanted = minimized ? SDL_EVENT_WINDOW_MINIMIZED : SDL_EVENT_WINDOW_RESTORED;
    const SDL_WindowID id = SDL_GetWindowID(window);
    const Uint64 deadline = SDL_GetTicks() + 5000;
    while (SDL_GetTicks() < deadline) {
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, 50)) continue;
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        if (event.type == wanted && event.window.windowID == id &&
            !!(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) == minimized) return true;
    }
    fprintf(stderr, "window timed out waiting for %s\n", minimized ? "minimize" : "restore");
    return false;
}

static bool exercise(Context *ctx, SDL_Window *window, Frames *frames, Swapchain *sc, unsigned *total, unsigned *cycles) {
    if (!batch_run(ctx, window, frames, sc, 120, total)) return false;
    puts("120 consecutive successful presents and readbacks");
    for (unsigned i = 1; i <= 5; ++i) {
        int w = 320 + (int)i * 24, h = 240 + (int)i * 16;
        SDL_TRY(SDL_SetWindowSize(window, w, h));
        SDL_TRY(SDL_SyncWindow(window));
        int actual_w, actual_h;
        SDL_TRY(SDL_GetWindowSize(window, &actual_w, &actual_h));
        REQUIRE(w == actual_w && h == actual_h);
        if (!batch_run(ctx, window, frames, sc, 1, total)) return false;
        SDL_TRY(SDL_MinimizeWindow(window));
        if (!wait_window_state(window, true)) return false;
        SDL_TRY(SDL_RestoreWindow(window));
        if (!wait_window_state(window, false)) return false;
        if (!batch_run(ctx, window, frames, sc, 1, total)) return false;
        ++*cycles;
        printf("cycle %u: resized to %dx%d, minimized, restored, pixels verified\n", i, w, h);
    }
    return true;
}

static bool run(Context *ctx, unsigned *total, unsigned *cycles) {
    REQUIRE(pthread_main_np() && SDL_IsMainThread());
    int translated = 0;
    size_t size = sizeof(translated);
    REQUIRE(sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) == 0 || errno == ENOENT);
    REQUIRE(!translated);
    if (access(manifest, R_OK) != 0) { perror(manifest); return false; }
    REQUIRE(setenv("VK_DRIVER_FILES", manifest, 1) == 0);
    Dl_info loader;
    REQUIRE(dladdr((void *)vkGetInstanceProcAddr, &loader) && loader.dli_fname);
    printf("loader %s\nmanifest %s\n", loader.dli_fname, manifest);
    SDL_TRY(SDL_Init(SDL_INIT_VIDEO));
    bool loaded = false, ok = false;
    SDL_Window *window = NULL;
    Frames frames = {0};
    Swapchain sc = {0};
    if (strcmp(SDL_GetCurrentVideoDriver(), "cocoa")) {
        fprintf(stderr, "requires Cocoa, got %s\n", SDL_GetCurrentVideoDriver());
        goto cleanup;
    }
    int version = SDL_GetVersion();
    printf("SDL %d.%d.%d, native x86_64 Cocoa main thread\n",
        SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version));
    loaded = SDL_Vulkan_LoadLibrary(loader.dli_fname);
    if (!loaded) { fprintf(stderr, "SDL_Vulkan_LoadLibrary: %s\n", SDL_GetError()); goto cleanup; }
    if ((PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr() != vkGetInstanceProcAddr) {
        fputs("SDL and executable resolved different Vulkan loaders\n", stderr);
        goto cleanup;
    }
    window = SDL_CreateWindow("mkw-recomp G1 probe", 320, 240,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); goto cleanup; }
    if (!open_instance(ctx)) goto cleanup;
    if (!SDL_Vulkan_CreateSurface(window, ctx->instance, NULL, &ctx->surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        goto cleanup;
    }
    ok = open_device(ctx) && driver_loaded() && open_frames(ctx, &frames) && exercise(ctx, window, &frames, &sc, total, cycles);
cleanup:
    if (ctx->device) {
        VkResult idle = vkDeviceWaitIdle(ctx->device);
        if (idle != VK_SUCCESS) { fprintf(stderr, "vkDeviceWaitIdle: %d\n", idle); ok = false; }
        close_chain(ctx, &sc);
        close_frames(ctx, &frames);
    }
    close_context(ctx);
    if (window) SDL_DestroyWindow(window);
    if (loaded) SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    Context ctx = {0};
    unsigned total = 0, cycles = 0;
    bool ok = run(&ctx, &total, &cycles) && atomic_load(&ctx.errors) == 0;
    printf("%s: %u successful presents with readback, %u cycles, %u validation errors\n",
        ok ? "PASS" : "FAIL", total, cycles, atomic_load(&ctx.errors));
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
