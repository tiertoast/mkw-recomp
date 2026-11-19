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

The executable is `build/dist/macos-x86_64/mkw`. Use `--out` for a separate build directory. The runtime requires
the adjacent host ABI library and the dependency prefixes used at build time. `Info.plist` embeds the Bluetooth
usage description in the executable for opt-in Switch 2 controller discovery.

For a controlled startup without a game, use an empty directory for `MKW_GAME_DIR`, set `VK_DRIVER_FILES` to the
NullMoth NVK manifest, and run `MKW_LAUNCHER_SHOT=3:settings /path/to/mkw --launcher`. The launcher renders the
settings page, writes `launcher.png` in the current directory and quits.
