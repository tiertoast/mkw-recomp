// The game code's tables (gamelib.h) and what the runtime reads from them. build/mkw links them in; the player's build
// loads them from mkwgame.so / mkwgame.dll in the selected game folder, which the start screen's setup builds from their own
// game files (SavageWindRecomp runtime/gamelib.c, MIT: same idea, one entry point found by name, an id check).
#ifdef __APPLE__
#include <dlfcn.h>
#include <pthread.h>
// Require local implementations, even though the module ABI dylib exports the same C names.
#pragma GCC visibility push(hidden)
#include "darwin_abi.h"
#pragma GCC visibility pop
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "gamelib.h"
#include "host.h"
#include "platform/platform.h"

// Keep Darwin loader policy outside the shared, fingerprinted module table header.
#ifdef __APPLE__
static const char module_file[] = "mkwgame.dylib";
#else
static const char module_file[] = RT_GAME_LIB;
#endif

static const RtGame *game;

static const char *check(const RtGame *g) {
    if (g->abi != RT_GAME_ABI || !g->id || strcmp(g->id, RT_GAME_ID)) return "built for another version of the runtime";
    if (g->nmodules < 1 || g->nmodules > RT_MAX_MODULES || g->npatch_sets > RT_MAX_PATCH_SETS) return "malformed";
    return NULL;
}

// The library in the selected game folder, its tables if they suit this runtime; why not in *why.
static const RtGame *load_library(const char **why) {
    char path[PLAT_PATH_MAX * 2];
    *why = "the game files are not set up";
    snprintf(path, sizeof path, "%s/%s", cfg_game_dir(), module_file);
#ifdef __APPLE__
    static const RtDarwinHostV1 host = {rt_dispatch, rt_sj_owner, rt_svc, rt_udf, rt_ticks, rt_spin_gate, fnprof_hit};
    if (!mkw_bind_host_v1(&host)) {
        *why = "incompatible Darwin host binding";
        return NULL;
    }
    // Bind imports before accepting the tables; resolve the entry in this image only.
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    if (!lib) {
        rt_log("dlopen(%s): %s", path, dlerror());
        *why = "the game code library does not load";
        return NULL;
    }
    const RtGame *(*entry)(void) = (const RtGame *(*)(void))dlsym(lib, RT_GAME_ENTRY);
    if (!entry) rt_log("dlsym(%s): %s", RT_GAME_ENTRY, dlerror());
#else
    if (!plat_file_exists(path)) return NULL;
    void *lib = plat_lib_open(path);
    const RtGame *(*entry)(void) = lib ? (const RtGame *(*)(void))plat_lib_sym(lib, RT_GAME_ENTRY) : NULL;
#endif
    const RtGame *g = entry ? entry() : NULL;
    *why = !lib ? "the game code library does not load" : !g ? "the game code library has no tables" : check(g);
#ifdef __APPLE__
    if (*why) dlclose(lib);
#endif
    return *why ? NULL : g;
}

#ifdef __APPLE__
static pthread_once_t discover_once = PTHREAD_ONCE_INIT;
#else
static int looked;
#endif
static const char *failure;

static void discover(void) {
    const RtGame *g = rt_game_builtin();
    if (g) failure = check(g);
    else g = load_library(&failure);
    if (!failure) __atomic_store_n(&game, g, __ATOMIC_RELEASE);
}

const RtGame *rt_game(void) {
    const RtGame *g = __atomic_load_n(&game, __ATOMIC_ACQUIRE);
    if (g) return g;
#ifdef __APPLE__
    pthread_once(&discover_once, discover);
#else
    if (!looked) {
        looked = 1;
        discover();
    }
#endif
    if (failure) rt_fatal("game code: %s", failure);
    return __atomic_load_n(&game, __ATOMIC_ACQUIRE);
}

// Without loading it: the start screen keeps no library mapped, so a new setup can replace it (Windows does not let a
// loaded DLL be overwritten). The id file beside it (build.py --dist, prepare.py) says which headers it was built with;
// the game's own start loads it and checks it again.
int rt_game_present(void) {
    if (__atomic_load_n(&game, __ATOMIC_ACQUIRE) || rt_game_builtin()) return 1;
    char path[PLAT_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/%s", cfg_game_dir(), module_file);
    if (!plat_file_exists(path)) return 0;
    snprintf(path, sizeof path, "%s/mkwgame.id", cfg_game_dir());
    uint64_t size = 0;
    char *id = (char *)plat_file_read(path, &size);
#ifdef __APPLE__
    int ok = id && size == strlen(RT_GAME_ID) && !memcmp(id, RT_GAME_ID, size);
#else
    int ok = id && size >= strlen(RT_GAME_ID) && !strncmp(id, RT_GAME_ID, strlen(RT_GAME_ID));
#endif
    free(id);
    return ok;
}

void rt_fnprof_set(int on) { *rt_game()->fnprof = on; }

int rt_linked_modules(RtModule *out, int max) {
    const RtGame *g = rt_game();
    int n = 0;
    for (; n < g->nmodules && n < max; n++) {
        const RtGameModule *m = &g->modules[n];
        out[n] = (RtModule){m->name, m->base, m->size, m->addrs, m->fns, *m->count};
    }
    return n;
}

int rt_game_version(void) { return rt_game()->version; }
