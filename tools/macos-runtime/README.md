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

The developer build links the launcher's setup sources and Python environment into its output directory.
Build using the repository's Python environment with its setup dependencies installed. Keep that environment and
checkout available. Setup uses xcrun's Apple Clang and SDK. The selected installation receives mkwgame.dylib and
mkwgame.id, while the versioned host library stays beside mkw. To validate module loading alone, run
`MKW_GAME_DIR=/path/to/installation /path/to/mkw --probe-game-module`.

For the subsequent shader stage, supply a validated SM86 instructions.json in build/sm86 in the checkout or
sm86 in the installation, plus native glslc. The spec generator's pinned NVIDIA tools target Linux and Windows.

A Linux or Windows setup using the repository's pinned CUDA 12.8 tools produces the same input format:
UTF-8 JSON from sm_86_instructions.txt, parser schema v2.6. Copy only instructions.json to one of the
two paths above. Record the producing checkout and compare SHA-256 on both machines (Get-FileHash,
sha256sum or shasum -a 256). The setup checks the decoder model and parser counts before starting the
copy or compilation. These checks detect incompatible or incomplete catalogs; they cannot authenticate
the generating machine. Keep NVIDIA-derived data outside git.
