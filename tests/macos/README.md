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
| `CMakeLists.txt` | Builds fixtures with dead stripping, links the versioned host ABI, and registers the native CTest cases |
| `fixture.c`, `declarations.cpp` | Concurrent module discovery, duplicate-name precedence, three real registry tables across C and an archived C++ object, seal, synthetic address and dispatch |
| `module.c` | One synthetic address calls through `rt_sj_owner`, re-enters its C body and dispatches to the registered host function |
| `empty.c` | Empty linked override and observer table bounds with the production lookup functions |
| `check.py` | Mach-O slices, two-level imports, exported symbols, section presence, relocation, readiness and rejection of wrong architecture, ABI, ID or entry |
| `threads.c` | Thread names and stack, timed join followed by join, detach, event and condition signaling, shared/exclusive contention, recursive ownership and once |
| `support.c` | Fixture configuration and diagnostics. The dispatch table uses native Darwin VM. Entry into an unavailable scheduler or service fails the process |

A public thread handle has one owner. Join and detach consume the handle. A timed-out join keeps it valid. Timed
join tests completion of the entry function, so subsequent pthread TLS destructor work consumes none of its timeout.
The inline Darwin lock and condition state supports zero initialization and the common API's object lifetime without
a heap allocation or a destroy operation. Timed waits use a monotonic clock.

## Guest VM

`vm.c` links the actual guest layout, heap, physical mapping, stack move and QueryMemory code. Its configuration
and single synthetic module are supplied by `vm_layout.c`, shared with the Vulkan fixture. A foreign Mach page occupies part of the first low-window candidate.
A separate run forces the fixed module collision through the runtime's re-exec path. The test exercises the page-state
boundaries, RSS across a heap commit/touch/decommit, repeated commits, zero-fill, eight concurrent faults at distinct
bytes of the same read-only page and eight workers repeatedly cycling page states. `faults.py` additionally checks the
OS signal from each forbidden access in its own process. The signal observer used by the eight workers exists only
in the test executable.

Darwin reserves at the requested address with no overwrite, initial protection NONE and maximum RW. Each mapping
lives until process exit. A mutex serializes the ownership ledger and the Mach operations. Decommit replaces only a
subrange of one owned guest reservation with fresh anonymous NONE pages. A repeated commit preserves its data.
A physical guest alias uses the runtime's existing mapping rules. Moving an alias page to a stack copies its bytes,
makes the source inaccessible and copies the bytes back on unmap.

Darwin leaves guest protection faults to the installed OS disposition. `PLAT_CAN_RETRY_FAULT` is zero. Freeze,
read traps and diagnostic write watches therefore never arm pages. `plat_mem_poke` returns zero, including on
a writable address. `plat_mem_peek` uses a checked Mach read with the current protection. Nonempty dirty and
population queries with output capacity return `UINT64_MAX` for unknown. Consumers must upload the entire relevant
range, or decline facilities that require a populated-page census. Demand paging supplies zero on first access.
The optional placed Vulkan mapping experiment cannot take ownership of a Darwin guest reservation.

`vm_vulkan.c` is an opt-in LunarG/NullMoth NVK buffer fixture. It requests Khronos synchronization validation,
checks the driver ID, stages a full guest range to a device-local buffer, changes a word on the GPU, waits for its
fence and checks every readback byte. Copying the known GPU-written word back also preserves the post-submit
CPU update to its neighbouring byte. It reports the memory properties of the staging allocations. This test reserves the full guest layout after loading NVK, maps its staging source with
`mem_map_physical` and checks the runtime dirty-query and read-trap fallbacks. Full renderer shadow merging and sparse residency need their own integration tests.

```sh
uv run tools/build/build.py --target macos-x86_64 --out build/g2b
cmake -S tests/macos -B build/g2b -DMKW_TEST_NVK=ON -DVULKAN_PREFIX=/usr/local
cmake --build build/g2b
ctest --test-dir build/g2b -V
ctest --test-dir build/g2b -R '^darwin-vm$' --repeat until-fail:30
env CFLAGS=-fsanitize=address,undefined CXXFLAGS=-fsanitize=address,undefined LDFLAGS=-fsanitize=address,undefined \
  uv run tools/build/build.py --target macos-x86_64 --out build/g2b-asan
cmake -S tests/macos -B build/g2b-asan -DMKW_TEST_NVK=ON
cmake --build build/g2b-asan
env UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/g2b-asan -V
env CFLAGS=-fsanitize=thread CXXFLAGS=-fsanitize=thread LDFLAGS=-fsanitize=thread \
  uv run tools/build/build.py --target macos-x86_64 --out build/g2b-tsan
ctest --test-dir build/g2b-tsan -R '^darwin-vm$' --repeat until-fail:10
```

## Cooperative guest kernel

`darwin-kernel` links the existing six-core arbiter, guest mutex and thread waits, fiber host overrides and
scheduler diagnostics with the Darwin platform archive. Darwin defaults to `MKW_ASYNC_PREEMPT=2`. A request
for 1 also runs cooperatively. Zero retains the explicit opt-out from inheritance and the TLR disable protocol.
The core gate uses the existing settings and masks. The cooperative policy runs without scanning module addresses.
A checkpoint defers handoff while TLR+0x100 is set. After the SDK clears it, SVC 0x36 honors the pending handoff.
CPU-only code without a checkpoint can keep its guest core indefinitely.

`fibers.c` and `fiber_regs.S` exercise guarded demand-paged stacks, all six SysV preserved integer registers,
entry alignment, x87 rounding, MXCSR control and flags, and A→B→A pthread migration. The 512-byte FXSAVE64 image
belongs to the fiber. AVX upper lanes are caller-saved at the switch call. TLS and signal masks belong to the
executing pthread. A guest nonlocal return to a live owner frame uses the Mach-O continuation in
`darwin_switch.S`. ExitThread unwinds on the current worker's base stack.

`scheduler.c` uses a synthetic builtin module, real host registration/dispatch, the G2-B guest layout and the
production kernel SVC handler. Three workers contend for the same guest core. The test checks inheritance from
an actual mutex waiter, the protected checkpoint and SVC 0x36, peer yield, timed wait, wakeup, exit and core
release. Its guest-fiber variant moves a suspended host call and a live nonlocal-return frame to another guest
thread with core diagnostics enabled.

```sh
uv run tools/build/build.py --target macos-x86_64 --out build/g2c
ctest --test-dir build/g2c -R '^darwin-(guest-fiber|fiber-context)$' --repeat until-fail:10 --output-on-failure
ctest --test-dir build/g2c -R '^(darwin-scheduler|darwin-fiber-guard|macho-roundtrip|darwin-vm)$' --output-on-failure
```

For an ASan+UBSan or TSan build, run `ctest -R '^darwin-scheduler$'` against its build directory.
The assembly stack switches have no ASan/TSan annotations; those cases report a skip when instrumented.
The complete runtime still needs its remaining platform services, renderer integration and module setup.
