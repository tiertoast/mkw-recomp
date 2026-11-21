// Zero-initialized storage, including locks embedded in short-lived runtime objects. Address waits avoid an
// allocated pthread lock/condition for each one (the platform API has no destroy operation).
#include "platform.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <os/os_sync_wait_on_address.h>

// The lock word: its low 32 bits hold the readers, or WRITING while one thread holds it exclusively; above them, the
// threads asleep or about to sleep on it, writers in bits 32-47 (WAITING each) and readers in bits 48-63 (RWAITING
// each). A waiting writer keeps new readers out, so readers cannot starve it. An unlock wakes only when the word
// counts a sleeper, as the Linux futex lock does: os_sync_wake_by_address_all is a system call even with nobody to
// wake, and every unlock made one (G5-L run 34, a 10 s race sample: __ulock_wake in 411 of the replay thread's 1928
// busy samples, ~1.4 ms of each ~21 ms frame, and 480 of the shadow thread's, 20/11/2026). A sleeper counts itself
// before it reads the word it sleeps on, and the unlock's read-modify-write of that word sees the count or else the
// sleeper's read sees the unlock: no wake is lost.
#define WRITING (1ull << 31)
#define WAITING (1ull << 32)
#define RWAITING (1ull << 48)
#define WRITERS_WAITING (0xffffull << 32)

void plat_mutex_init(PlatMutex *m) { m->state = 0; }
void plat_cond_init(PlatCond *c) { c->sequence = c->waiters = 0; }

