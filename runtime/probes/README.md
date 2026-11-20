# runtime/probes

Self-tests of each subsystem, one command-line option each (`build/mkw --probe-…`). They come from totk-pc-recomp
except `texpre_probe.c`, `fg_probe.c`, `fps_probe.c`, `discord_probe.c`, `clk_probe.c` and `dsu_probe.c`.

`vk_sparse_probe.c` is a standalone Vulkan diagnostic, built independently by this folder's `CMakeLists.txt`.
Unlike the runtime probes below it needs no game setup, guest memory, SDL, VMA or shaders. The normal runtime's
recursive source discovery compiles it without its standalone definition, so it contributes no entry point.

```sh
cmake -S runtime/probes -B build/vk-sparse -DVulkan_ROOT=/usr/local -DCMAKE_OSX_ARCHITECTURES=x86_64
cmake --build build/vk-sparse
VK_DRIVER_FILES=/Library/GPUBundles/nvmtl/nvk_icd.json build/vk-sparse/vk-sparse-probe --application-shape
# Independent control with two tiled mips, without mip-tail backing:
VK_DRIVER_FILES=/Library/GPUBundles/nvmtl/nvk_icd.json build/vk-sparse/vk-sparse-probe --tiled-control
# Independent large control: 68 layers, 5 mips, no opaque bindings:
VK_DRIVER_FILES=/Library/GPUBundles/nvmtl/nvk_icd.json build/vk-sparse/vk-sparse-probe --large-tiled-control
# Resource-size control: 32 layers, 5 tiled mips, within the declared resource limit:
VK_DRIVER_FILES=/Library/GPUBundles/nvmtl/nvk_icd.json build/vk-sparse/vk-sparse-probe --resource-size-control
# 32-bit layer-offset discriminator (G5-L), with NVK's VA log:
NVK_DEBUG=vm VK_DRIVER_FILES=/Library/GPUBundles/nvmtl/nvk_icd.json build/vk-sparse/vk-sparse-probe --wrap-discriminator
# Copies that touch unbound tiles of a small image (G5-L, H2); may end in a device loss by design:
NVK_DEBUG=vm VK_DRIVER_FILES=/Library/GPUBundles/nvmtl/nvk_icd.json build/vk-sparse/vk-sparse-probe --unbound-access
# Partial unbinds and binds over bound tiles, as the renderer issues them (G5-L):
NVK_DEBUG=vm VK_DRIVER_FILES=/Library/GPUBundles/nvmtl/nvk_icd.json build/vk-sparse/vk-sparse-probe --rebind-patterns
```

Or compile directly with Apple Clang:

```sh
mkdir -p build/vk-sparse
clang -std=c11 -Wall -Wextra -Werror -DMKW_STANDALONE_SPARSE_PROBE -I/usr/local/include \
  runtime/probes/vk_sparse_probe.c -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lvulkan \
  -o build/vk-sparse/vk-sparse-probe
```

The caller selects the existing ICD; the probe never changes it and requires an NVK device. Khronos validation,
debug utils and synchronization validation are mandatory (set `VK_LAYER_PATH` if the SDK needs it). No GPU work
is registered with CTest or CI. Run with an external 60-second process timeout as well as the internal 5-second
fence waits: a driver entry point itself can block. Output is unbuffered. Exit codes are 0 PASS, 1 FAIL, 77 SKIP,
and 2 for invalid arguments. Stop at the first failing Vulkan result, validation error, malformed requirement,
timeout or data mismatch; on failure process teardown releases resources without further device waits.

The default case is a fully resident 512x512 BC7 UNORM array, 4 layers, 10 mips. Both sparse-requirements APIs
are cross-checked and mip-tail ranges checked before any allocation/bind. Separate checkpoints cover standard
`vkAllocateMemory`, compatible backing-buffer zero fill, tile binds, opaque mip tails (including metadata when
reported), image initialization, layout transitions and per-mip upload/readback of two generated BC7 mode-6
patterns. Every queue operation uses a chained timeline semaphore and an independently checked fence. Transfers
compare compressed bytes; shader sampling and decoded colour are outside this test.

`--application-shape` runs the small case first, then, only if it passes, queries/creates 8192x8192 with 68 layers
and 14 mips. The large case binds one tile per non-tail mip/layer, copies at most a 32x32 patch within that tile,
and fully backs/transfers the mip tails. Other tiles stay untouched. It compares the application's virtual shape
without allocating its full residency. `--tiled-control` instead runs only 512x512, 4 layers, 2 mips; it is an
explicit independent diagnostic, never an automatic fallback or a renderer setting.

