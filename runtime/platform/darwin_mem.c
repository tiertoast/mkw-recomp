// Native x86_64 guest mappings. Reservations are never released: ownership stays valid across decommit.
#include "platform.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_BYTES 4096ull
#define GUEST_END (1ull << 39)
#define RW (VM_PROT_READ | VM_PROT_WRITE)

// Sorted by base. Zero in the per-page array means reserved, including untouched pages of large windows.
typedef struct Range {
    struct Range *next;
    uint64_t base, end;
    int guest;
    unsigned char state[];
} Range;
static Range *ranges;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

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

static uint64_t new_range(mach_vm_address_t base, uint64_t n, int guest) {
    if (!n || vm_page_size != PAGE_BYTES) return 0;
    Range *r = calloc(1, sizeof *r + n / PAGE_BYTES);
    if (!r) return 0;
    pthread_mutex_lock(&lock);
    if (!map_anon(&base, n, guest ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE, guest ? VM_PROT_NONE : RW)) {
        pthread_mutex_unlock(&lock);
        free(r);
        return 0;
    }
    r->base = base;
    r->end = base + n;
    r->guest = guest;
    if (!guest) memset(r->state, PLAT_PROT_RW + 1, n / PAGE_BYTES);
    Range **slot = &ranges;
    while (*slot && (*slot)->base < base) slot = &(*slot)->next;
    r->next = *slot;
    *slot = r;
    pthread_mutex_unlock(&lock);
    return base;
}

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    (void)watch;   // No dirty epochs. plat_mem_written reports this explicitly.
    if (!addr || ((addr | n) & (PAGE_BYTES - 1)) || addr >= GUEST_END || n > GUEST_END - addr) return 0;
    return new_range(addr, n, 1) != 0;
}

void *plat_mem_alloc(size_t n) {
    return (void *)(uintptr_t)new_range(0, rounded(n), 0);
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
    unsigned char *pages = r->state + (addr - r->base) / PAGE_BYTES;
    if (op == DECOMMIT) {
        // One kernel replacement, only inside our reservation. There is no unmap/reacquire window.
        mach_vm_address_t base = addr;
        ok = map_anon(&base, n, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, VM_PROT_NONE);
    } else {
        if (op == PROTECT && memchr(pages, 0, n / PAGE_BYTES)) goto done;
        vm_prot_t p = prot == PLAT_PROT_RW ? RW : prot == PLAT_PROT_READ ? VM_PROT_READ : VM_PROT_NONE;
        ok = mach_vm_protect(mach_task_self(), addr, n, FALSE, p) == KERN_SUCCESS;
    }
    if (ok) memset(pages, op == DECOMMIT ? 0 : prot + 1, n / PAGE_BYTES);
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

uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    (void)addr; (void)pages; (void)reset;
    return n && max ? UINT64_MAX : 0;
}

uint64_t plat_mem_populated(uint64_t addr, uint64_t n, uint64_t (*runs)[2], uint64_t max) {
    (void)addr; (void)runs;
    return n && max ? UINT64_MAX : 0;   // Residency does not identify non-zero data.
}

int plat_mem_replace_anon(uint64_t addr, uint64_t n) {
    (void)addr; (void)n;
    return 0;   // External placed Vulkan mappings do not participate in Darwin reservation ownership.
}
