// Darwin process, executable and dynamic-library services.
#include "platform.h"
#include <crt_externs.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int plat_setenv(const char *name, const char *value) {
    return (value && *value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

_Noreturn void plat_reexec(void) {
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *path = malloc(size);
    if (path && !_NSGetExecutablePath(path, &size)) execv(path, *_NSGetArgv());
    perror("Darwin VM: re-executing the process");
    _Exit(70);
}

uintptr_t plat_exe_base(void) { return (uintptr_t)_dyld_get_image_header(0); }

int plat_exe_path(char *out, size_t cap) {
    uint32_t size = cap > UINT32_MAX ? UINT32_MAX : (uint32_t)cap;
    return size && !_NSGetExecutablePath(out, &size);
}

void plat_exit(int code) { _exit(code); }

uint32_t plat_process_id(void) { return (uint32_t)getpid(); }

int plat_process_alive(uint32_t pid) {
    if (!pid || pid > INT_MAX) return 0;   // kill() interprets nonpositive IDs as process groups.
    return !kill((pid_t)pid, 0) || errno == EPERM;
}

uint64_t plat_process_uptime_ns(void) {
    struct proc_bsdinfo info;
    if (proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &info, sizeof info) != sizeof info) return 0;
    // proc_pidinfo exposes the process start as a Unix timestamp, in microseconds.
    uint64_t start = info.pbi_start_tvsec * 1000000000ull + info.pbi_start_tvusec * 1000ull;
    uint64_t now = clock_gettime_nsec_np(CLOCK_REALTIME);
    return start && now > start ? now - start : 0;
}

int plat_random(void *out, size_t n) {
    unsigned char *p = out;
    while (n) {
        size_t count = n < 256 ? n : 256;
        if (getentropy(p, count)) {
            if (errno == EINTR) continue;
            return 0;
        }
        p += count;
        n -= count;
    }
    return 1;
}

static int copy_string(char *out, size_t cap, const char *value) {
    size_t n = strlen(value);
    if (n >= cap) return 0;
    memcpy(out, value, n + 1);
    return 1;
}

int plat_os_version(char *out, size_t cap) {
    char version[64], build[64];
    size_t n = sizeof version, m = sizeof build;
    if (sysctlbyname("kern.osproductversion", version, &n, NULL, 0) ||
        sysctlbyname("kern.osversion", build, &m, NULL, 0)) return 0;
    int len = snprintf(out, cap, "macOS %s (%s)", version, build);
    return len >= 0 && (size_t)len < cap;
}

int plat_stderr_path(char *out, size_t cap) {
    struct stat st;
    char path[PATH_MAX];
    if (fstat(STDERR_FILENO, &st) || !S_ISREG(st.st_mode) || fcntl(STDERR_FILENO, F_GETPATH, path)) return 0;
    return copy_string(out, cap, path);
}

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    Dl_info info;
    if (!dladdr((void *)addr, &info) || !info.dli_fname) return 0;
    const char *leaf = strrchr(info.dli_fname, '/');
    if (!copy_string(name, cap, leaf ? leaf + 1 : info.dli_fname)) return 0;
    *base = (uintptr_t)info.dli_fbase;
    return 1;
}

void *plat_lib_open(const char *name) {
    if (!name || !*name) return NULL;
    if (strchr(name, '/')) return dlopen(name, RTLD_NOW | RTLD_LOCAL);
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "/usr/lib/%s", name);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    // System dylibs in the shared cache may have no on-disk file.
    if (!_dyld_shared_cache_contains_path(path) && !plat_file_exists(path)) return NULL;
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void *plat_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }

static pid_t wait_child(pid_t pid, int *status, int flags) {
    pid_t result;
    do result = waitpid(pid, status, flags);
    while (result < 0 && errno == EINTR);
    return result;
}

static int collect_child(pid_t pid, int fd, char *out, size_t cap, uint32_t timeout_ms) {
    uint64_t deadline = plat_now_ns() + (uint64_t)timeout_ms * 1000000u;
    size_t used = 0;
    char buf[4096];
    int status, finished = 0, eof = 0;
    for (;;) {
        uint64_t now = plat_now_ns();
        if (!finished) {
            pid_t result = wait_child(pid, &status, WNOHANG);
            if (result < 0) return -1;
            finished = result == pid;
            if (!finished && timeout_ms && now >= deadline) {
                kill(pid, SIGKILL);
                if (wait_child(pid, &status, 0) < 0) return -1;
                finished = 1;
            }
        }

        // After exit, collect queued output without waiting for an inherited writer.
        size_t room = cap && used < cap - 1 ? cap - 1 - used : 0;
        if (finished && (eof || !room)) break;
        ssize_t n = eof ? 0 : read(fd, buf, sizeof buf);
        if (n > 0) {
            size_t keep = (size_t)n < room ? (size_t)n : room;
            if (keep) {
                memcpy(out + used, buf, keep);
                used += keep;
                out[used] = 0;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (finished) break;
        if (!n) eof = 1;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) break;

        // Check the actual child even when another process still holds the write end.
        struct pollfd p = {.fd = fd, .events = POLLIN};
        uint64_t ms = timeout_ms ? (deadline - now + 999999) / 1000000 : 20;
        if (poll(&p, eof ? 0 : 1, ms < 20 ? (int)ms : 20) < 0 && errno != EINTR) break;
    }
    if (!finished) {
        kill(pid, SIGKILL);
        wait_child(pid, &status, 0);
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int capture_pipe(int ends[2]) {
    int raw[2];
    if (pipe(raw)) return -1;
    // Keep the dup2 source distinct from stdout and stderr, even if either was closed.
    ends[0] = fcntl(raw[0], F_DUPFD_CLOEXEC, 3);
    ends[1] = fcntl(raw[1], F_DUPFD_CLOEXEC, 3);
    close(raw[0]);
    close(raw[1]);
    if (ends[0] >= 0 && ends[1] >= 0 && !fcntl(ends[0], F_SETFL, O_NONBLOCK)) return 0;
    if (ends[0] >= 0) close(ends[0]);
    if (ends[1] >= 0) close(ends[1]);
    return -1;
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    if (cap) out[0] = 0;
    char path[PATH_MAX];
    if (!*env_name || strchr(env_name, '=') || !plat_exe_path(path, sizeof path)) return -1;
    char **environ = *_NSGetEnviron();
    size_t count = 0, key = strlen(env_name);
    while (environ[count]) count++;
    char **env = calloc(count + 2, sizeof *env);
    char *value = NULL;
    if (!env || asprintf(&value, "%s=%s", env_name, env_value) < 0) {
        free(env);
        return -1;
    }
    size_t used = 0;
    for (size_t i = 0; i < count; i++)
        if (strncmp(environ[i], env_name, key) || environ[i][key] != '=') env[used++] = environ[i];
    env[used] = value;

    int ends[2], result = -1;
    if (!capture_pipe(ends)) {
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        pid_t child;
        if (!error) {
            error = posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
            if (!error) error = posix_spawn_file_actions_adddup2(&actions, ends[1], STDERR_FILENO);
            if (!error) error = posix_spawn(&child, path, &actions, NULL, *_NSGetArgv(), env);
            posix_spawn_file_actions_destroy(&actions);
        }
        close(ends[1]);
        if (!error) result = collect_child(child, ends[0], out, cap, timeout_ms);
        close(ends[0]);
    }
    free(value);
    free(env);
    return result;
}
