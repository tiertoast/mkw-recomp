// Fixture dependencies of the real dispatcher and registry. Entering a guest kernel/service path fails the test.
#include "host.h"
#include "gamelib.h"
#include "cfg.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <libproc.h>
#include <math.h>
#include <unistd.h>

extern const char *fixture_dir;
const char *cfg_game_dir(void) { return fixture_dir; }
const RtGame *rt_game_builtin(void) { return NULL; }
const char *cfg_get(const char *name) { return getenv(name); }
unsigned rt_trace;
volatile uint32_t prof_render_tid;
int kern_host_track;

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

void kern_checkpoint(void) { abort(); }
int kern_core_away(void) { abort(); }
void kern_core_back(int *away) { (void)away; abort(); }
void kern_host_left(void *t) { (void)t; abort(); }
void rt_spin_gate(Cpu *c, uint64_t head) { (void)c; (void)head; abort(); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; abort(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; abort(); }
uint64_t rt_ticks(void) { return plat_now_ns() / RT_TICK_NS; }
