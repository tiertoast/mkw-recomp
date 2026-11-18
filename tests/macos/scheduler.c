// Drive the production guest kernel with C host functions and a single synthetic module descriptor.
#include "gamelib.h"
#include "kernel_int.h"
#include "cfg.h"

#define REQUIRE(x) do { if (!(x)) rt_fatal("scheduler:%d: %s", __LINE__, #x); } while (0)
#define MODULE (440ull << 30)
static uint64_t worker_addr, fiber_fn, init_addr, switch_addr, fini_addr, fiber, fiber_stack;
static uint32_t owner, *word;
static int high_done, other_done, migrated;
static uint64_t first_tlr;
static int with_fiber;

// Only a module descriptor and test entries are supplied. Arbitrary guest services are outside this fixture.
static void unused_entry(Cpu *c) { (void)c; abort(); }
const RtGame *rt_game_builtin(void) {
    static const uint64_t addr = MODULE, count = 1;
    static const GuestFn fn = unused_entry;
    static const RtGameModule module = {"fixture", MODULE, 4096, &addr, &fn, &count};
    static const RtGame game = {.abi = RT_GAME_ABI, .id = RT_GAME_ID, .modules = &module, .nmodules = 1};
    return &game;
}
int ldr_count(void) { return 1; }
const RtLoaded *ldr_module(int i) {
    static const RtLoaded mod = {.name = "fixture", .base = MODULE, .size = 4096};
    REQUIRE(!i);
    return &mod;
}
int64_t frame_step_world_us(void) { return -1; }   // No frames in this fixture.

void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)pc;
    kern_checkpoint();
    REQUIRE(kern_svc(c, imm));
}

static unsigned svc(Cpu *c, unsigned imm, uint64_t a, uint64_t b, uint64_t d, uint64_t e) {
    c->x[0] = a; c->x[1] = b; c->x[2] = d; c->x[3] = e;
    REQUIRE(kern_svc(c, imm));
    return (unsigned)c->x[0];
}

static uint32_t spawn(Cpu *c, int prio) {
    uint8_t *stack = mem_kernel_alloc(65536, 16);
    c->x[4] = prio;
    c->x[5] = 0;   // contend only for core zero
    REQUIRE(!svc(c, 8, 0, worker_addr, prio, (uintptr_t)(stack + 65536)));
    return c->x[1];
}

static void await_core_waiter(uint32_t handle) {
    for (;;) {
        lock();
        RtThread *w = g_core_wait;
        while (w && w->handle != handle) w = w->cnext;
        unlock();
        if (w) return;
        plat_yield();
    }
}

static void join_guest(Cpu *c, uint32_t h) {
    REQUIRE(!svc(c, 0x18, 0, (uintptr_t)&h, 1, 1000000000));
    REQUIRE(!svc(c, 0x16, h, 0, 0, 0));
}

static void moving_body(Cpu *c) {
    if (c->pc == MODULE + 8) {
        REQUIRE(migrated == 1 && c->thread == tcur && c->tpidrro != first_tlr);
        migrated++;
        c->x[0] = 0;
        return;
    }
    REQUIRE(c->tpidrro == first_tlr && kern_cpu() == c);
    c->x[23] = 0x998877;
    rt_call(c, switch_addr, 1, (uint64_t)0);
    REQUIRE(c->x[23] == 0x998877 && c->tpidrro != first_tlr && kern_cpu() == c && c->thread == tcur);
    migrated++;
    c->pc = MODULE + 8;   // Middle of an active owner, through the dispatcher's nonlocal-return path.
    rt_dispatch(c);
    abort();
}

static void moving(Cpu *c) {
    rt_sj_owner(c, moving_body, MODULE + 4, MODULE + 12);
}

static void worker(Cpu *c) {
    int priority = c->x[0];
    REQUIRE(tcur->cur_core == 0);
    if (priority == 30) {
        REQUIRE(!svc(c, 0x1a, owner, (uintptr_t)word, tcur->handle, 0));
        REQUIRE((*word & ~WAITERS_BIT) == tcur->handle && !other_done);
        if (with_fiber) rt_call(c, switch_addr, 1, fiber);
        REQUIRE(!svc(c, 0x1b, (uintptr_t)word, 0, 0, 0));
        high_done++;
    } else {
        REQUIRE(high_done == 1);
        other_done++;
    }
    svc(c, 0x0a, 0, 0, 0, 0);
    abort();
}

