// The module's two-level imports bind here. Tail forwarding preserves the dispatcher's constant stack usage.
#include "darwin_abi.h"
#include <stdatomic.h>
#include <stdlib.h>

static _Atomic(const RtDarwinHostV1 *) bound;

int mkw_bind_host_v1(const RtDarwinHostV1 *host) {
    if (!host || !host->dispatch || !host->sj_owner || !host->svc || !host->udf || !host->ticks ||
        !host->spin_gate || !host->prof_hit) return 0;
    const RtDarwinHostV1 *old = NULL;
    return atomic_compare_exchange_strong(&bound, &old, host) || old == host;
}

static const RtDarwinHostV1 *host(void) {
    const RtDarwinHostV1 *h = atomic_load_explicit(&bound, memory_order_acquire);
    if (!h) abort();
    return h;
}

void rt_dispatch(Cpu *c) { __attribute__((musttail)) return host()->dispatch(c); }
void rt_sj_owner(Cpu *c, GuestFn body, uint64_t lo, uint64_t hi) {
    __attribute__((musttail)) return host()->sj_owner(c, body, lo, hi);
}
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { __attribute__((musttail)) return host()->svc(c, imm, pc); }
void rt_udf(Cpu *c, uint64_t pc) { host()->udf(c, pc); __builtin_unreachable(); }
uint64_t rt_ticks(void) { __attribute__((musttail)) return host()->ticks(); }
void rt_spin_gate(Cpu *c, uint64_t head) { __attribute__((musttail)) return host()->spin_gate(c, head); }
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { __attribute__((musttail)) return host()->prof_hit(addr, lr, x0); }
