// Native x86_64 guest mappings. Reservations are never released: ownership stays valid across decommit.
//
// Write watch (plat_mem_written) for reservations made with watch set. Darwin has no kernel write watch: neither
// Linux's asynchronous userfaultfd nor Windows' GetWriteWatch, and the kernel's dirty bit (mach_vm_page_range_query)
// cannot be cleared. So a watched page that is read-write to the caller and not written since its last report is
// read-only in fact; the first write faults into ww_fault, which makes the page writable, marks it written and lets
// the write run again. A report with reset clears the mark first and then protects the page again: a write between
// the two lands before plat_mem_written returns, so the caller reads it; every later write faults and is reported next
// time. (Protecting first and clearing after loses a write that faults and completes between the two.) The kernel
// writing into a protected page (read(2) into a guest buffer) fails with EFAULT instead of faulting: callers put
// plat_mem_will_write and plat_mem_wrote around those calls (memory.c mem_kernel_write). In between the pages are busy:
// a report lists a written one once and clears it, as the contract says (vk_mem_gen.c queries a range again while the
// answer is full, so a page reported again and again never lets it end: 20/11/2026, 369 859 queries a frame), but
// leaves it writable, UNARMED; plat_mem_wrote marks the unarmed pages written again, so the kernel's bytes and any
// write in between are reported, and protected, at the next report. Without the pair a report on another thread
// protected a page between the announcement and the read (first races, 20/11/2026: "RomFS: read ... refused").
// A commit is not a write, as on Linux and Windows.
#include "platform.h"
#include "darwin_context.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>

#define PAGE_BYTES 4096ull
#define GUEST_END (1ull << 39)
#define RW (VM_PROT_READ | VM_PROT_WRITE)

// Sorted by base; ww_fault walks the list without the lock, so a range is linked only once complete and never freed.
// Zero in the per-page array means reserved, including untouched pages of large windows.
typedef struct Range {
    struct Range *next;
    uint64_t base, end;
    int guest, watch;
    unsigned char *written;   // watch: per page CLEAN, WRITTEN since the last report with reset, UNARMED or HOT
    uint16_t *busy;           // watch: per page, kernel writes in flight (lock held)
    unsigned char *quick;     // watch: per page, quick faults in a row (QUICK_MS)
    uint32_t *armed_at;       // watch: per page, the ms (now_ms) it was last protected again; HOT: reports since
    unsigned char state[];
} Range;
// CLEAN: protected, not written since. WRITTEN: written, writable. UNARMED: reported while a kernel write is in flight,
// writable until it ends. HOT: rewritten about every frame, so it stays writable and every report lists it, as written
// or not; a fault and a protection call per frame cost ~10 times the copy that the callers make of it then (G5-L races,
// 20/11/2026: 52-72% of ~6000 faults a frame while driving came < 40 ms after the page was protected again).
enum { CLEAN, WRITTEN, UNARMED, HOT };
#define QUICK_MS 40       // a fault this soon after the page was protected again is quick: a page rewritten every frame
#define HOT_QUICK 2       // quick faults in a row that make a page hot
#define HOT_REPORTS 256   // reports after which a hot page is protected again, to see whether it still is
static mach_timebase_info_data_t timebase;
static uint32_t now_ms(void) { return (uint32_t)(mach_absolute_time() * timebase.numer / timebase.denom / 1000000u); }
static Range *ranges;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Write-watch counters (plat_mem_watch_stats): faults, mach_absolute_time ticks in them, pages reported, protected
// again, pages made hot, mach_vm_protect calls that protected pages again, quick faults.
static uint64_t ww_faults, ww_ticks, ww_reported, ww_armed, ww_hot, ww_arm_calls, ww_quick;
static int ww_on;
static struct sigaction prior_bus, prior_segv;

// No recoverable fault context on Darwin; the common diagnostics may link these even with watches off.
_Thread_local int plat_fault_write = -1;
_Thread_local uint64_t plat_fault_pc, plat_fault_ret;

