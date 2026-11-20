// The SDL3 back end of the window (gfx_wsys.h): the main thread on Darwin and a dedicated window thread elsewhere.
// The window thread runs SDL's event loop, F11 fullscreen (borderless, the desktop's mode), the settings MKW_WINDOW_SIZE and
// MKW_FULLSCREEN, the display's refresh rate, focus, the Vulkan surface. The swapchain and the present are
// gfx_present.c's. Keyboard and mouse events go to input_sdl.c and to the menu (ui/menu/menu_sdl.cpp).
//
// SDL wants its video calls on the thread that initialised video, so this thread does them all: the others hand it
// work through run_on_window (a user event and a wait) or post a user event. Video is initialised before the window
// exists: the Vulkan instance asks for SDL's surface extensions first (gfx_window_vk_extensions), the window comes with
// gfx_window_start (wsys_open).
//
// MKW_WINDOW_SIZE is in pixels. On a display that scales windows (Wayland, HiDPI) the window asks for its pixels in
// the display's units (SDL_GetWindowDisplayScale), and SDL reports mouse positions in those units: input_sdl.c and the
// menu scale them back (gfx_window_sdl_density).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "cfg.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "input.h"
#include "input_sdl.h"
#include "ui/menu/menu.h"
#include "vk.h"
#include "platform/platform.h"

#ifdef __APPLE__
static _Atomic(SDL_Window *) g_win;
static PlatEvent g_runtime_request;   // the runtime requests video, reports a boot error or returns
static const char *g_boot_error;
static int g_video_requested;
#else
static SDL_Window *g_win;
#endif
static uint32_t g_ev_base;               // SDL_RegisterEvents: the user events below
enum { EV_RUN, EV_SETTINGS, EV_CLOSE, EV_COUNT };
static PlatEvent g_main_ready;           // SDL video initialization or the boot dialog returned
static int g_video_ok;
static PlatOnce g_thread_once = PLAT_ONCE_INIT;
static uint32_t g_window_tid;            // the window thread, which runs the video calls
static volatile int32_t g_density_milli = 1000;   // pixels per window unit, x1000

#ifdef __APPLE__
// Present and menu code may hold graphics locks. Read window state without a synchronous call back into Cocoa.
typedef struct {
    int width, height, monitor_width, monitor_height, monitor_ok, minimized, hdr;
    float sdr_white, headroom;
} WindowState;
static WindowState g_window_state = {.hdr = -1};
static PlatMutex g_window_state_lock = PLAT_MUTEX_INIT;
static void update_window_state(void);

static WindowState window_state(void) {
    plat_mutex_lock(&g_window_state_lock);
    WindowState state = g_window_state;
    plat_mutex_unlock(&g_window_state_lock);
    return state;
}
#endif

// ---- work on the window thread
typedef struct {
    void (*fn)(void *arg);
    void *arg;
    PlatEvent done;
} RunReq;

static void run_on_window(void (*fn)(void *arg), void *arg) {
    if (plat_thread_id() == g_window_tid) {
        fn(arg);
        return;
    }
    RunReq r = {fn, arg};
    plat_event_init(&r.done, 1, 0);
    SDL_Event e = {.user = {.type = g_ev_base + EV_RUN, .data1 = &r}};
    if (!SDL_PushEvent(&e)) rt_fatal("gfx: cannot reach the window thread (%s)", SDL_GetError());
    plat_event_wait(&r.done);
}

static void post(int ev) {
    SDL_Event e = {.user = {.type = g_ev_base + (uint32_t)ev}};
    SDL_PushEvent(&e);
}

double gfx_window_sdl_density(void) { return __atomic_load_n(&g_density_milli, __ATOMIC_RELAXED) / 1000.0; }

SDL_Window *gfx_window_sdl(void) { return g_win; }

// ---- settings: size and fullscreen
static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("MKW_WINDOW_SIZE"));
    g_applied_fs = gfx_cfg_fullscreen();
}

