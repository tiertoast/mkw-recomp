// Diagnostics and optional services shared by the synthetic ABI and kernel fixtures.
#include "host.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <libproc.h>
#include <math.h>
#include <unistd.h>

unsigned rt_trace;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
void rt_vlogf(const char *fmt, va_list ap) { vfprintf(stderr, fmt, ap); }
void rt_log_raw(const char *p, size_t n) { fwrite(p, 1, n, stderr); }
const char *ldr_describe(uint64_t a, char *buf, int n) { return rt_guest_name(a, buf, n); }
void rt_patch_hosts(void) { /* This module declares no patches. */ }

// rt_fatal prints process diagnostics even for a rejected library. Mark unavailable counters explicitly.
void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    struct proc_taskinfo info;
    *resident = proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &info, sizeof info) == sizeof info ? info.pti_resident_size : 0;
    *private_bytes = 0;
}
void plat_mem_info(float *used, float *total, float *proc) {
    uint64_t rss, ignored;
    plat_process_memory(&rss, &ignored);
    *proc = rss / 1048576.0f;
    *total = sysconf(_SC_PHYS_PAGES) * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
    *used = NAN;
}

void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; abort(); }
uint64_t rt_ticks(void) { return plat_now_ns() / RT_TICK_NS; }
