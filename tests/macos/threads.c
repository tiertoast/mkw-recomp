// Check the Darwin primitives through platform.h, including ownership after timed join and detach.
#include "platform/platform.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed at line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static PlatEvent entered, proceed;
static PlatMutex rw = PLAT_MUTEX_INIT;
static PlatRMutex recursive = PLAT_RMUTEX_INIT;
static PlatOnce once = PLAT_ONCE_INIT;
static volatile int32_t initializations, detached;
static uint32_t worker_id;
static PlatEvent start_race;
static PlatOnce racing_once = PLAT_ONCE_INIT;
static int32_t init_race;
static unsigned protected_count;

static void race_init(void *unused) {
    (void)unused;
    plat_wait_ns(1000000);
    ++init_race;
}
static void contend(void *unused) {
    (void)unused;
    plat_event_wait(&start_race);
    plat_once(&racing_once, race_init, NULL);
    for (unsigned i = 0; i < 1000; ++i) {
        plat_mutex_lock_shared(&rw);
        unsigned before = protected_count;
        plat_mutex_unlock_shared(&rw);
        plat_mutex_lock(&rw);
        CHECK(before <= protected_count);
        ++protected_count;
        plat_mutex_unlock(&rw);
    }
}


static void init(void *unused) { (void)unused; plat_inc32(&initializations); }
static void worker(void *unused) {
    (void)unused;
    char name[64];
    CHECK(!pthread_getname_np(pthread_self(), name, sizeof name) && !strcmp(name, "foundation"));
    CHECK(pthread_get_stacksize_np(pthread_self()) >= PLAT_STACK_DEFAULT);
    worker_id = plat_thread_id();
    plat_once(&once, init, NULL);
    plat_mutex_lock_shared(&rw);
    plat_event_set(&entered);
    plat_event_wait(&proceed);
    plat_mutex_unlock_shared(&rw);
}
static void finish(void *unused) { (void)unused; plat_once(&once, init, NULL); plat_inc32(&detached); }

int main(void) {
    plat_event_init(&entered, 0, 0);
    plat_event_init(&proceed, 1, 0);
    PlatThread *t = plat_thread_new(worker, NULL, "foundation");
    CHECK(t && plat_event_wait_ns(&entered, 2000000000));
    CHECK(worker_id && worker_id != plat_thread_id());
    CHECK(plat_mutex_trylock_shared(&rw));
    plat_mutex_unlock_shared(&rw);
    CHECK(!plat_mutex_trylock(&rw));
    uint64_t start = plat_now_ns();
    CHECK(!plat_thread_join_ns(t, 20000000));
    CHECK(plat_now_ns() - start >= 20000000);
    CHECK(!plat_thread_join_ns(t, 0));
    plat_event_set(&proceed);
    CHECK(plat_thread_join_ns(t, UINT64_MAX));
    CHECK(plat_mutex_trylock(&rw) && !plat_mutex_trylock_shared(&rw));
    plat_mutex_unlock(&rw);
    plat_rmutex_lock(&recursive);
    CHECK(plat_rmutex_trylock(&recursive));
    plat_rmutex_unlock(&recursive);
    plat_rmutex_unlock(&recursive);
    CHECK(!plat_thread_new_ex(finish, NULL, NULL, 1));
    for (unsigned i = 0; i < 100; ++i) CHECK(plat_thread_spawn(finish, NULL, NULL, 1u << 20));
    t = plat_thread_new(finish, NULL, NULL);
    CHECK(t);
    plat_thread_join(t);
    while (__atomic_load_n(&detached, __ATOMIC_ACQUIRE) != 101) plat_yield();
    CHECK(initializations == 1);
    PlatThread *writers[8];
    plat_event_init(&start_race, 1, 0);
    for (unsigned i = 0; i < 8; ++i) {
        writers[i] = plat_thread_new(contend, NULL, "rw");
        CHECK(writers[i]);
    }
    plat_event_set(&start_race);
    for (unsigned i = 0; i < 8; ++i) plat_thread_join(writers[i]);
    CHECK(protected_count == 8000 && init_race == 1);
    puts("PASS thread lifetime, timeout then join, shared/exclusive/recursive locks, events and once");
    return 0;
}
