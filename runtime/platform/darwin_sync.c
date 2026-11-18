// Zero-initialized storage, including locks embedded in short-lived runtime objects. Address waits avoid an
// allocated pthread lock/condition for each one (the platform API has no destroy operation).
#include "platform.h"
#include <errno.h>
#include <os/os_sync_wait_on_address.h>

// Readers occupy the low word, writers announce themselves before waiting to drain the readers.
#define WRITING (1ull << 31)
#define WAITING (1ull << 32)

void plat_mutex_init(PlatMutex *m) { m->state = 0; }
void plat_cond_init(PlatCond *c) { c->sequence = 0; }

int plat_mutex_trylock(PlatMutex *m) {
    uint64_t idle = 0;
    return __atomic_compare_exchange_n(&m->state, &idle, WRITING, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void plat_mutex_lock(PlatMutex *m) {
    if (plat_mutex_trylock(m)) return;
    __atomic_fetch_add(&m->state, WAITING, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t state = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
        if ((uint32_t)state) {
            os_sync_wait_on_address(&m->state, state, sizeof state, 0);
        } else if (__atomic_compare_exchange_n(&m->state, &state, state - WAITING + WRITING, 0,
                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    }
}
void plat_mutex_lock_from(PlatMutex *m, uintptr_t ra) { (void)ra; plat_mutex_lock(m); }

void plat_mutex_unlock(PlatMutex *m) {
    __atomic_fetch_sub(&m->state, WRITING, __ATOMIC_RELEASE);
    os_sync_wake_by_address_all(&m->state, sizeof m->state, 0);
}

int plat_mutex_trylock_shared(PlatMutex *m) {
    uint64_t state = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
    while (state < WRITING - 1)
        if (__atomic_compare_exchange_n(&m->state, &state, state + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock_shared(PlatMutex *m) {
    while (!plat_mutex_trylock_shared(m)) {
        uint64_t state = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
        if (state >= WRITING - 1) os_sync_wait_on_address(&m->state, state, sizeof state, 0);
    }
}

void plat_mutex_unlock_shared(PlatMutex *m) {
    uint64_t old = __atomic_fetch_sub(&m->state, 1, __ATOMIC_RELEASE);
    if ((uint32_t)old == 1 && old >= WAITING) os_sync_wake_by_address_all(&m->state, sizeof m->state, 0);
}

void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    uint64_t seq = __atomic_load_n(&c->sequence, __ATOMIC_RELAXED);
    plat_mutex_unlock(m);
    os_sync_wait_on_address(&c->sequence, seq, sizeof seq, 0);
    plat_mutex_lock(m);
}

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint64_t seq = __atomic_load_n(&c->sequence, __ATOMIC_RELAXED);
    plat_mutex_unlock(m);
    // Keep conversion to a Mach deadline representable even for an effectively infinite timeout.
    uint64_t slice = ns > INT64_MAX / 2 ? INT64_MAX / 2 : ns;
    int woke = 0;
    if (slice) {
        int result = os_sync_wait_on_address_with_timeout(&c->sequence, seq, sizeof seq, 0,
                                                          OS_CLOCK_MACH_ABSOLUTE_TIME, slice);
        woke = result >= 0 || errno != ETIMEDOUT || slice < ns;
    }
    plat_mutex_lock(m);
    return woke;
}

void plat_cond_signal(PlatCond *c) {
    __atomic_fetch_add(&c->sequence, 1, __ATOMIC_RELEASE);
    os_sync_wake_by_address_any(&c->sequence, sizeof c->sequence, 0);
}

void plat_cond_broadcast(PlatCond *c) {
    __atomic_fetch_add(&c->sequence, 1, __ATOMIC_RELEASE);
    os_sync_wake_by_address_all(&c->sequence, sizeof c->sequence, 0);
}