static int is_fullscreen(void) { return (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0; }

// The window's size in its units for w x h pixels.
static void set_pixel_size(int w, int h) {
    float scale = SDL_GetWindowDisplayScale(g_win);
    if (!(scale > 0)) scale = 1;
    SDL_SetWindowSize(g_win, (int)(w / scale + 0.5f), (int)(h / scale + 0.5f));
}

static void set_fullscreen(int on) {
    input_cursor_suspend(1);
    SDL_SetWindowFullscreen(g_win, on);   // the desktop's mode: borderless over the display
    SDL_SyncWindow(g_win);
    input_cursor_suspend(0);
}

// Window thread: the window takes the size and fullscreen state of the settings when they changed. A window resized by
// hand keeps its size when another setting changes.
static void apply_settings(void) {
    int size_changed = strcmp(cfg_value("MKW_WINDOW_SIZE"), g_applied_size) != 0, fs = gfx_cfg_fullscreen();
    int fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int w, h;
        gfx_cfg_window_size(&w, &h);
        if (is_fullscreen()) SDL_SetWindowFullscreen(g_win, false);
        set_pixel_size(w, h);
        if (is_fullscreen() || (fs && !fs_changed)) SDL_SetWindowFullscreen(g_win, true);
    }
    if (fs_changed && fs != is_fullscreen()) set_fullscreen(fs);
#ifdef __APPLE__
    update_window_state();
#endif
}

void gfx_window_apply_settings(void) {
    if (g_win) post(EV_SETTINGS);
}

void gfx_window_request_close(void) {
    if (g_win) post(EV_CLOSE);
}

// ---- the display's refresh rate and size
static void refresh_rate_update(void) {
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_win));
    double hz = 0;
    if (m && m->refresh_rate_denominator) hz = (double)m->refresh_rate_numerator / m->refresh_rate_denominator;
    else if (m) hz = m->refresh_rate;
    gfx_window_set_refresh(hz);
}

static int read_monitor_size(int *w, int *h) {
    if (!g_win) return 0;
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_win));
    if (!m) return 0;
    *w = (int)(m->w * m->pixel_density + 0.5f);
    *h = (int)(m->h * m->pixel_density + 0.5f);
    return 1;
}

int gfx_window_monitor_size(int *w, int *h) {
#ifdef __APPLE__
    WindowState state = window_state();
    if (state.monitor_ok) *w = state.monitor_width, *h = state.monitor_height;
    return state.monitor_ok;
#else
    return read_monitor_size(w, h);
#endif
}

static int video_up(void);

typedef struct { int w, h; } DisplaySize;

static void read_desktop_size(void *arg) {
    DisplaySize *size = arg;
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (m) size->w = (int)(m->w * m->pixel_density + 0.5f), size->h = (int)(m->h * m->pixel_density + 0.5f);
}

// The lines of the picture the window will show, for the game's output size before the window exists
// (patches.c): the monitor's in fullscreen (its desktop mode, in pixels), else MKW_WINDOW_SIZE's. 0 when unknown.
int gfx_window_shape(int *w, int *h) {
    if (!gfx_cfg_fullscreen()) {
        gfx_cfg_window_size(w, h);
        return *h > 0;
    }
    if (!video_up()) return 0;
    DisplaySize size = {0};
#ifdef __APPLE__
    run_on_window(read_desktop_size, &size);
#else
    read_desktop_size(&size);
#endif
    if (!size.h) return 0;
    *w = size.w, *h = size.h;
    return 1;
}

int gfx_window_lines(void) {
    int w, h;
    return gfx_window_shape(&w, &h) ? h : 0;
}

static void density_update(void) {
    float d = SDL_GetWindowPixelDensity(g_win);
    __atomic_store_n(&g_density_milli, (int32_t)((d > 0 ? d : 1) * 1000 + 0.5f), __ATOMIC_RELAXED);
}

#ifdef __APPLE__
static void close_runtime(void *arg) {
    (void)arg;
    gfx_window_closed();
}
#endif

static void close_window(void) {
#ifdef __APPLE__
    // Saving can wait on workers that still need the main thread to process window requests.
    static int closing;
    if (closing) return;
    closing = 1;
    if (!plat_thread_spawn(close_runtime, NULL, "window-close", 0)) rt_fatal("gfx: cannot start window shutdown");
#else
    gfx_window_closed();
#endif
}

// ---- the window thread
static void handle(const SDL_Event *e) {
    if (e->type == g_ev_base + EV_RUN) {
        RunReq *r = e->user.data1;
        r->fn(r->arg);
        plat_event_set(&r->done);
        return;
    }
    if (e->type == g_ev_base + EV_SETTINGS) {
        if (g_win) apply_settings();
        return;
    }
    if (e->type == g_ev_base + EV_CLOSE) {
        close_window();
        return;
    }
    if (!g_win) return;
    input_sdl_event(e);
    menu_sdl_event(e);
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_F11 && !e->key.repeat) {
            set_fullscreen(!is_fullscreen());
#ifdef __APPLE__
            update_window_state();
#endif
            cfg_set("MKW_FULLSCREEN", is_fullscreen() ? "1" : "0");   // the menu shows it; the next start keeps it
            cfg_save();
            note_applied();
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        density_update();
#ifdef __APPLE__
        update_window_state();
#endif
        gfx_window_mark_dirty();
        break;
    case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:   // the system turned HDR on or off: the swapchain follows (gfx_hdr.c)
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        density_update();
        refresh_rate_update();
#ifdef __APPLE__
        update_window_state();
#endif
        gfx_window_mark_dirty();
        break;
#ifdef __APPLE__
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        update_window_state();
        break;
#endif
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        gfx_window_focus_changed(e->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        break;
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        close_window();
        break;
    }
}