static uint64_t rounded(uint64_t n) {
    return n > UINT64_MAX - (PAGE_BYTES - 1) ? 0 : (n + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
}

static int map_anon(mach_vm_address_t *base, uint64_t n, int flags, vm_prot_t prot) {
    return mach_vm_map(mach_task_self(), base, n, 0, flags, MACH_PORT_NULL, 0, FALSE, prot, RW,
                       VM_INHERIT_COPY) == KERN_SUCCESS;
}

// The watched range holding addr, without the lock (ww_fault).
static Range *watched_at(uint64_t addr) {
    for (Range *r = __atomic_load_n(&ranges, __ATOMIC_ACQUIRE); r; r = __atomic_load_n(&r->next, __ATOMIC_ACQUIRE))
        if (addr < r->end) return addr >= r->base && r->watch ? r : NULL;
    return NULL;
}

// A write to a read-only page of a watched reservation that is read-write to the caller: writable again, marked
// written, and the write runs again. Async-signal-safe: no lock, no allocation, no thread-local storage. Anything else
// goes on to the crash report or the handler that was there before.
static void ww_fault(int sig, siginfo_t *si, void *context) {
    ucontext_t *uc = context;
    uint64_t addr = (uintptr_t)si->si_addr;
    int synchronous = si->si_code > 0 && si->si_code < SI_USER;
    Range *r = synchronous && (uc->uc_mcontext->__es.__err & 2) ? watched_at(addr) : NULL;   // bit 1: a write
    if (r) {
        uint64_t i = (addr - r->base) / PAGE_BYTES, t0 = mach_absolute_time();
        if (__atomic_load_n(&r->state[i], __ATOMIC_ACQUIRE) == PLAT_PROT_RW + 1 &&
            mach_vm_protect(mach_task_self(), addr & ~(PAGE_BYTES - 1), PAGE_BYTES, FALSE, RW) == KERN_SUCCESS) {
            // Writable first, marked after: a report that runs between the two sees the page unwritten and leaves it
            // writable and marked, so the next report has it.
            __atomic_store_n(&r->written[i], WRITTEN, __ATOMIC_RELEASE);
            __atomic_add_fetch(&ww_faults, 1, __ATOMIC_RELAXED);
            uint32_t at = __atomic_load_n(&r->armed_at[i], __ATOMIC_RELAXED);
            int quick = at && now_ms() - at < QUICK_MS;
            unsigned char q = __atomic_load_n(&r->quick[i], __ATOMIC_RELAXED);
            __atomic_store_n(&r->quick[i], quick ? (unsigned char)(q < 255 ? q + 1 : 255) : 0, __ATOMIC_RELAXED);
            if (quick) __atomic_add_fetch(&ww_quick, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&ww_ticks, mach_absolute_time() - t0, __ATOMIC_RELAXED);
            return;
        }
    }
    darwin_crash_forward(sig, si, context, sig == SIGBUS ? &prior_bus : &prior_segv);
}

// Once, at the first watched reservation. A protection fault on macOS x86_64 is a SIGBUS; SIGSEGV too, to be safe.
static void ww_install(void) {
    struct sigaction action = {.sa_sigaction = ww_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER};
    sigemptyset(&action.sa_mask);
    mach_timebase_info(&timebase);
    sigaction(SIGBUS, &action, &prior_bus);
    sigaction(SIGSEGV, &action, &prior_segv);
    __atomic_store_n(&ww_on, 1, __ATOMIC_RELEASE);
}

static uint64_t new_range(mach_vm_address_t base, uint64_t n, int guest, int watch) {
    if (!n || vm_page_size != PAGE_BYTES) return 0;
    Range *r = calloc(1, sizeof *r + n / PAGE_BYTES);
    if (!r) return 0;
    // Zero-filled lazily by the kernel: only the pages of the maps that are touched cost memory.
    if (watch && (!(r->written = calloc(n / PAGE_BYTES, 1)) || !(r->busy = calloc(n / PAGE_BYTES, sizeof *r->busy)) ||
                  !(r->quick = calloc(n / PAGE_BYTES, 1)) ||
                  !(r->armed_at = calloc(n / PAGE_BYTES, sizeof *r->armed_at)))) {
        free(r->written);
        free(r->busy);
        free(r->quick);
        free(r);
        return 0;
    }
    pthread_mutex_lock(&lock);
    if (!map_anon(&base, n, guest ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE, guest ? VM_PROT_NONE : RW)) {
        pthread_mutex_unlock(&lock);
        free(r->written);
        free(r->busy);
        free(r->quick);
        free(r->armed_at);
        free(r);
        return 0;
    }
    r->base = base;
    r->end = base + n;
    r->guest = guest;
    r->watch = watch;
    if (!guest) memset(r->state, PLAT_PROT_RW + 1, n / PAGE_BYTES);
    if (watch && !ww_on) ww_install();
    Range **slot = &ranges;
    while (*slot && (*slot)->base < base) slot = &(*slot)->next;
    r->next = *slot;
    __atomic_store_n(slot, r, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
    return base;
}

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    if (!addr || ((addr | n) & (PAGE_BYTES - 1)) || addr >= GUEST_END || n > GUEST_END - addr) return 0;
    return new_range(addr, n, 1, watch != 0) != 0;
}

void *plat_mem_alloc(size_t n) {
    return (void *)(uintptr_t)new_range(0, rounded(n), 0, 0);
}

// The first containing or following range. Called with lock held.
static Range *at_or_after(uint64_t addr) {
    Range *r = ranges;
    while (r && r->end <= addr) r = r->next;
    return r;
}

typedef enum { COMMIT, DECOMMIT, PROTECT } Operation;
static int change(uint64_t addr, uint64_t n, Operation op, int prot) {
    n = rounded(n);
    if (!n || (addr & (PAGE_BYTES - 1))) return 0;
    pthread_mutex_lock(&lock);
    Range *r = at_or_after(addr);
    int ok = 0;
    if (!r || addr < r->base || n > r->end - addr || (!r->guest && op != PROTECT)) goto done;
    uint64_t first = (addr - r->base) / PAGE_BYTES, count = n / PAGE_BYTES;
    unsigned char *pages = r->state + first;
    if (op == PROTECT && memchr(pages, 0, count)) goto done;
    // Watched: the state changes before the protection, so a write that faults meanwhile finds the new one and
    // ww_fault never makes writable a page being decommitted or made read-only.
    if (r->watch) memset(pages, op == DECOMMIT ? 0 : prot + 1, count);
    if (op == DECOMMIT) {
        // One kernel replacement, only inside our reservation. There is no unmap/reacquire window.
        mach_vm_address_t base = addr;
        ok = map_anon(&base, n, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, VM_PROT_NONE);
        if (ok && r->watch) {   // the pages are gone: nothing written there, nothing hot
            memset(r->written + first, CLEAN, count);
            memset(r->quick + first, 0, count);
        }
    } else {
        vm_prot_t p = prot == PLAT_PROT_RW ? RW : prot == PLAT_PROT_READ ? VM_PROT_READ : VM_PROT_NONE;
        // Watched and read-write to the caller: read-only in fact until the first write, writable where already
        // written.
        int watch_rw = r->watch && prot == PLAT_PROT_RW;
        ok = mach_vm_protect(mach_task_self(), addr, n, FALSE, watch_rw ? VM_PROT_READ : p) == KERN_SUCCESS;
        for (uint64_t i = 0, j; ok && watch_rw && i < count; i = j) {
            for (j = i; j < count && __atomic_load_n(&r->written[first + j], __ATOMIC_ACQUIRE); j++) {}
            if (j > i)
                ok = mach_vm_protect(mach_task_self(), addr + i * PAGE_BYTES, (j - i) * PAGE_BYTES, FALSE, RW)
                     == KERN_SUCCESS;
            else j++;
        }
    }
    if (ok) memset(pages, op == DECOMMIT ? 0 : prot + 1, count);
done:
    pthread_mutex_unlock(&lock);
    return ok;
}

int plat_mem_commit(uint64_t addr, size_t n) { return change(addr, n, COMMIT, PLAT_PROT_RW); }
int plat_mem_decommit(uint64_t addr, uint64_t n) { return change(addr, n, DECOMMIT, PLAT_PROT_NONE); }
int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    if (prot < PLAT_PROT_NONE || prot > PLAT_PROT_RW) return 0;
    return change(addr, n, PROTECT, prot);
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    pthread_mutex_lock(&lock);
    Range *r = at_or_after(addr);
    int state = PLAT_MEM_FREE;
    uint64_t stop = limit;
    if (r && addr < r->base) {
        if (stop > r->base) stop = r->base;
    } else if (r) {
        uint64_t index = (addr - r->base) / PAGE_BYTES;
        unsigned char value = r->state[index];
        state = (int)value - 1;
        for (;;) {
            stop = r->base + index * PAGE_BYTES;
            if (stop >= limit || r->state[index] != value) break;
            if ((++index) * PAGE_BYTES == r->end - r->base) {
                stop = r->end;
                r = r->next;
                if (!r || r->base != stop) break;
                index = 0;
            }
        }
        if (stop > limit) stop = limit;
    }
    pthread_mutex_unlock(&lock);
    *end = stop;
    return state;
}

