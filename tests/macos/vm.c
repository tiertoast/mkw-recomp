// Exercise the production memory SVCs with a synthetic layout. No module data or guest threads are needed.
#include "host.h"
#include "gamelib.h"
#include "platform/platform.h"
#include "memory_int.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE 4096ull
#define GIB (1ull << 30)
#define CHECK(e) do { if (!(e)) rt_fatal("line %d: %s", __LINE__, #e); } while (0)

static void state(uint64_t addr, uint64_t limit, int expected, uint64_t end) {
    uint64_t actual;
    CHECK(plat_mem_state(addr, limit, &actual) == expected && actual == end);
}

static void zero(uint64_t addr) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    for (size_t i = 0; i < PAGE; i++) CHECK(!p[i]);
}

static void collision(uint64_t addr) {
    mach_vm_address_t foreign = addr;
    CHECK(mach_vm_allocate(mach_task_self(), &foreign, PAGE, VM_FLAGS_FIXED) == KERN_SUCCESS);
    uint8_t *sentinel = (uint8_t *)(uintptr_t)addr;
    *sentinel = 0xa5;
    CHECK(!plat_mem_reserve(addr - PAGE, 3 * PAGE, 1));
    CHECK(!plat_mem_commit(addr, PAGE) && !plat_mem_protect(addr, PAGE, PLAT_PROT_NONE));
    CHECK(!plat_mem_decommit(addr, PAGE) && *sentinel == 0xa5);
    state(addr, addr + PAGE, PLAT_MEM_FREE, addr + PAGE);
}