void app_icon_set(SDL_Window *w);   // build/obj/rt/app_icon.c, drawn by tools/build/icon.py

// Started by Steam on a Wayland desktop (cfg_steam_video_driver): XWayland lets Steam give the controller to the game,
// but SDL's X11 driver cannot tell HDR (wsys_hdr: SDR there). So X11 only when the picture is SDR anyway: MKW_HDR off,
// or no display in HDR as Wayland reports it (SDL fills the displays' HDR from wp_color_management at init, no window).
static const char *steam_video_driver(void) {
    const char *drv = cfg_steam_video_driver();
    if (!drv) return NULL;
    int hdr = 0, hdr_off = !strcmp(cfg_value("MKW_HDR"), "off");
    if (!hdr_off) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
        if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            int n = 0;
            SDL_DisplayID *ids = SDL_GetDisplays(&n);
            for (int i = 0; ids && i < n && !hdr; i++)
                hdr = SDL_GetBooleanProperty(SDL_GetDisplayProperties(ids[i]), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
            SDL_free(ids);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    }
    if (hdr) {
        rt_log("gfx: started by Steam in a Wayland session with a display in HDR: native Wayland keeps HDR, Steam may "
               "not give the controller to the game (MKW_HDR=off for SDL's %s driver)", drv);
        return NULL;
    }
    rt_log("gfx: started by Steam in a Wayland session, SDR (%s): SDL's %s driver, so Steam sees the window take the "
           "focus (MKW_STEAM_X11=0 for native Wayland)", hdr_off ? "MKW_HDR off" : "no display in HDR", drv);
    return drv;
}

static void window_init(void) {
    g_window_tid = plat_thread_id();
    // Wayland's app id: the desktop file and the taskbar match it. Our background runs take another one, so a
    // compositor rule can keep them from taking the focus without touching the player's window (KWin ignores
    // NOT_FOCUSABLE on a new window: tools/run/kwin-rule.sh).
    const char *bg = cfg_get("MKW_BACKGROUND_WINDOW");
    SDL_SetHint(SDL_HINT_APP_ID, bg && strcmp(bg, "0") ? "mkw-recomp-run" : "mkw-recomp");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    const char *steam_drv = steam_video_driver();   // started by Steam on a Wayland desktop, SDR: XWayland
    if (steam_drv) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, steam_drv);
    g_video_ok = SDL_Init(SDL_INIT_VIDEO);
    if (!g_video_ok) rt_log("gfx: SDL video init failed (%s)", SDL_GetError());
    // SDL's Vulkan queries need its Vulkan loader, which a window would load at creation: the instance and the device
    // are made before the window.
    else if (!SDL_Vulkan_LoadLibrary(NULL)) {
        rt_log("gfx: SDL cannot load Vulkan (%s)", SDL_GetError());
        g_video_ok = 0;
    } else {
        g_ev_base = SDL_RegisterEvents(EV_COUNT);
        if (!g_ev_base) {
            rt_log("gfx: SDL cannot register window events (%s)", SDL_GetError());
            g_video_ok = 0;
        } else rt_log("gfx: SDL %d.%d.%d, video driver %s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
               SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());
    }
    plat_event_set(&g_main_ready);
}

static void window_pump(void) {
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, 100)) handle(&e);
    else input_sdl_idle();   // a cursor change requested from another thread meanwhile
}

#ifdef __APPLE__
typedef struct {
    int (*run)(int, char **);
    int argc;
    char **argv;
    int result, done;
} RuntimeCall;

static void run_runtime_thread(void *arg) {
    RuntimeCall *call = arg;
    call->result = call->run(call->argc, call->argv);
    __atomic_store_n(&call->done, 1, __ATOMIC_RELEASE);
    plat_event_set(&g_runtime_request);
}