int plat_mem_poke(uint64_t addr, const void *src, size_t n) {
    (void)addr; (void)src; (void)n;
    return 0;   // No through-protection write, even on the caller's writable probe byte.
}

int plat_mem_peek(void *dst, uint64_t addr, size_t n) {
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), addr, n, (uintptr_t)dst, &copied) == KERN_SUCCESS && copied == n;
}

int plat_mem_readable(uint64_t addr, uint64_t n) {
    if (n > UINT64_MAX - addr) return 0;
    uint64_t end = addr + n;
    while (addr < end) {
        mach_vm_address_t base = addr;
        mach_vm_size_t size = 0;
        natural_t depth = UINT32_MAX;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &base, &size, &depth, (vm_region_recurse_info_t)&info, &count)
            != KERN_SUCCESS || base > addr || size <= addr - base || !(info.protection & VM_PROT_READ)) return 0;
        if (size >= end - base) break;
        addr = base + size;
    }
    return 1;
}

// Lock held: [addr, addr + n) inside one watched range, else NULL; *first and *count are its pages.
static Range *watched_locked(uint64_t addr, uint64_t n, uint64_t *first, uint64_t *count) {
    Range *r = at_or_after(addr);
    if (!r || !r->watch || addr < r->base || n > r->end - addr) return NULL;
    *first = (addr - r->base) / PAGE_BYTES;
    *count = (addr + n - r->base + PAGE_BYTES - 1) / PAGE_BYTES - *first;
    return r;
}

