// Run through the real loader, linker tables, seal, dispatch and module ABI without any game input.
#include "host.h"
#include "gamelib.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fixture_dir;
static void first(Cpu *c) { c->x[0]++; }
HOST_OVERRIDE("duplicate", first);

static PlatEvent start;
static void discover(void *arg) {
    plat_event_wait(&start);
    *(const RtGame **)arg = rt_game();
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 2;
    fixture_dir = argv[1];
    if (argc == 3 && !strcmp(argv[2], "--present")) return rt_game_present() ? 0 : 1;
    const RtGame *seen[4];
    PlatThread *threads[4];
    plat_event_init(&start, 1, 0);
    for (unsigned i = 0; i < 4; ++i) {
        threads[i] = plat_thread_new(discover, &seen[i], "discover");
        if (!threads[i]) return 1;
    }
    plat_event_set(&start);
    for (unsigned i = 0; i < 4; ++i) plat_thread_join(threads[i]);
    const RtGame *g = rt_game();
    for (unsigned i = 0; i < 4; ++i) if (seen[i] != g) return 1;
    if (g->nmodules != 1 || strcmp(g->modules[0].name, "fixture")) return 1;
    GuestFn twice = hostfn_override("archive");
    if (!twice || hostfn_override("duplicate") != first || hostfn_observer("observed") != twice ||
        hostfn_table_size(0) != 3 || hostfn_table_size(1) != 1 || hostfn_override("absent")) return 1;
    uint64_t registered = hostfn_register("twice", twice);
    if (hostfn_register("twice", twice) != registered) return 1;
    hostfn_seal();
    hostfn_seal();
    uint64_t addr = hostfn_address(twice);
    if (strcmp(hostfn_name(addr), "twice") || addr != RT_HOSTFN_BASE) return 1;
    Cpu c = {0};
    if (rt_call(&c, g->modules[0].base, 2, (uint64_t)11, addr) != 25 || hostfn_calls() != 1 || c.sj) return 1;
    puts("PASS module -> host ABI -> module -> registered host function: 11 * 2 + 3 = 25");
    return 0;
}