int wsys_run_runtime(int (*run)(int, char **), int argc, char **argv) {
    RuntimeCall call = {.run = run, .argc = argc, .argv = argv};
    plat_event_init(&g_runtime_request, 1, 0);
    plat_event_init(&g_main_ready, 1, 0);
    PlatThread *worker = plat_thread_new(run_runtime_thread, &call, "runtime");
    if (!worker) rt_fatal("gfx: cannot start the runtime thread");
    // Preserve lazy video initialization for probes and modes that never request a window.
    plat_event_wait(&g_runtime_request);
    if (g_boot_error) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "mkw-recomp", g_boot_error, NULL);
        plat_event_set(&g_main_ready);
    } else if (__atomic_load_n(&g_video_requested, __ATOMIC_ACQUIRE)) {
        window_init();
        while (g_video_ok && !__atomic_load_n(&call.done, __ATOMIC_ACQUIRE)) window_pump();
    }
    plat_thread_join(worker);
    return call.result;
}

void wsys_show_boot_error(const char *message) {
    // The profile lock is checked before the runtime can request video.
    g_boot_error = message;
    plat_event_set(&g_runtime_request);
    plat_event_wait(&g_main_ready);
}
#else
static void window_main(void *arg) {
    (void)arg;
    window_init();
    while (g_video_ok) window_pump();
}
#endif

static void video_start(void *arg) {
    (void)arg;
#ifdef __APPLE__
    __atomic_store_n(&g_video_requested, 1, __ATOMIC_RELEASE);
    plat_event_set(&g_runtime_request);
#else
    plat_event_init(&g_main_ready, 1, 0);
    if (!plat_thread_spawn(window_main, NULL, "window", 0)) rt_fatal("gfx: cannot start the window thread");
#endif
    plat_event_wait(&g_main_ready);
}

static int video_up(void) {
    plat_once(&g_thread_once, video_start, NULL);
    return g_video_ok;
}

int gfx_window_vk_extensions(const char **out, int max) {
    if (!video_up()) return 0;
    Uint32 n = 0;
    const char *const *ext = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!ext) {
        rt_log("gfx: SDL_Vulkan_GetInstanceExtensions failed (%s)", SDL_GetError());
        return 0;
    }
    int k = 0;
    for (Uint32 i = 0; i < n && k < max; i++) out[k++] = ext[i];
    return k;
}

int gfx_window_vk_presentable(VkInstance inst, VkPhysicalDevice pd, uint32_t family) {
    return video_up() && SDL_Vulkan_GetPresentationSupport(inst, pd, family);
}

// ---- gfx_wsys.h
static int open_ok;

