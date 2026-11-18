# macOS x86_64 Vulkan presentation probe

Build this standalone target from the repository root on a native x86_64 Mac. Run it in a logged-in graphical
session with the window visible. It uses synthetic clears and reads three pixels of the acquired swapchain image
before each present. The runtime and game are independent of this build.

The tested baseline is macOS 26.7.2, SDL 3.4.18 and the LunarG 1.4.363 loader, headers and Khronos validation layer.
Install the NullMoth NVK ICD at `/Library/GPUBundles/nvmtl/nvk_icd.json`. For SDL, extract
`macos-arm64_x86_64/SDL3.framework` from the official SDL3.xcframework. Set `SDL3_FRAMEWORK_DIR` to the
directory containing that framework. Set `VULKAN_PREFIX` to the SDK prefix containing `include/vulkan` and
`lib/libvulkan.1.dylib`. If the layers are outside the loader's search paths, set `VK_LAYER_PATH` to the
SDK's `share/vulkan/explicit_layer.d`.

```sh
cmake -S tools/macos-probe -B build/macos-probe -G Ninja \
  -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)" \
  -DSDL3_FRAMEWORK_DIR="$SDL3_FRAMEWORK_DIR" -DVULKAN_PREFIX="${VULKAN_PREFIX:-/usr/local}"
cmake --build build/macos-probe
ctest --test-dir build/macos-probe -V
```

CTest gives the process 60 seconds. Success requires a batch of 120 consecutive presentations, then one after
each of five resizes and one after each restore. The worker uses a timeline semaphore for command completion,
a present fence for resource reuse and a present ID for display progress. The last two checks require
`VK_KHR_swapchain_maintenance1`, `VK_KHR_present_id` and `VK_KHR_present_wait` on the probe device.
The main thread pumps Cocoa events while the worker renders. It observes both the SDL event and minimized
flag before advancing each window transition. Each rebuilt swapchain must match the drawable size in pixels.

The process selects the NVK manifest before initializing SDL and passes the linked loader's path to SDL.
It checks the shared entry point, driver ID and mapped driver image. A missing runtime feature, failed pixel check,
lost surface/device, timeout, validation error or early close fails the run. Out-of-date or suboptimal images
restart the interrupted batch with a fresh swapchain, for up to three attempts. The result is printed after teardown.

| File | Responsibility |
|---|---|
| `CMakeLists.txt` | Mach-O target, SDL framework, SDK loader and local CTest entry |
| `probe.h` | Resource ownership and interfaces between window, device and frame code |
| `main.c` | Loader identity, Cocoa event loop, window transitions, worker lifetime and final result |
| `device.c` | Validated instance, SDL surface queue selection, runtime extension and feature contract |
| `frames.c` | SDR FIFO swapchain, submit2, timeline, present completion and pixel comparison |
