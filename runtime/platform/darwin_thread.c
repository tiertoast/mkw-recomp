// Darwin thread handles have two owners. Detaching releases the caller's reference even if the entry has not run yet.
#include "platform.h"
#include "darwin_context.h"
#include <errno.h>
#include <CoreFoundation/CFNumber.h>
#include <IOKit/IOKitLib.h>
#include <cpuid.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
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
    darwin_crash_stack();
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

// Darwin's affinity tags express cache-sharing preferences. They cannot enforce a processor mask.
uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) { (void)t; (void)mask; return 0; }
int plat_process_affinity(uint64_t mask) { (void)mask; return 0; }
void plat_thread_background(void) {} // There is no restricted foreground mask to escape.
void plat_timer_1ms(void) {} // Mach timed waits require no process-wide resolution request.

uint64_t plat_cpu_fast(void) {
    unsigned count = plat_cpu_count();
    return count >= 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
}

static pthread_once_t topology_once = PTHREAD_ONCE_INIT;
static uint64_t siblings[64], representatives;

static int registry_id(io_registry_entry_t entry, CFStringRef key, int *id) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, NULL, 0);
    int ok = value && CFGetTypeID(value) == CFNumberGetTypeID() &&
             CFNumberGetValue(value, kCFNumberIntType, id) && *id >= 0;
    if (value) CFRelease(value);
    return ok;
}

static void read_topology(void) {
    unsigned physical, logical = plat_cpu_count();
    size_t size = sizeof physical;
    if (logical > 64 || sysctlbyname("hw.physicalcpu", &physical, &size, NULL, 0)) return;
    if (physical == logical) {
        representatives = plat_cpu_fast();
        return;
    }
    // CPUID leaf B gives the SMT shift in the local APIC ID. IOKit relates that ID to Darwin's CPU number.
    unsigned shift, count, level, unused;
    if (!__get_cpuid_count(0xb, 0, &shift, &count, &level, &unused) ||
        ((level >> 8) & 255) != 1 || !(count & 65535)) return;
    shift &= 31;
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOCPU"), &it)) return;
    unsigned apic[64];
    uint64_t seen = 0;
    io_object_t cpu;
    while ((cpu = IOIteratorNext(it))) {
        int index, id;
        io_registry_entry_t acpi;
        if (registry_id(cpu, CFSTR("IOCPUNumber"), &index) && (unsigned)index < logical &&
            !IORegistryEntryGetParentEntry(cpu, kIOServicePlane, &acpi)) {
            if (registry_id(acpi, CFSTR("processor-lapic"), &id)) {
                apic[index] = (unsigned)id;
                seen |= UINT64_C(1) << index;
            }
            IOObjectRelease(acpi);
        }
        IOObjectRelease(cpu);
    }
    IOObjectRelease(it);
    if (seen != plat_cpu_fast()) return;
    uint64_t first = 0;
    for (unsigned i = 0; i < logical; i++) {
        uint64_t group = 0;
        for (unsigned j = 0; j < logical; j++) {
            if (i != j && apic[i] == apic[j]) return;
            if ((apic[i] >> shift) == (apic[j] >> shift)) group |= UINT64_C(1) << j;
        }
        siblings[i] = group;
        first |= group & -group;
    }
    if ((unsigned)__builtin_popcountll(first) == physical) representatives = first;
}

int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    pthread_once(&topology_once, read_topology);
    *all = plat_cpu_fast();
    *first = representatives;
    return __builtin_popcountll(*first);
}

uint64_t plat_cpu_core_of(int cpu) {
    if ((unsigned)cpu >= 64) return 0;
    pthread_once(&topology_once, read_topology);
    return representatives && siblings[cpu] ? siblings[cpu] : UINT64_C(1) << cpu;
}

// Darwin time-sharing pthreads have a default base priority of 31. Keep all seven levels time-sharing.
enum { NORMAL_BASE = 31 };
void plat_thread_set_priority(PlatThread *t, int prio) {
    struct sched_param p = {.sched_priority = NORMAL_BASE + prio};
    pthread_setschedparam(t ? t->thread : pthread_self(), SCHED_OTHER, &p);
}

void plat_thread_priority(int prio) { plat_thread_set_priority(NULL, prio); }

int plat_thread_priority_get(void) {
    struct sched_param p;
    int policy;
    if (pthread_getschedparam(pthread_self(), &policy, &p) || policy != SCHED_OTHER) return PLAT_PRIO_NORMAL;
    int level = p.sched_priority - NORMAL_BASE;
    if (level <= PLAT_PRIO_IDLE) return PLAT_PRIO_IDLE;
    if (level >= PLAT_PRIO_TIME_CRITICAL) return PLAT_PRIO_TIME_CRITICAL;
    if (level < -2) return PLAT_PRIO_LOWEST;
    if (level > 2) return PLAT_PRIO_HIGHEST;
    return level;
}

int64_t plat_threads_cpu_ns_named(const char *name) {
    thread_act_array_t threads;
    mach_msg_type_number_t count;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return -1;
    int64_t sum = -1;
    for (unsigned i = 0; i < count; i++) {
        thread_extended_info_data_t info;
        mach_msg_type_number_t n = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, (thread_info_t)&info, &n) == KERN_SUCCESS &&
            !strcmp(info.pth_name, name)) {
            if (sum < 0) sum = 0;
            sum += info.pth_user_time + info.pth_system_time;
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof *threads);
    return sum;
}