static void open_window(void *arg) {
    (void)arg;
    if (g_win) return;
    int w, h;
    gfx_cfg_window_size(&w, &h);
    // MKW_BACKGROUND_WINDOW=1 (our runs and replays, never the player's default): the window opens without taking the
    // focus or coming to the front, so a run started by an agent does not interrupt whoever is at the desktop (Adrià,
    // 07/11/2026). The game would pause without focus: the run scripts pin it with MKW_ASSUME_FOCUS.
    // =hidden (mkw --replay): no window on screen at all; the presents are read back from their textures.
    const char *bg = cfg_get("MKW_BACKGROUND_WINDOW");
    int background = bg && strcmp(bg, "0"), hidden = bg && !strcmp(bg, "hidden");
    if (background) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    g_win = SDL_CreateWindow("MKW Recomp", w, h,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                 (background ? SDL_WINDOW_NOT_FOCUSABLE : 0) | (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!g_win) {
        rt_log("gfx: SDL_CreateWindow failed (%s)", SDL_GetError());
        return;
    }
    app_icon_set(g_win);
    set_pixel_size(w, h);   // w x h were window units; the setting is in pixels
    density_update();
    note_applied();
    refresh_rate_update();
    if (background) rt_log("gfx: window opened in the background, not focusable (MKW_BACKGROUND_WINDOW)");
    if (gfx_cfg_fullscreen() && !background) SDL_SetWindowFullscreen(g_win, true);
    SDL_SyncWindow(g_win);
    input_sdl_window(g_win);
#ifdef __APPLE__
    update_window_state();
#endif
    open_ok = 1;
}

int wsys_open(void) {
    if (!video_up()) return 0;
#ifdef __APPLE__
    run_on_window(open_window, NULL);
#else
    if (!g_win) run_on_window(open_window, NULL);
#endif
    return open_ok;
}

typedef struct { VkInstance inst; VkSurfaceKHR surface; } SurfaceReq;

static void make_surface(void *arg) {
    SurfaceReq *r = arg;
    if (!SDL_Vulkan_CreateSurface(g_win, r->inst, NULL, &r->surface)) {
        rt_log("gfx: SDL_Vulkan_CreateSurface failed (%s)", SDL_GetError());
        r->surface = VK_NULL_HANDLE;
    }
}

VkSurfaceKHR wsys_surface(VkInstance inst) {
    SurfaceReq r = {inst, VK_NULL_HANDLE};
    run_on_window(make_surface, &r);
    return r.surface;
}

static void read_client_size(int *w, int *h) {
    *w = *h = 0;
    if (g_win) SDL_GetWindowSizeInPixels(g_win, w, h);
}

static int read_hdr(float *sdr_white, float *headroom) {
    *sdr_white = *headroom = 0;
    if (!g_win) return -1;
    SDL_PropertiesID p = SDL_GetWindowProperties(g_win);
    if (!p || !SDL_HasProperty(p, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN)) return -1;
    // SDL's X11 driver sets the property but never to true (it cannot know). Inside gamescope (its Xwayland: Steam
    // Deck's game mode) that is unknown, so the surface's formats decide (gfx_hdr.c: HDR10 only with its HDR on); hdr5,
    // 12/11/2026, was "not in HDR" there. Elsewhere on X11 it stays SDR: a driver may list HDR10 on an SDR screen.
    const char *drv = SDL_GetCurrentVideoDriver();
    int gamescope = drv && !strcmp(drv, "x11") && SDL_getenv("GAMESCOPE_WAYLAND_DISPLAY");
    if (!SDL_GetBooleanProperty(p, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false)) return gamescope ? -1 : 0;
    *sdr_white = SDL_GetFloatProperty(p, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 0.0f);
    *headroom = SDL_GetFloatProperty(p, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 0.0f);
    return 1;
}

static int read_minimized(void) { return g_win && (SDL_GetWindowFlags(g_win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)); }

#ifdef __APPLE__
static void update_window_state(void) {
    WindowState state = {.minimized = read_minimized()};
    read_client_size(&state.width, &state.height);
    state.monitor_ok = read_monitor_size(&state.monitor_width, &state.monitor_height);
    state.hdr = read_hdr(&state.sdr_white, &state.headroom);
    plat_mutex_lock(&g_window_state_lock);
    g_window_state = state;
    plat_mutex_unlock(&g_window_state_lock);
}
#endif

void wsys_client_size(int *w, int *h) {
#ifdef __APPLE__
    WindowState state = window_state();
    *w = state.width, *h = state.height;
#else
    read_client_size(w, h);
#endif
}

int wsys_hdr(float *sdr_white, float *headroom) {
#ifdef __APPLE__
    WindowState state = window_state();
    *sdr_white = state.sdr_white, *headroom = state.headroom;
    return state.hdr;
#else
    return read_hdr(sdr_white, headroom);
#endif
}

int wsys_minimized(void) {
#ifdef __APPLE__
    return window_state().minimized;
#else
    return read_minimized();
#endif
}

static void resize_window(void *arg) {
    const int *wh = arg;
    set_pixel_size(wh[0], wh[1]);
    SDL_SyncWindow(g_win);
#ifdef __APPLE__
    update_window_state();
#endif
}

void wsys_resize(int w, int h) {
    int wh[2] = {w, h};
    run_on_window(resize_window, wh);
}

// ---- --probe-window: synthetic events through the event handler, on the window thread
static void probe_event(void *arg) {
    SDL_Event *e = arg;
    e->window.windowID = SDL_GetWindowID(g_win);   // same field in SDL's window, key and button events
    handle(e);
}

void wsys_probe_focus(int in) {
    SDL_Event e = {.window = {.type = in ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST}};
    run_on_window(probe_event, &e);
}

void wsys_probe_escape(void) {
    SDL_Event e = {.key = {.type = SDL_EVENT_KEY_DOWN, .scancode = SDL_SCANCODE_ESCAPE, .key = SDLK_ESCAPE, .down = true}};
    run_on_window(probe_event, &e);
    e.key.type = SDL_EVENT_KEY_UP, e.key.down = false;
    run_on_window(probe_event, &e);
}

// SDL reports no activation by a title bar click: only the client click exists.
int wsys_probe_click(int where) {
    if (where == 2) return 0;
    SDL_Event e = {.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN, .button = SDL_BUTTON_LEFT, .down = true}};
    run_on_window(probe_event, &e);
    e.button.type = SDL_EVENT_MOUSE_BUTTON_UP, e.button.down = false;
    run_on_window(probe_event, &e);
    return 1;
}
