// Darwin thread handles have two owners. Detaching releases the caller's reference even if the entry has not run yet.
#include "platform.h"
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

struct PlatThread {
    pthread_t thread;
    PlatThreadFn fn;
    void *arg;
    PlatEvent returned;
    int32_t refs;
    char name[64];
};

static void release(PlatThread *t) {
    if (!plat_dec32(&t->refs)) free(t);
}

static void completed(void *arg) {
    PlatThread *t = arg;
    plat_event_set(&t->returned);
    release(t);
}

static void *start(void *arg) {
    PlatThread *t = arg;
    if (*t->name) pthread_setname_np(t->name);
    pthread_cleanup_push(completed, t);
    t->fn(t->arg);
    pthread_cleanup_pop(1);
    return NULL;
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->refs = 2;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    plat_event_init(&t->returned, 1, 0);
    pthread_attr_t attr;
    int error = pthread_attr_init(&attr);
    if (!error) {
        error = pthread_attr_setstacksize(&attr, stack ? stack : PLAT_STACK_DEFAULT);
        if (!error) error = pthread_create(&t->thread, &attr, start, t);
        pthread_attr_destroy(&attr);
    }
    if (!error) return t;
    free(t);
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

void plat_thread_join(PlatThread *t) {
    if (pthread_join(t->thread, NULL)) abort();
    release(t);
}

void plat_thread_detach(PlatThread *t) {
    if (pthread_detach(t->thread)) abort();
    release(t);
}

int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    if (!plat_event_wait_ns(&t->returned, ns)) return 0;
    // The API waits for fn to return. A pthread_join here could block again in user TLS destructors.
    plat_thread_detach(t);
    return 1;
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (!t) return 0;
    plat_thread_detach(t);
    return 1;
}

// A borrowed thread port name is a nonzero, task-local 32-bit identity for the lifetime of the thread.
uint32_t plat_thread_id(void) { return pthread_mach_thread_np(pthread_self()); }
uint64_t plat_self_cpu_ns(void) { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); }
uint64_t plat_thread_cpu_ns(uint32_t tid) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(tid, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS) return 0;
    return ((uint64_t)info.user_time.seconds + info.system_time.seconds) * 1000000000u +
           ((uint64_t)info.user_time.microseconds + info.system_time.microseconds) * 1000u;
}
uint32_t plat_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (uint32_t)count : 1;
}
void plat_yield(void) { sched_yield(); }

uint64_t plat_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
uint64_t plat_counter(void) { return plat_now_ns(); }
uint64_t plat_counter_hz(void) { return 1000000000u; }

void plat_wait_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
void plat_sleep_ns(uint64_t ns) { plat_wait_ns(ns); }
void plat_sleep_ms(uint32_t ms) { plat_wait_ns((uint64_t)ms * 1000000u); }
