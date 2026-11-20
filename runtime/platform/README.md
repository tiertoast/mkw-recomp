# runtime/platform

| File | What it is |
|---|---|
| `darwin_thread.c`, `darwin_sync.c` | Native pthread handles, monotonic time, shared/exclusive locks and condition variables on public Darwin address waits |
| `darwin_fiber.c`, `darwin_switch.S`, `darwin_context.h` | Guarded movable x86_64 fiber stacks, Mach-O context switch and same-fiber nonlocal guest returns |
| `darwin_mem.c` | Mach VM exact reservations, page-state ledger, demand-zero commit and atomic replacement on decommit. The write watch of watched reservations (`MKW_WRITE_WATCH=1`, memory.c): clean read-write pages are read-only in fact, the first write faults into its handler, which makes the page writable and marks it written; a page rewritten about every frame turns hot (writable, reported at every query); kernel writes are bracketed (`plat_mem_will_write` / `plat_mem_wrote`); other protected accesses follow the OS fault disposition |
| `darwin_process.c` | Process identity, self-spawn and re-exec, randomness and dyld lookup |
| `darwin_time.c` | Wall time and standard offset through libc, system zone and preferences through CoreFoundation |
| `darwin_crash.c` | Alternate signal stacks and terminal diagnostics for faults inside the executable; the faults the write watch's handler does not own are passed on here |
| `darwin_load.c` | Mach CPU and memory counters and optional IOKit GPU statistics |
| `darwin_ipc.c` | Local stream sockets with nonblocking connect and receive |
| `darwin_desktop.c` | SDL clipboard, Finder URLs, CoreText outline fonts and executable desktop shortcuts |
| `darwin_file.c` | Darwin file I/O, append logging, file locks, atomic replacement and save filesystem operations |
| `platform.h` | The operating system interface the rest of the runtime uses |
| `linux.c`, `linux_int.h` | Linux: threads, preemption through signals, fibers; the header is what the `linux_*.c` parts share |
| `linux_sync.c` | Linux: clock, locks and condition variables on futex |
| `linux_fs.c` | Linux: files and file system (atomic write, rename without replace) |
| `linux_mem.c` | Linux: virtual memory, write watching with userfaultfd (Linux 6.7+), fault handler; `plat_mem_poke` writes through the protection via `/proc/self/mem` |
| `linux_sys.c` | Linux: process, local IPC (Unix sockets), desktop, libraries, machine load (CPU, temperature, GPU) and desktop shortcut |
| `win32.c`, `win32_int.h` | Windows: threads, preemption (SuspendThread), fibers, clock, locks; the header is what the `win32_*.c` parts share |
| `win32_fs.c` | Windows: files and file system (UTF-16 paths) |
| `win32_mem.c` | Windows: virtual memory and write watching (`MEM_WRITE_WATCH`) |
| `win32_sys.c` | Windows: process, local IPC (named pipes), desktop and libraries |
| `win32_load.c` | Windows: machine load (CPU, PDH for temperature and GPU) and desktop shortcut |
| `common.c`, `fp_x86.h` | Common parts, x86 floating point control |
| `mkw.rc`, `mkw.manifest` | `mkw.exe`'s manifest (UTF-8 code page, from totk-pc-recomp); `build.py --target windows` |
