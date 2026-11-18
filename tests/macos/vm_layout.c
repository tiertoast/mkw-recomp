// Synthetic configuration and module metadata shared by the Mach VM and NVK tests.
#include "cfg.h"
#include "gamelib.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULE (440ull << 30)
#define PAGE 4096ull

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

// Fixture inputs for configuration and the module list. All reservation, alias and permission operations are real.
void cfg_init(void) {}
const char *cfg_get(const char *key) { return !strcmp(key, "MKW_REEXEC") ? getenv(key) : NULL; }
const RtGame *rt_game(void) {
    static const RtGameModule mod = {.name = "fixture", .base = MODULE, .size = 16 * PAGE};
    static const RtGame g = {.modules = &mod, .nmodules = 1};
    return &g;
}
int ldr_count(void) { return 1; }
const RtLoaded *ldr_module(int i) {
    static const RtLoaded mod = {.name = "fixture", .base = MODULE, .size = 16 * PAGE};
    if (i != 0) rt_fatal("fixture: unknown module index %d", i);
    return &mod;
}
