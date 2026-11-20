// --probe-locks: platform.h's locks and condition variables under contention, then what an uncontended one costs.
// Eight threads take one lock exclusively (no increment lost); two writers and six readers share another (no reader
// sees a writer inside, no write lost); two producers and four consumers pass numbers through a queue of four with
// condition variables, half the consumers with short timed waits (every number arrives once). A lost wakeup leaves a
// thread asleep: each part must end within 60 s or the probe fails and ends the process. Darwin's locks wake only a
// sleeper they counted (darwin_sync.c); with a wake that never comes, both kinds of lost wakeup were caught here by
// that deadline (G5-L, 20/11/2026). Needs no game files.
#include <stdint.h>
#include <stdio.h>

#include "host.h"
#include "platform/platform.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL locks: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define N_EXCL 500000
#define N_RW 200000
#define N_Q 100000
#define QCAP 4
#define DEADLINE_NS 60000000000ull

static PlatMutex m = PLAT_MUTEX_INIT, rw = PLAT_MUTEX_INIT, qm = PLAT_MUTEX_INIT;
static PlatCond q_full = PLAT_COND_INIT, q_empty = PLAT_COND_INIT;
static uint64_t counter, val[2], overlaps;
static int writing;
static int q[QCAP], qn, qh;
static uint64_t got_sum, got_n;

static void excl(void *a) {
    (void)a;
    for (int i = 0; i < N_EXCL; i++) plat_mutex_lock(&m), counter++, plat_mutex_unlock(&m);
}

static void rw_thread(void *a) {
    int writer = a != NULL;
    for (int i = 0; i < N_RW; i++) {
        if (writer && i % 4 == 0) {
            plat_mutex_lock(&rw);
            __atomic_store_n(&writing, 1, __ATOMIC_RELAXED);
            val[0]++, val[1]++;
            __atomic_store_n(&writing, 0, __ATOMIC_RELAXED);
            plat_mutex_unlock(&rw);
        } else {
            plat_mutex_lock_shared(&rw);
            if (__atomic_load_n(&writing, __ATOMIC_RELAXED) || val[0] != val[1])
                __atomic_fetch_add(&overlaps, 1, __ATOMIC_RELAXED);
            plat_mutex_unlock_shared(&rw);
        }
    }
}

static void producer(void *a) {
    (void)a;
    for (int i = 1; i <= N_Q; i++) {
        plat_mutex_lock(&qm);
        while (qn == QCAP) plat_cond_wait(&q_empty, &qm);
        q[(qh + qn++) % QCAP] = i;
        plat_cond_signal(&q_full);
        plat_mutex_unlock(&qm);
    }
}

static void consumer(void *a) {
    int timed = a != NULL;
    for (;;) {
        plat_mutex_lock(&qm);
        while (!qn && got_n < 2ull * N_Q) {
            if (timed) plat_cond_wait_ns(&q_full, &qm, 200000);   // 0.2 ms: timeouts mixed with wakeups
            else plat_cond_wait(&q_full, &qm);
        }
        if (!qn) {   // all arrived
            plat_mutex_unlock(&qm);
            return;
        }
        got_sum += (uint64_t)q[qh], got_n++;
        qh = (qh + 1) % QCAP, qn--;
        plat_cond_signal(&q_empty);
        if (got_n == 2ull * N_Q) plat_cond_broadcast(&q_full);   // the others stop waiting
        plat_mutex_unlock(&qm);
    }
}

// Starts n threads of fn (arg(i) each) and joins them by the deadline; a thread still asleep there is a lost wakeup.
static void run(const char *what, int n, PlatThreadFn fn, void *(*arg)(int)) {
    PlatThread *t[8];
    for (int i = 0; i < n; i++) t[i] = plat_thread_new(fn, arg(i), "lock probe");
    uint64_t end = plat_now_ns() + DEADLINE_NS;
    for (int i = 0; i < n; i++) {
        uint64_t now = plat_now_ns();
        if (!t[i] || !plat_thread_join_ns(t[i], now < end ? end - now : 1)) {
            fprintf(stderr, "FAIL locks: %s: a thread did not finish in 60 s (a lost wakeup)\n", what);
            fflush(stderr);
            plat_exit(1);
        }
    }
}
static void *none(int i) { (void)i; return NULL; }
static void *first_two(int i) { return i < 2 ? (void *)1 : NULL; }
static void *odd(int i) { return i & 1 ? (void *)1 : NULL; }

int probe_locks(void) {
    PlatMutex u = PLAT_MUTEX_INIT;
    PlatCond uc = PLAT_COND_INIT;
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < 1000000; i++) plat_mutex_lock(&u), plat_mutex_unlock(&u);
    uint64_t t1 = plat_now_ns();
    for (int i = 0; i < 1000000; i++) plat_cond_signal(&uc);
    uint64_t t2 = plat_now_ns();
    printf("locks: uncontended lock and unlock %.1f ns, a signal with no waiter %.1f ns\n", (t1 - t0) / 1e6,
           (t2 - t1) / 1e6);

    run("exclusive", 8, excl, none);
    CHECK(counter == 8ull * N_EXCL, "exclusive: %llu increments, want %llu", (unsigned long long)counter,
          8ull * N_EXCL);
    run("shared", 8, rw_thread, first_two);
    CHECK(!overlaps, "shared: %llu reads saw a writer inside", (unsigned long long)overlaps);
    CHECK(val[0] == 2ull * N_RW / 4, "shared: %llu writes, want %llu", (unsigned long long)val[0], 2ull * N_RW / 4);
    // producers and consumers at once: one run of six threads
    PlatThread *t[6];
    for (int i = 0; i < 2; i++) t[i] = plat_thread_new(producer, NULL, "lock probe");
    for (int i = 0; i < 4; i++) t[2 + i] = plat_thread_new(consumer, odd(i), "lock probe");
    uint64_t end = plat_now_ns() + DEADLINE_NS;
    for (int i = 0; i < 6; i++) {
        uint64_t now = plat_now_ns();
        if (!t[i] || !plat_thread_join_ns(t[i], now < end ? end - now : 1)) {
            fprintf(stderr, "FAIL locks: queue: a thread did not finish in 60 s (a lost wakeup)\n");
            fflush(stderr);
            plat_exit(1);
        }
    }
    uint64_t want = 2ull * ((uint64_t)N_Q * (N_Q + 1) / 2);
    CHECK(got_n == 2ull * N_Q && got_sum == want, "queue: %llu numbers summing %llu, want %llu summing %llu",
          (unsigned long long)got_n, (unsigned long long)got_sum, 2ull * N_Q, (unsigned long long)want);
    printf("locks: %s (%d failures)\n", fails ? "FAILED" : "passed", fails);
    return fails ? 1 : 0;
}
