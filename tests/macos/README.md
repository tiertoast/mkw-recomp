# Darwin foundations

On macOS 26.7.2 x86_64 with the Command Line Tools, CMake 3.24+ and Ninja, run from the repository root:

```sh
uv run tools/build/build.py --target macos-x86_64
ctest --test-dir build/macos-x86_64-g2a --output-on-failure
```

Use the build.py entry point after changing headers or toolchain settings. It recomputes the module ID, configures
the active SDK and builds incrementally. `--out DIR`, `--jobs N`, `--dry-run` and `--frame-pointers` are supported.
The build directory and module fingerprint distinguish Darwin x86_64, its Mach-O host ABI major version,
the host export list, binding declarations and the existing common header/flags ID.

## Module and registry contract

The runtime executable contains the real host service bodies with private visibility. Before opening a module,
the loader installs an immutable table of those addresses in `@rpath/libmkw-host.1.dylib` through
`mkw_bind_host_v1`. This host library exposes the seven C symbols listed in `build.py`'s `RT_EXPORTS`.
Changing the calling convention or the shape of the binding table requires a new host ABI major version.

A module links against that dylib using Mach-O two-level names. Its install name is `@rpath/mkwgame.dylib`.
Its only export, `mkwgame_tables`, returns an `RtGame` with matching ABI and ID. The fixture keeps the host library
beside the executable and resolves it through `@loader_path`. The loader opens the module from the selected
directory using `RTLD_LOCAL | RTLD_NOW | RTLD_FIRST`, checks the entry in that image, and retains accepted tables
for the process lifetime. A readiness check can read `mkwgame.id` before opening any library.

Declarations reside in the executable's `__DATA_CONST,__mkw_ovr`, `__mkw_obs` and `__mkw_fns` sections. The
linker's relocated start/end symbols preserve link order and bracket empty tables. Include archive members containing
only registrations with targeted whole-archive linkage. Registry construction/sealing precedes dispatch by workers.

## Coverage

| File | Test |
|---|---|
| `CMakeLists.txt` | Builds fixtures with dead stripping, links the versioned host ABI, and registers the four local CTest cases |
| `fixture.c`, `declarations.cpp` | Concurrent module discovery, duplicate-name precedence, three real registry tables across C and an archived C++ object, seal, synthetic address and dispatch |
| `module.c` | One synthetic address calls through `rt_sj_owner`, re-enters its C body and dispatches to the registered host function |
| `empty.c` | Empty linked override and observer table bounds with the production lookup functions |
| `check.py` | Mach-O slices, two-level imports, exported symbols, section presence, relocation, readiness and rejection of wrong architecture, ABI, ID or entry |
| `threads.c` | Thread names and stack, timed join followed by join, detach, event and condition signaling, shared/exclusive contention, recursive ownership and once |
| `support.c` | Fixture configuration, diagnostics and anonymous pages for the dispatch table. Entry into an unavailable scheduler or service fails the process |

A public thread handle has one owner. Join and detach consume the handle. A timed-out join keeps it valid. Timed
join tests completion of the entry function, so subsequent pthread TLS destructor work consumes none of its timeout.
The inline Darwin lock and condition state supports zero initialization and the common API's object lifetime without
a heap allocation or a destroy operation. Timed waits use a monotonic clock.

The full runtime still needs guest VM reservations and coherence, fibers and scheduler integration, the general
Darwin filesystem/process/service and diagnostic backends, and game renderer/shader/setup integration. G2-A builds
only the named native sources. The fixture provides no guest kernel, VM backend, shader translator bootstrap or
generated game module.