// Lock held: protects pages [i, j) of r again (read-only, the write watch armed).
static void arm_locked(Range *r, uint64_t i, uint64_t j) {
    if (j <= i) return;
    if (mach_vm_protect(mach_task_self(), r->base + i * PAGE_BYTES, (j - i) * PAGE_BYTES, FALSE, VM_PROT_READ)
        == KERN_SUCCESS)
        __atomic_add_fetch(&ww_armed, j - i, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ww_arm_calls, 1, __ATOMIC_RELAXED);
    // The exact ms (0 means never): `| 1` stored the next ms half the time, and ww_fault's now - at then wrapped.
    uint32_t t = now_ms();
    for (t = t ? t : 1; i < j; i++) __atomic_store_n(&r->armed_at[i], t, __ATOMIC_RELAXED);
}

uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !n) return 0;
    pthread_mutex_lock(&lock);
    uint64_t first, count, got = 0, run = UINT64_MAX, i;
    Range *r = watched_locked(addr, n, &first, &count);
    if (!r) {
        pthread_mutex_unlock(&lock);
        return UINT64_MAX;
    }
    for (i = first; i < first + count && got < max; i++) {
        // ww_fault only marks CLEAN pages, and the others are writable, so they cannot fault: what is read here stays
        // until the store below.
        int again = 0;
        unsigned char w = __atomic_load_n(&r->written[i], __ATOMIC_ACQUIRE);
        int rw = r->state[i] == PLAT_PROT_RW + 1;   // read-only pages stay as the caller set them
        if (w == HOT) {
            pages[got++] = r->base + i * PAGE_BYTES;
            if (reset && (++r->armed_at[i] >= HOT_REPORTS || !rw)) {   // cooled down? protected again to see
                __atomic_store_n(&r->written[i], CLEAN, __ATOMIC_RELEASE);
                r->quick[i] = 0;
                again = rw;
            }
        } else if (w == WRITTEN) {
            pages[got++] = r->base + i * PAGE_BYTES;
            if (reset && r->busy[i]) __atomic_store_n(&r->written[i], UNARMED, __ATOMIC_RELEASE);   // until plat_mem_wrote
            else if (reset && rw && r->quick[i] >= HOT_QUICK) {
                __atomic_store_n(&r->written[i], HOT, __ATOMIC_RELEASE);
                r->armed_at[i] = 0;
                __atomic_add_fetch(&ww_hot, 1, __ATOMIC_RELAXED);
            } else if (reset) {
                __atomic_store_n(&r->written[i], CLEAN, __ATOMIC_RELEASE);
                again = rw;
            }
        }
        if (again && run == UINT64_MAX) run = i;
        if (!again && run != UINT64_MAX) arm_locked(r, run, i), run = UINT64_MAX;
    }
    if (run != UINT64_MAX) arm_locked(r, run, i);
    __atomic_add_fetch(&ww_reported, got, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&lock);
    return got;
}