enum { THREADS = 8, ROUNDS = 300 };
typedef struct { uint64_t addr; uint32_t *counter; } Worker;
static void *churn(void *arg) {
    Worker *w = arg;
    for (int i = 0; i < ROUNDS; i++) {
        CHECK(plat_mem_commit(w->addr, PAGE));
        zero(w->addr);
        memset((void *)(uintptr_t)w->addr, 0xb3, PAGE);
        CHECK(plat_mem_protect(w->addr, PAGE, PLAT_PROT_READ));
        state(w->addr, w->addr + PAGE, PLAT_PROT_READ, w->addr + PAGE);
        CHECK(plat_mem_decommit(w->addr, PAGE));
        __atomic_add_fetch(w->counter, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void stress(uint64_t base) {
    pthread_t ids[THREADS];
    Worker workers[THREADS];
    CHECK(plat_mem_commit(base, PAGE));
    uint32_t *counter = (uint32_t *)(uintptr_t)base;
    uint64_t start = plat_now_ns();
    for (unsigned i = 0; i < THREADS; i++) {
        workers[i] = (Worker){base + (1 + i) * PAGE, counter};
        CHECK(!pthread_create(&ids[i], NULL, churn, &workers[i]));
    }
    for (int i = 0; i < THREADS; i++) CHECK(!pthread_join(ids[i], NULL));
    CHECK(*counter == THREADS * ROUNDS);
    printf("VM stress: %u cycles, %d threads, %.3f ms\n", *counter, THREADS, (plat_now_ns() - start) / 1e6);
}

// Test-only observers: each thread leaves its faulting access by siglongjmp. Production installs no handler.
static _Thread_local sigjmp_buf escape;
static _Thread_local volatile uint8_t *fault_page;
static atomic_uint ready;
static void expected_fault(int sig, siginfo_t *info, void *context) {
    (void)context;
    if ((sig != SIGBUS && sig != SIGSEGV) || !fault_page || info->si_addr != fault_page || info->si_code <= 0) _Exit(2);
    siglongjmp(escape, 1);
}

static void *fault_writer(void *arg) {
    fault_page = arg;
    if (!sigsetjmp(escape, 1)) {
        atomic_fetch_add(&ready, 1);
        while (atomic_load(&ready) != THREADS) sched_yield();
        *fault_page = 1;
        return NULL;
    }
    return arg;
}

static void concurrent_faults(uint64_t addr) {
    CHECK(plat_mem_commit(addr, PAGE));
    CHECK(plat_mem_protect(addr, PAGE, PLAT_PROT_READ));
    struct sigaction observer = {.sa_sigaction = expected_fault, .sa_flags = SA_SIGINFO}, bus, segv;
    sigemptyset(&observer.sa_mask);
    CHECK(!sigaction(SIGBUS, &observer, &bus) && !sigaction(SIGSEGV, &observer, &segv));
    pthread_t threads[THREADS];
    for (unsigned i = 0; i < THREADS; i++)
        CHECK(!pthread_create(&threads[i], NULL, fault_writer, (void *)(uintptr_t)(addr + i)));
    for (unsigned i = 0; i < THREADS; i++) {
        void *result;
        CHECK(!pthread_join(threads[i], &result) && (uintptr_t)result == addr + i);
    }
    CHECK(!sigaction(SIGBUS, &bus, NULL) && !sigaction(SIGSEGV, &segv, NULL));
    zero(addr);
    state(addr, addr + PAGE, PLAT_PROT_READ, addr + PAGE);
}

static uint64_t resident(void) {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    CHECK(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS);
    return info.resident_size;
}

static void heap_test(uint64_t base) {
    const uint64_t size = 16ull << 20;
    uint64_t heap, before = resident();
    CHECK(!mem_set_heap_size(size, &heap) && heap == base);
    uint64_t committed = resident();
    for (uint64_t i = 0; i < size; i += PAGE) *(uint8_t *)(uintptr_t)(base + i) = 0x71;
    uint64_t touched = resident();
    CHECK(!mem_set_heap_size(0, &heap));
    uint64_t released = resident();
    printf("RSS MiB before/commit/touch/decommit of 16 MiB: %.2f %.2f %.2f %.2f\n",
           before / 1048576., committed / 1048576., touched / 1048576., released / 1048576.);
    CHECK(!mem_set_heap_size(size, &heap));
    for (uint64_t i = 0; i < size; i += PAGE) CHECK(!*(uint8_t *)(uintptr_t)(base + i));
    CHECK(!mem_set_heap_size(0, &heap));
}

static void alias_test(const RtGuestLayout *g) {
    uint64_t src = g->alias.base + PAGE, dst = g->stack.base;
    CHECK(!mem_map_physical(src, PAGE));
    uint8_t *p = (uint8_t *)(uintptr_t)src, *q = (uint8_t *)(uintptr_t)dst;
    p[10] = 0xcd;
    CHECK(!mem_map_physical(src - PAGE, 3 * PAGE) && p[10] == 0xcd);
    RtMemoryInfo info;
    CHECK(!mem_query(src, &info) && info.base == src - PAGE && info.size == 3 * PAGE && info.perm == 3);
    CHECK(!mem_map(dst, src, PAGE) && q[10] == 0xcd);
    CHECK(!plat_mem_readable(src, PAGE) && mem_unmap_physical(src, PAGE));
    q[11] = 0x73;
    CHECK(!mem_unmap(dst, src, PAGE) && p[11] == 0x73 && !plat_mem_readable(dst, PAGE));
    CHECK(!mem_unmap_physical(src, PAGE));
    state(src, src + 2 * PAGE, PLAT_MEM_RESERVED, src + PAGE);
    CHECK(!mem_map_physical(src, PAGE));
    zero(src);
    CHECK(!mem_unmap_physical(src - PAGE, 3 * PAGE));
}

static void page_test(uint64_t base) {
    CHECK(!plat_mem_commit(base + 1, PAGE));
    CHECK(!plat_mem_reserve(UINT64_MAX - PAGE + 1, 2 * PAGE, 0));
    CHECK(plat_mem_commit(base, 2 * PAGE));
    memset((void *)(uintptr_t)base, 0x17, 2 * PAGE);
    CHECK(plat_mem_commit(base, PAGE) && *(uint8_t *)(uintptr_t)base == 0x17);
    CHECK(!plat_mem_protect(base, 3 * PAGE, PLAT_PROT_NONE));
    state(base + 7, base + 3 * PAGE, PLAT_PROT_RW, base + 2 * PAGE);
    CHECK(plat_mem_protect(base, PAGE, PLAT_PROT_NONE));
    CHECK(!plat_mem_readable(base, 1) && !plat_mem_readable(base + PAGE - 1, 2));
    uint8_t byte = 0;
    CHECK(!plat_mem_poke(base, &byte, 1) && !plat_mem_peek(&byte, base, 1));
    CHECK(plat_mem_peek(&byte, base + PAGE, 1) && byte == 0x17);
    CHECK(plat_mem_decommit(base, PAGE) && plat_mem_commit(base, PAGE));
    zero(base);
    CHECK(*(uint8_t *)(uintptr_t)(base + PAGE) == 0x17);
    uint64_t page = 123, run[1][2] = {{88,99}};
    CHECK(mem_written_pages(base, 2 * PAGE, &page, 1) == UINT64_MAX);
    CHECK(mem_written_peek(base, 2 * PAGE, &page, 1) == UINT64_MAX && page == 123);
    CHECK(plat_mem_populated(base, PAGE, run, 1) == UINT64_MAX && run[0][0] == 88 && run[0][1] == 99);
    CHECK(!mem_freeze(base) && !mem_trap_init() && !mem_trap(base, 1, &byte));
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "retry")) {
        if (!getenv("MKW_REEXEC")) collision(rt_game()->modules[0].base);
        mem_init();
        CHECK(getenv("MKW_REEXEC") && !strcmp(getenv("MKW_REEXEC"), "1"));
        return 0;
    }
    // Leave a real foreign page inside the first 14 GiB candidate. Its neighbours remain unreserved.
    collision(6 * GIB);
    uint64_t start = plat_now_ns();
    mem_init();
    const RtGuestLayout *g = mem_layout();
    CHECK(g->aslr.base > 6 * GIB && g->alias.size == 64 * GIB);
    CHECK(*(uint8_t *)(uintptr_t)(6 * GIB) == 0xa5);
    state(g->aslr.base, g->aslr.base + 14 * GIB, PLAT_MEM_RESERVED, g->aslr.base + 14 * GIB);
    state(g->alias.base, g->alias.base + 64 * GIB, PLAT_MEM_RESERVED, g->alias.base + 64 * GIB);
    printf("VM layout: low=%#llx alias=%#llx init and scan=%.3f ms\n",
           (unsigned long long)g->aslr.base, (unsigned long long)g->alias.base, (plat_now_ns() - start) / 1e6);
    if (argc == 2) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)g->alias.base;
        if (!strcmp(argv[1], "write-ro")) {
            CHECK(!mem_map_physical((uintptr_t)p, PAGE));
            CHECK(plat_mem_protect((uintptr_t)p, PAGE, PLAT_PROT_READ));
        } else if (!strcmp(argv[1], "read-none")) {
            CHECK(!mem_map_physical((uintptr_t)p, PAGE));
            CHECK(plat_mem_protect((uintptr_t)p, PAGE, PLAT_PROT_NONE));
        } else CHECK(!strcmp(argv[1], "read-hole"));
        fprintf(stderr, "armed %s\n", argv[1]);
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        if (!strcmp(argv[1], "write-ro")) *p = 1;
        else (void)*p;
        return 1;
    }
    uint64_t edge = g->alias.base + g->alias.size - PAGE;
    CHECK(!plat_mem_commit(edge, 2 * PAGE) && !plat_mem_decommit(edge, 2 * PAGE));
    CHECK(!plat_mem_reserve(edge, 2 * PAGE, 0));
    state(edge, edge + 2 * PAGE, PLAT_MEM_RESERVED, edge + PAGE);
    heap_test(g->heap.base);
    page_test(g->heap.base + 32 * (1ull << 20));
    alias_test(g);
    concurrent_faults(g->alias.base + 8 * PAGE);
    stress(g->alias.base + 16 * PAGE);
    return 0;
}