`--large-tiled-control` runs only 8192x8192, 68 layers and 5 mips, independently of the small/full-chain cases.
Before allocation or queue work it checks every sparse requirement: all requested color mips must precede the
mip tail, and no metadata aspect may require opaque backing. Otherwise it returns 77 with the reason. It then
uses the unchanged requirements-conformity checks, binds one tile at the origin of each mip/layer and transfers
a patch up to 32x32 within each tile. No opaque binding is submitted and all other tiles remain untouched.
Compare its log with a previous small control; invoking it does not rerun the small control. A special driver
tail offset is unused when no tail needs backing. Range/alignment checks remain enforced for required tails.
Logs classify requirements incompatibility as `REQUIREMENTS_CONFORMANCE`, separately from `GPU_OPERATION`,
`GPU_DATA_MISMATCH`, `VALIDATION`, other API results and probe checks. A conformity failure alone does not prove
a GPU operation failed or explain a device loss in the application.

`--resource-size-control` runs only 8192x8192, 32 layers, 5 mips with the same partial residency, no-tail
eligibility, synchronization, layout and synthetic upload/readback checkpoints as the 68-layer tiled case.
Every image mode now logs the theoretical BC7 payload (16 bytes per 4x4 block, all requested mips/layers),
`maxResourceSize` and, after creation, `VkMemoryRequirements.size`. It returns an explained SKIP before
creation if the payload exceeds the declared resource/sparse-address-space limit, or before allocation and
queue work if the Vulkan requirements exceed either limit. Extent, layer, mip and sample limits are checked
as well. Thus a formerly accepted 68-layer case can now SKIP on a device declaring a smaller resource limit.
The allocation preflight separately logs planned backing and both staging buffers against the 128 MiB cap;
per-heap and reported-free-budget checks remain enforced before every allocation. Resource size, virtual
address reservation and physically allocated resident bytes are reported separately.

`--wrap-discriminator` tests one hypothesis for the 68-layer device loss. Mesa NVK 26.3.0-rc3 computes the layer
address of copy-engine copies as `(layer) * array_stride` in 32-bit arithmetic (`nvk_cmd_copy.c`), while sparse
binds use 64-bit offsets. With 8192x8192, 68 layers and 5 mips, the layer stride is the requirements' size over the
layers. This is checked against the levels' whole tiles, and the run skips before GPU work if they differ. The
probe takes the first layer whose offset reaches 2^32 and the layer just below it as the control. It predicts where
a wrapped copy would land, then binds the control tile, the wrap tile and every tile of the level that holds the
predicted address. It copies a distinct 32x32 marker into each of the two layers and finds the markers by copying
all backing memory through a buffer bound to it, with no image addressing. If the control marker is not in its own
tile, the run stops before the wrap copy. `VERDICT WRAPPED_LAYER_OFFSET`, `NO_WRAP`, `NOT_FOUND` or `UNEXPECTED`
reports where the wrap marker went; a device loss means it reached memory outside every bound tile. This is the
only mode that intentionally exceeds `maxResourceSize` (`INTENTIONAL_OVER_LIMIT`); the sparse address-space limit
still applies. With `NVK_DEBUG=vm`, NVK logs the image's base VA, and the `PREDICT` lines give the wrapped and
unwrapped addresses relative to it. Backing is about 4 MiB for this shape (one level), and its readback the same.

`--unbound-access` tests whether a copy that touches an unbound tile faults. Vulkan allows it on a sparse-residency
image: with `residencyNonResidentStrict` reads are zero and writes are discarded, and without it the access is still
safe, so a device loss is out of spec either way. The image is 512x512 BC7 with one mip and four tiles, far from 4 GiB
offsets, mip tails and `maxResourceSize`. Only tile (0,0) is bound. A marker written there and read back through both
the image and the backing is the control. Then come a read, a write and a second read of never-bound tile (1,1).
Tile (0,0) is then unbound, as the renderer does when an NVN pool page is unmapped, and read and written. Each access
is its own submission, so the checkpoint of a device loss names it. After each write, the backing is read through a
buffer to see whether the write reached memory. `VERDICT UNBOUND_ACCESS_STRICT`, `UNBOUND_ACCESS_VIOLATES_STRICT` or
`UNBOUND_ACCESS_NO_FAULT` sums up the values, and a `NOTE` says if the unbound tile still reached its former memory.
Only the copy engine is exercised: sampling an unbound tile in a shader is outside this shader-free probe.

`--rebind-patterns` covers the bind sequences `vk_sparse.c` issues that the other modes do not. The renderer binds
merged runs of blocks, later unbinds single blocks out of a run, and moves a block to other memory with no unbind in
between, which Vulkan allows. The image is 512x512 BC7, one mip, 2x2 tiles. One bind backs the whole level from memory A
and a marker goes into each tile. Then come, each in its own queue operation: unbinding tile 1 alone, unbinding tile 2,
moving tile 3 to memory B and writing it, and unbinding tile 0. After each step every tile is read through the image.
A failing bind names its checkpoint. Wrong values do not stop the run, so `VERDICT REBIND_PATTERNS_OK` or
`REBIND_PATTERNS_WRONG_DATA` reports them all. Unbound tiles must read zero, so run it on a driver whose unbound pages
do not fault (`--unbound-access` passes), or a fault there hides the bind results.

