// A synthetic module with a single callable address. Its imports bind only to the versioned host ABI.
#include "gamelib.h"

static void body(Cpu *c) {
    c->pc = c->x[1];
    rt_dispatch(c);
    c->x[0] += 3;
}
static void enter(Cpu *c) {
    rt_sj_owner(c, body, 0x1000, 0x1004);
}

RT_GAME_EXPORT const RtGame *mkwgame_tables(void) {
    static const uint64_t address = 0x1000, count = 1;
    static const GuestFn fn = enter;
    static const RtGameModule module = {"fixture", 0x1000, 4, &address, &fn, &count};
    static volatile int profile;
    static uint8_t patches[RT_MAX_PATCH_SETS];
    static const RtGame tables = {.abi = RT_GAME_ABI, .id = RT_GAME_ID, .modules = &module, .nmodules = 1,
                                  .fnprof = &profile, .patch_on = patches};
    return &tables;
}