int plat_mutex_trylock(PlatMutex *m) {   // free and no writer waiting first (readers asleep do not count)
    uint64_t state = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
    while (!(uint32_t)state && !(state & WRITERS_WAITING))
        if (__atomic_compare_exchange_n(&m->state, &state, state | WRITING, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 1;
    return 0;
}

// Contention profiling is opt-in. Keep the uncontended path free of timers and profiling locks.
typedef struct { uintptr_t lock, caller; uint64_t ns, count; } LockWait;
static LockWait waits[256];
static unsigned wait_count;
static pthread_mutex_t samples_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t profile_once = PTHREAD_ONCE_INIT;
static int profile;

static void profile_init(void) {
    const char *value = getenv("MKW_LOCK_PROF");
    profile = value && *value && *value != '0';
}

static uint64_t start_wait(void) {
    pthread_once(&profile_once, profile_init);
    return profile ? plat_now_ns() : 0;
}

static void record_wait(PlatMutex *m, uintptr_t caller, uint64_t start) {
    uint64_t ns = plat_now_ns() - start;
    pthread_mutex_lock(&samples_lock);
    unsigned i = 0;
    while (i < wait_count && (waits[i].lock != (uintptr_t)m || waits[i].caller != caller)) i++;
    if (i < sizeof waits / sizeof *waits) {
        if (i == wait_count) {
            waits[i] = (LockWait){.lock = (uintptr_t)m, .caller = caller};
            wait_count++;
        }
        waits[i].ns += ns;
        waits[i].count++;
    }
    pthread_mutex_unlock(&samples_lock);
}

void plat_mutex_lock_from(PlatMutex *m, uintptr_t ra) {
    if (plat_mutex_trylock(m)) return;
    uint64_t began = start_wait();
    __atomic_fetch_add(&m->state, WAITING, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t state = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
        if ((uint32_t)state) {
            os_sync_wait_on_address(&m->state, state, sizeof state, 0);
        } else if (__atomic_compare_exchange_n(&m->state, &state, state - WAITING + WRITING, 0,
                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
    }
    if (began) record_wait(m, ra, began);
}
void plat_mutex_lock(PlatMutex *m) { plat_mutex_lock_from(m, (uintptr_t)__builtin_return_address(0)); }

void plat_mutex_unlock(PlatMutex *m) {
    uint64_t old = __atomic_fetch_sub(&m->state, WRITING, __ATOMIC_RELEASE);
    if (old >= WAITING) os_sync_wake_by_address_all(&m->state, sizeof m->state, 0);
}

static int shared_free(uint64_t state) { return (uint32_t)state < WRITING - 1 && !(state & WRITERS_WAITING); }

int plat_mutex_trylock_shared(PlatMutex *m) {
    uint64_t state = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
    while (shared_free(state))
        if (__atomic_compare_exchange_n(&m->state, &state, state + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock_shared(PlatMutex *m) {
    if (plat_mutex_trylock_shared(m)) return;
    uint64_t began = start_wait();
    __atomic_fetch_add(&m->state, RWAITING, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t state = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
        if (!shared_free(state)) {
            os_sync_wait_on_address(&m->state, state, sizeof state, 0);
        } else if (__atomic_compare_exchange_n(&m->state, &state, state - RWAITING + 1, 0, __ATOMIC_ACQUIRE,
                                               __ATOMIC_RELAXED)) break;
    }
    if (began) record_wait(m, (uintptr_t)__builtin_return_address(0), began);
}

void plat_mutex_unlock_shared(PlatMutex *m) {   // the last reader out wakes a waiting writer
    uint64_t old = __atomic_fetch_sub(&m->state, 1, __ATOMIC_RELEASE);
    if ((uint32_t)old == 1 && old >= WAITING) os_sync_wake_by_address_all(&m->state, sizeof m->state, 0);
}

// The sequence a waiter sleeps on, and beside it the waiters, counted before each reads the sequence: a signal skips
// its system call when none is counted (the same reasoning as the lock word's; both sides sequentially consistent).
static uint32_t cond_enter(PlatCond *c) {
    __atomic_fetch_add(&c->waiters, 1, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&c->sequence, __ATOMIC_SEQ_CST);
}

void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    uint32_t seq = cond_enter(c);
    plat_mutex_unlock(m);
    os_sync_wait_on_address(&c->sequence, seq, sizeof seq, 0);
    __atomic_fetch_sub(&c->waiters, 1, __ATOMIC_RELAXED);
    plat_mutex_lock(m);
}

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint32_t seq = cond_enter(c);
    plat_mutex_unlock(m);
    // Keep conversion to a Mach deadline representable even for an effectively infinite timeout.
    uint64_t slice = ns > INT64_MAX / 2 ? INT64_MAX / 2 : ns;
    int woke = 0;
    if (slice) {
        int result = os_sync_wait_on_address_with_timeout(&c->sequence, seq, sizeof seq, 0,
                                                          OS_CLOCK_MACH_ABSOLUTE_TIME, slice);
        woke = result >= 0 || errno != ETIMEDOUT || slice < ns;
    }
    __atomic_fetch_sub(&c->waiters, 1, __ATOMIC_RELAXED);
    plat_mutex_lock(m);
    return woke;
}

void plat_cond_signal(PlatCond *c) {
    __atomic_fetch_add(&c->sequence, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->waiters, __ATOMIC_SEQ_CST)) os_sync_wake_by_address_any(&c->sequence, sizeof c->sequence, 0);
}

void plat_cond_broadcast(PlatCond *c) {
    __atomic_fetch_add(&c->sequence, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->waiters, __ATOMIC_SEQ_CST)) os_sync_wake_by_address_all(&c->sequence, sizeof c->sequence, 0);
}

static int longest_first(const void *a, const void *b) {
    uint64_t x = ((const LockWait *)a)->ns, y = ((const LockWait *)b)->ns;
    return (x < y) - (x > y);
}

int plat_lock_report(int holds, char *buf, int size) {
    if (size <= 0) return 0;
    buf[0] = 0;
    if (holds) return 0; // Holder profiling is Linux-specific.
    LockWait snapshot[sizeof waits / sizeof *waits];
    pthread_mutex_lock(&samples_lock);
    unsigned n = wait_count;
    memcpy(snapshot, waits, n * sizeof *waits);
    wait_count = 0;
    pthread_mutex_unlock(&samples_lock);
    qsort(snapshot, n, sizeof *snapshot, longest_first);
    int len = 0;
    uintptr_t base = plat_exe_base();
    for (unsigned i = 0; i < n && i < 10 && len < size - 1; i++) {
        LockWait *w = &snapshot[i];
        int k = snprintf(buf + len, size - len, "%s exe+%#lx from exe+%#lx %.1f ms (%llu)", i ? ";" : "",
                         (unsigned long)(w->lock - base), (unsigned long)(w->caller - base), w->ns / 1e6,
                         (unsigned long long)w->count);
        if (k < 0) break;
        len += k < size - len ? k : size - len - 1;
    }
    return len;
}