static void scenario(Cpu *c) {
    owner = tcur->handle;
    REQUIRE(g_ap == 1 && !g_ap_int && cores_enabled());
    REQUIRE(!svc(c, 0x0f, owner, 0, 1, 0));
    word = mem_kernel_alloc(sizeof *word, 4);
    *word = owner | WAITERS_BIT;
    if (with_fiber) {
        fiber = (uintptr_t)mem_kernel_alloc(256, 16);
        fiber_stack = (uintptr_t)mem_kernel_alloc(65536, 16);
        first_tlr = c->tpidrro;
        rt_call(c, init_addr, 6, fiber, fiber_fn, (uint64_t)0, fiber_stack, (uint64_t)65536, (uint64_t)0);
        rt_call(c, switch_addr, 1, fiber);
    }

    uint32_t high = spawn(c, 30);
    uint64_t start = plat_now_ns();
    REQUIRE(svc(c, 0x18, 0, (uintptr_t)&high, 1, 1000000) == KERN_R_TIMEOUT);
    REQUIRE(plat_now_ns() - start >= 1000000);
    volatile uint16_t *tlr = ap_tlr(tcur);
    tlr[0] = 1;
    REQUIRE(!svc(c, 9, high, 0, 0, 0));
    await_core_waiter(high);
    rt_spin_gate(c, MODULE);   // Even a zero-delay polling handoff must respect TLR disable_count.
    kern_checkpoint();
    REQUIRE(tlr[1] && tcur->cur_core == 0 && !high_done);
    REQUIRE(!svc(c, 0x36, 0, 0, 0, 0) && tlr[1] && tcur->cur_core == 0);
    tlr[0] = 0;
    REQUIRE(!svc(c, 0x36, 0, 0, 0, 0) && !tlr[1]);
    lock();
    REQUIRE(g_wait && g_wait->handle == high && g_wait->wkind == W_LOCK && prio_of(tcur) == 30);
    unlock();

    uint32_t medium = spawn(c, 36);
    REQUIRE(!svc(c, 9, medium, 0, 0, 0));
    await_core_waiter(medium);
    kern_checkpoint();
    REQUIRE(!other_done && tcur->cur_core == 0);
    REQUIRE(!svc(c, 0x1b, (uintptr_t)word, 0, 0, 0));
    await_core_waiter(high);   // Woken from the mutex and queued for this core.
    join_guest(c, high);
    join_guest(c, medium);
    REQUIRE(high_done == 1 && other_done == 1);
    if (with_fiber) {
        REQUIRE(migrated == 2);
        rt_call(c, fini_addr, 1, fiber);
    }

    uint32_t peer = spawn(c, tcur->prio);
    REQUIRE(!svc(c, 9, peer, 0, 0, 0));
    await_core_waiter(peer);
    svc(c, 0x0b, 0, 0, 0, 0);
    REQUIRE(other_done == 2);
    join_guest(c, peer);
    REQUIRE(!g_ap_sent && !g_ap_taken && g_ap_deferred && g_sync36_flag);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "scheduler") || !strcmp(argv[1], "fiber")));
    REQUIRE(!strcmp(cfg_meta("ASYNC_PREEMPT")->def, "2"));
    with_fiber = !strcmp(argv[1], "fiber");
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    if (with_fiber) {
        puts("SKIP: run the pthread scheduler case under ASan/TSan");
        return 77;
    }
#endif
    mem_init();
    worker_addr = hostfn_register("worker", worker);
    fiber_fn = hostfn_register("moving", moving);
    init_addr = hostfn_register("init", hostfn_override(KERN_SYM_INITIALIZE_FIBER));
    switch_addr = hostfn_register("switch", hostfn_override(KERN_SYM_SWITCH_TO_FIBER));
    fini_addr = hostfn_register("finish", hostfn_override(KERN_SYM_FINALIZE_FIBER));
    hostfn_seal();
    kern_run_on_guest_main(scenario);
    int held, peak;
    kern_core_stats(&held, &peak);
    REQUIRE(!held && peak == 1);
    puts("PASS cooperative core, deferred checkpoint, inheritance, yield, wait, wake, exit");
    return 0;
}
