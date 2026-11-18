# Darwin x86_64 runtime-only link

`CMakeLists.txt` builds the runtime source list from `build.py` and links it with the versioned Mach-O host ABI
library. The build generates the default paths, UI catalogs, icon and module ID. It requires no game data or
generated game code.

Use native x86_64 Command Line Tools, CMake 3.24+, Ninja, the SDL3 framework with its CMake package, and a LunarG
Vulkan SDK. The other x86_64 dependencies are Opus, libvpx and zstd plus the VMA 3.4.0 and xxHash headers.
Supply the framework's parent and the native dependencies prefix as a semicolon-delimited CMake list.

```sh
CMAKE_PREFIX_PATH="/path/to/frameworks;/path/to/native/deps" VULKAN_PREFIX="/usr/local" \
  uv run tools/build/build.py --target macos-x86_64 --runtime-only --jobs 8
```

The candidate binary is `build/dist/macos-x86_64/mkw`. Use `--out` for a separate build directory.
The current build reaches ld with the full runtime source list. The Darwin filesystem, process and crash
services and the remaining platform backends still have unresolved references. The executable requires those
backends before it can be linked.
