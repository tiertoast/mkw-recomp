// Native calling convention and pthread migration. The separate guard case succeeds only on the expected address.
#include "platform/platform.h"
#include <fenv.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xmmintrin.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "fiber:%d: %s\n", __LINE__, #e); exit(1); } } while (0)
extern int fiber_check_registers(PlatFiber *to, uintptr_t seed);
extern void fiber_test_entry(void *arg);
static PlatFiber *fiber, *base_a, *base_b;
static uint32_t tid_a, tid_b;
static int step;
static volatile unsigned char *guard;

static void protection_fault(int sig, siginfo_t *info, void *context) {
    (void)context;
    _Exit((sig == SIGBUS || sig == SIGSEGV) && info->si_code > 0 && info->si_addr == guard ? 0 : 1);
}

static void touch_guard(void) {
    char local;
    mach_vm_address_t addr = (uintptr_t)&local;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    CHECK(mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                         &count, &object) == KERN_SUCCESS);
    if (object) mach_port_deallocate(mach_task_self(), object);
    CHECK(addr <= (uintptr_t)&local && (uintptr_t)&local - addr < size && size == 65536);
    guard = (volatile unsigned char *)(uintptr_t)(addr - 1);
    struct sigaction sa = {.sa_flags = SA_SIGINFO, .sa_sigaction = protection_fault};
    sigemptyset(&sa.sa_mask);
    CHECK(!sigaction(SIGBUS, &sa, NULL) && !sigaction(SIGSEGV, &sa, NULL));
    *guard = 1;
    exit(1);
}

void fiber_test_body(void *arg) {
    if (!arg) touch_guard();
    CHECK(arg == &step && plat_thread_id() == tid_a);
    CHECK(fegetround() == FE_TONEAREST);
    CHECK(!fesetround(FE_DOWNWARD));
    _mm_setcsr(_mm_getcsr() | 0x21);   // MXCSR invalid + precision flags
    const unsigned saved = _mm_getcsr();
    step++;
    CHECK(fiber_check_registers(base_a, 0x1000));
    CHECK(plat_thread_id() == tid_b && fegetround() == FE_DOWNWARD && _mm_getcsr() == saved);
    step++;
    CHECK(fiber_check_registers(base_b, 0x2000));
    CHECK(plat_thread_id() == tid_a && fegetround() == FE_DOWNWARD && _mm_getcsr() == saved);
    step++;
    plat_fiber_switch(base_a);
    abort();
}

static void migrate(void *unused) {
    (void)unused;
    tid_b = plat_thread_id();
    CHECK(tid_b != tid_a && !fesetround(FE_TOWARDZERO));
    base_b = plat_fiber_self();
    CHECK(fiber_check_registers(fiber, 0x3000));
    CHECK(fegetround() == FE_TOWARDZERO && step == 2);
    plat_fiber_unself();
}

int main(int argc, char **argv) {
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    puts("SKIP: custom fiber stacks have no ASan/TSan switch annotations");
    return 77;
#endif
    int guard_case = argc == 2 && !strcmp(argv[1], "guard");
    tid_a = plat_thread_id();
    CHECK(!plat_fiber_new(SIZE_MAX, fiber_test_entry, NULL));
    fiber = plat_fiber_new(65536, fiber_test_entry, guard_case ? NULL : &step);
    CHECK(fiber && !fesetround(FE_UPWARD));
    base_a = plat_fiber_self();
    CHECK(fiber_check_registers(fiber, 0x4000));
    CHECK(step == 1 && fegetround() == FE_UPWARD);
    PlatThread *t = plat_thread_new(migrate, NULL, "fiber migration");
    CHECK(t);
    plat_thread_join(t);
    CHECK(fiber_check_registers(fiber, 0x5000));
    CHECK(step == 3 && fegetround() == FE_UPWARD);
    plat_fiber_free(fiber);
    plat_fiber_unself();
    puts("PASS native GPRs, FP, stack alignment and A-B-A migration");
    return 0;
}