int plat_mem_will_write(uint64_t addr, uint64_t n) {
    if (!n || !__atomic_load_n(&ww_on, __ATOMIC_ACQUIRE)) return 1;
    uint64_t lo = addr & ~(PAGE_BYTES - 1), hi = rounded(addr + n);
    int ok = 1;
    pthread_mutex_lock(&lock);
    for (Range *r = at_or_after(lo); r && r->base < hi; r = r->next) {
        if (!r->watch) continue;
        uint64_t a = lo > r->base ? lo : r->base, b = hi < r->end ? hi : r->end;
        for (uint64_t i = (a - r->base) / PAGE_BYTES, end = (b - r->base) / PAGE_BYTES; i < end; i++)
            if (r->state[i] == PLAT_PROT_RW + 1 && r->busy[i] < UINT16_MAX) r->busy[i]++;
        for (uint64_t i = (a - r->base) / PAGE_BYTES, end = (b - r->base) / PAGE_BYTES, j; i < end; i = j) {
            // Runs of clean read-write pages: marked written, then writable (the kernel's write is a write).
            for (j = i; j < end && r->state[j] == PLAT_PROT_RW + 1 && r->written[j] == CLEAN; j++)
                __atomic_store_n(&r->written[j], WRITTEN, __ATOMIC_RELEASE);
            if (j > i)
                ok &= mach_vm_protect(mach_task_self(), r->base + i * PAGE_BYTES, (j - i) * PAGE_BYTES, FALSE, RW)
                      == KERN_SUCCESS;
            else j++;
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

void plat_mem_wrote(uint64_t addr, uint64_t n) {
    if (!n || !__atomic_load_n(&ww_on, __ATOMIC_ACQUIRE)) return;
    uint64_t lo = addr & ~(PAGE_BYTES - 1), hi = rounded(addr + n);
    pthread_mutex_lock(&lock);
    for (Range *r = at_or_after(lo); r && r->base < hi; r = r->next) {
        if (!r->watch) continue;
        uint64_t a = lo > r->base ? lo : r->base, b = hi < r->end ? hi : r->end;
        for (uint64_t i = (a - r->base) / PAGE_BYTES, end = (b - r->base) / PAGE_BYTES; i < end; i++)
            // The last write in flight on a page reported meanwhile: written again, so the next report has the
            // kernel's bytes (and any write since) and protects it.
            if (r->busy[i] && !--r->busy[i] && r->written[i] == UNARMED)
                __atomic_store_n(&r->written[i], WRITTEN, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock);
}

int plat_mem_watch_stats(uint64_t out[7]) {
    if (!__atomic_load_n(&ww_on, __ATOMIC_ACQUIRE)) return 0;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    out[0] = __atomic_load_n(&ww_faults, __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&ww_ticks, __ATOMIC_RELAXED) * tb.numer / tb.denom;
    out[2] = __atomic_load_n(&ww_reported, __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&ww_armed, __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&ww_hot, __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&ww_arm_calls, __ATOMIC_RELAXED);
    out[6] = __atomic_load_n(&ww_quick, __ATOMIC_RELAXED);
    return 1;
}

uint64_t plat_mem_populated(uint64_t addr, uint64_t n, uint64_t (*runs)[2], uint64_t max) {
    (void)addr; (void)runs;
    return n && max ? UINT64_MAX : 0;   // Residency does not identify non-zero data.
}

int plat_mem_replace_anon(uint64_t addr, uint64_t n) {
    (void)addr; (void)n;
    return 0;   // External placed Vulkan mappings do not participate in Darwin reservation ownership.
}

void *plat_aligned_alloc(size_t size, size_t align) {
    void *p;
    return posix_memalign(&p, align, size) ? NULL : p;
}
void plat_aligned_free(void *p) { free(p); }