Allocations are capped at 128 MiB total, 10% of each heap, and a quarter of the currently reported free budget
when `VK_EXT_memory_budget` is available. The large plan is skipped if its estimated backing/staging exceeds the
cap; later allocation-budget failures stop the run. Requested live/peak allocation bytes, heap sizes and optional
driver budget/usage are printed; these do not measure all driver overhead. Flags match the sparse renderer
(`SPARSE_BINDING`, `SPARSE_RESIDENCY`, `SPARSE_ALIASED`), but the test uses unique memory for each tile, no
NVN virtual pools, no actual tile sharing/rebinding and no concurrent upload batch. It takes API granularity
verbatim rather than applying `vk_sparse.c`'s vendor compatibility conversion. Machine-specific results and
investigation logs belong in the PR, not in this directory.

| File | Option | What it checks |
|---|---|---|
| `astc_probe.c` | `--probe-astc` | The compute ASTC decoder (`vk_astc.c`) against the CPU one, bit for bit, with synthetic blocks; the BC7 and BC1 re-encoders, transparent texels keeping alpha 0 exactly |
| `audio_probe.c` | `--probe-audio` | MKW's own: the SDK's translated `nn::audio` client against the objects of `audren_sf.c`: parameter, open, 200 updates at 5 ms, device and close (formerly `--probe-ipc`) |
| `cfg_probe.c` | `--probe-cfg` | Reading the `.ini`, environment variables and a missing file (`cfg.c`) |
| `fg_probe.c` | `--probe-framegen` | MKW's own: frame generation's passes (`vk_framegen.c`) on two pictures in `MKW_FG_PROBE` (from `tools/imagen/fg_check.py`), the frame halfway and the vectors of each level written back, and its GPU time |
| `discord_probe.c` | `--probe-discord` | MKW's own: Discord Rich Presence (`core/presence`) against the Discord running on this machine: connects, shows each phase a few seconds, clears it |
| `dsu_probe.c` | `--probe-dsu` | MKW's own: the Cemuhook / DSU motion client (`input/dsu.c`) against a fake server on the loopback: the request's header and CRC, the axes and units in the Switch frame, the fusion turning a quarter turn a second, a bad CRC ignored, the samples back after the server quits and starts again, `MKW_DSU=0` live; with `MKW_DSU_SERVER` set, against that server instead |
| `fps_probe.c` | `--probe-fps` | MKW's own: `MKW_FPS=auto`'s controller (`fps_auto.c`) on synthetic frame times: the interval each steady frame time settles on with no changes after, a loading hitch, a return to 60, stalls slower than any interval |
| `clk_probe.c` | `--probe-gpu-clocks` | MKW's own: whether the driver lowers the GPU's clock in the gaps the race's chain leaves it idle: a fixed compute batch after CPU gaps of 0 to 50 ms, its GPU time (and amdgpu's core clock); with the runtime's device, so `MKW_GPU_CLOCKS` applies, and `MKW_CLK_FILL` keeps the GPU busy a share of each gap |
| `gfx_abi_probe.c` | `--probe-gfx-abi` | Our objects fit in NVN's and the fields the game reads are where it expects them |
| `gfx_window_probe.c` | `--probe-window` | Window present, vsync, focus, cursor and resize |
| `kernel_probe.c` | `--probe-kernel` | Waits, wakeups, virtual cores and their arguments (cores 0-5), fibers (also one moved between threads) and translated code on four threads |
| `nvn_probe.c` | `--probe-nvn` | The NVN front end over the null back end, called as the game calls it |
| `nvn_tex_probe.c` | `--probe-nvn-tex` | Size, alignment and mip offsets of textures (`nvn_tex.c`) |
| `ov_probe.c`, `ov_probe.h` | `--probe-services` | Each replacement of `runtime/sdk/` and NVN2's native images called through its synthetic address; a linked one the loaded game imports and no probe calls fails it |
| `ov_mkw_probe.c`, `ov_mkw_list.h` | `--probe-services` | MKW's own: the replacements only MKW imports (offline network, account, `oe`, clock, touch, the VP9 decoder with frames the test encodes itself, and sockets over the host's loopback) |
| `tex_probe.c` | `--probe-vkmem` | Texture memory: block-linear copies, ASTC, format table, imported pools; with `MKW_GPU_MEM_LIMIT_HEAP<n>=1` on the device-local heap, a physical pool backed outside it (#443); sparse block sizes given in texels or 4x4 blocks (#476) |
| `texpre_probe.c` | `--probe-texpre` | MKW's own: quality of the GPU BC7 against `bc7enc` on the game's textures, listed in `MKW_TEXPRE_PROBE_LIST`: PSNR of RGBA and of premultiplied colour (what a blend shows) |
| `vk_exec_probe.c` | `--probe-vkexec` | Vulkan replay: fence values, completion order, empty submits; `MKW_RTSTATS`'s rules on cleared targets; a pitch texture the GPU writes, in its pool bytes after the fence wait; which render targets count as rendered recently (one-shot draws); unwritten sparse small mips start at zero across allocation reuse and retain later writes |
