// Exercise Darwin services without installing a game or modifying system preferences.
#include "platform/platform.h"
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "Darwin services:%d: %s\n", __LINE__, #cond); exit(1); \
} } while (0)

static void time_locale(void) {
    char zone[128], system_zone[128];
    CHECK(!plat_time_zone(zone, 1) && plat_time_zone(zone, sizeof zone));
    CFTimeZoneRef system = CFTimeZoneCopySystem();
    CHECK(system && CFStringGetCString(CFTimeZoneGetName(system), system_zone, sizeof system_zone, kCFStringEncodingUTF8));
    CFRelease(system);
    CHECK(!strcmp(zone, system_zone));

    CHECK(!strcmp(plat_ui_language(), "fr-CA"));
    CHECK(!strcmp(plat_user_locale(), "en-IT"));   // The CoreFoundation identifier also has a calendar keyword.
    CHECK(plat_user_locale() == plat_user_locale() && plat_ui_language() == plat_ui_language());

    CHECK(plat_setenv("TZ", "America/New_York"));
    CHECK(plat_tz_west() == 18000);
    struct tm t;
    CHECK(plat_gmtime(0, &t) && t.tm_year == 70 && t.tm_mon == 0 && t.tm_mday == 1);
    CHECK(plat_localtime(0, &t) && t.tm_hour == 19 && t.tm_gmtoff == -18000 && !t.tm_isdst);
    CHECK(plat_localtime(1721000000, &t) && t.tm_gmtoff == -14400 && t.tm_isdst == 1);
    CHECK(plat_tz_west() == 18000);
    CHECK(plat_gmtime(PLAT_TIME_MAX, &t) && plat_localtime(PLAT_TIME_MAX, &t));
    CHECK(!plat_gmtime(PLAT_TIME_MAX + 1, &t) && !plat_localtime(-1, &t));

    time_t before = time(NULL);
    uint64_t now = plat_unix_time_ns() / 1000000000u;
    CHECK(now >= (uint64_t)before && now <= (uint64_t)time(NULL));
    plat_local_time(&t);
    CHECK(t.tm_gmtoff == -14400 || t.tm_gmtoff == -18000);
}

static void unexpected_exit_handler(void) {
    const char msg[] = "atexit ran";
    (void)write(STDERR_FILENO, msg, sizeof msg - 1);
}

static void run_child(const char *mode, const char *arg) {
    CHECK(!strcmp(arg, "argument with spaces"));
    CHECK(plat_process_alive(plat_process_id()));
    char saved_pid[32];
    if (!strcmp(mode, "reexec")) {
        const char *pid = getenv("MKW_SERVICE_REEXEC_PID");
        snprintf(saved_pid, sizeof saved_pid, "%u", plat_process_id());
        if (!pid) {
            CHECK(plat_setenv("MKW_SERVICE_REEXEC_PID", saved_pid));
            plat_reexec();
        }
        CHECK(!strcmp(pid, saved_pid) && !atexit(unexpected_exit_handler));
        CHECK(write(STDOUT_FILENO, "out", 3) == 3 && write(STDERR_FILENO, "err", 3) == 3);
        plat_exit(23);
    }
    if (!strcmp(mode, "wait")) {
        CHECK(write(STDOUT_FILENO, "waiting", 7) == 7);
        for (;;) pause();
    }
    CHECK(!strcmp(mode, "fill"));
    char block[4096];
    memset(block, 'x', sizeof block);
    for (int i = 0; i < 64; i++) CHECK(write(STDOUT_FILENO, block, sizeof block) == sizeof block);
    plat_exit(17);
}

static void stderr_file(void) {
    char path[PATH_MAX], dir[PATH_MAX];
    CHECK(plat_temp_dir(dir, sizeof dir));
    int n = snprintf(path, sizeof path, "%s/mkw-stderr.XXXXXX", dir);
    CHECK(n > 0 && (size_t)n < sizeof path);
    int fd = mkstemp(path), saved = dup(STDERR_FILENO);
    CHECK(fd >= 0 && saved >= 0 && dup2(fd, STDERR_FILENO) >= 0);
    char actual[PATH_MAX], expected[PATH_MAX];
    int ok = plat_stderr_path(actual, sizeof actual) && realpath(path, expected) &&
             !strcmp(actual, expected) && !plat_stderr_path(actual, 1);
    CHECK(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    close(fd);
    CHECK(!unlink(path) && ok);
}

static void process_loading(const char *library) {
    char out[128];
    CHECK(plat_process_id() == (uint32_t)getpid() && plat_process_alive(plat_process_id()));
    CHECK(!plat_process_alive(0) && !plat_process_alive(UINT32_MAX));
    uint64_t age = plat_process_uptime_ns();
    CHECK(age > 0 && plat_process_uptime_ns() >= age);
    CHECK(plat_os_version(out, sizeof out) && !strncmp(out, "macOS ", 6) && !plat_os_version(out, 1));
    stderr_file();

    unsigned char first[600], second[600];
    CHECK(plat_random(NULL, 0) && plat_random(first, sizeof first) && plat_random(second, sizeof second));
    CHECK(memcmp(first, second, sizeof first) != 0);
    void *system = plat_lib_open("libSystem.B.dylib");
    CHECK(system && plat_lib_sym(system, "getpid") && !plat_lib_sym(system, "mkw_missing_symbol") && !plat_lib_sym(NULL, "getpid"));
    const char *leaf = strrchr(library, '/');
    CHECK(leaf && !plat_lib_open(leaf + 1));   // The fixture dylib exists in CTest's working directory.
    void *local = plat_lib_open(library);
    void *symbol = plat_lib_sym(local, "mkw_bind_host_v1");
    uintptr_t base = 0;
    CHECK(local && symbol && plat_module_of((uintptr_t)symbol, out, sizeof out, &base) && base);
    CHECK(!strncmp(out, "libmkw-host.", 12) && !plat_module_of((uintptr_t)symbol, out, 1, &base));
    CHECK(!plat_module_of(0, out, sizeof out, &base));

    CHECK(plat_setenv("MKW_SERVICE_MODE", "parent") && plat_setenv("MKW_SERVICE_REEXEC_PID", NULL));
    CHECK(plat_run_self("MKW_SERVICE_MODE", "reexec", out, sizeof out, 3000) == 23 && !strcmp(out, "outerr"));
    CHECK(!strcmp(getenv("MKW_SERVICE_MODE"), "parent") && !getenv("MKW_SERVICE_REEXEC_PID"));
    CHECK(plat_run_self("MKW_SERVICE_MODE", "fill", out, 4, 0) == 17 && !strcmp(out, "xxx"));
    CHECK(plat_run_self("MKW_SERVICE_MODE", "wait", out, sizeof out, 300) == 128 + SIGKILL);
    CHECK(plat_run_self("BAD=KEY", "", out, sizeof out, 10) == -1 && !*out);
}

int main(int argc, char **argv) {
    CHECK(argc >= 2);
    if (!strcmp(argv[1], "time")) {
        time_locale();
    } else {
        CHECK(argc == 4 && !strcmp(argv[1], "process"));
        const char *mode = getenv("MKW_SERVICE_MODE");
        if (mode && strcmp(mode, "parent")) run_child(mode, argv[3]);
        process_loading(argv[2]);
    }
    return 0;
}
