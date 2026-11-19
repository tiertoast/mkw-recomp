// Darwin files, append-only logs, and the host filesystem behind saves.
#include "platform.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static _Thread_local int fs_error;

static int fs_err(int error) {
    fs_error = error;
    switch (error) {
    case ENOENT: case ENOTDIR: return PLAT_FS_NOT_FOUND;
    case EEXIST: return PLAT_FS_EXISTS;
    case ENOTEMPTY: return PLAT_FS_NOT_EMPTY;
    case EBUSY: case ETXTBSY: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

int plat_fs_os_error(void) { return fs_error; }

// Accept both separators at the platform boundary. Keep the original pointer for native paths.
static const char *native_path(const char *path, char buf[PATH_MAX]) {
    if (!strchr(path, '\\')) return path;
    size_t n = strlen(path);
    if (n >= PATH_MAX) { errno = ENAMETOOLONG; return NULL; }
    for (size_t i = 0; i <= n; i++) buf[i] = path[i] == '\\' ? '/' : path[i];
    return buf;
}

// A trailing slash can make lstat follow the final symlink. Operate on the directory entry itself.
static const char *entry_path(const char *path, char buf[PATH_MAX]) {
    const char *p = native_path(path, buf);
    if (!p) return NULL;
    size_t n = strlen(p), end = n;
    while (end > 1 && p[end - 1] == '/') end--;
    if (end == n) return p;
    if (end >= PATH_MAX) { errno = ENAMETOOLONG; return NULL; }
    if (p != buf) memcpy(buf, p, end);
    buf[end] = 0;
    return buf;
}

static int path_stat(const char *path, struct stat *st) {
    char buf[PATH_MAX];
    const char *p = native_path(path, buf);
    return p ? stat(p, st) : -1;
}

static int open_path(const char *path, int flags) {
    char buf[PATH_MAX];
    const char *p = native_path(path, buf);
    return p ? open(p, flags | O_CLOEXEC, 0644) : -1;
}

int plat_file_exists(const char *path) {
    struct stat st;
    return !path_stat(path, &st) && S_ISREG(st.st_mode);
}

int plat_dir_exists(const char *path) {
    struct stat st;
    return !path_stat(path, &st) && S_ISDIR(st.st_mode);
}

int plat_full_path(const char *path, char *out, size_t cap) {
    char buf[PATH_MAX], cwd[PATH_MAX];
    const char *p = native_path(path, buf);
    if (!p) return 0;
    int n;
    if (*p == '/') n = snprintf(out, cap, "%s", p);
    else {
        if (!getcwd(cwd, sizeof cwd)) return 0;
        n = snprintf(out, cap, "%s%s%s", cwd, !strcmp(cwd, "/") ? "" : "/", p);
    }
    return n >= 0 && (size_t)n < cap;
}

void plat_file_mkdir_p(const char *path) {
    char buf[PATH_MAX], dir[PATH_MAX];
    const char *p = native_path(path, buf);
    if (!p || !*p || strlen(p) >= sizeof dir) return;
    strcpy(dir, p);
    for (char *slash = dir + 1; *slash; slash++) {
        if (*slash != '/') continue;
        *slash = 0;
        if (mkdir(dir, 0755) && errno != EEXIST) return;
        *slash = '/';
    }
    mkdir(dir, 0755);
}

static int regular_fd(int fd, struct stat *st) {
    if (fstat(fd, st)) return 0;
    if (S_ISREG(st->st_mode)) return 1;
    errno = S_ISDIR(st->st_mode) ? EISDIR : EINVAL;
    return 0;
}

// A descriptor plus one keeps fd 0 distinct from NULL.
static PlatFile *file_handle(int fd) { return fd < 0 ? NULL : (PlatFile *)((intptr_t)fd + 1); }
static int fd_of(PlatFile *f) { return (int)((intptr_t)f - 1); }

static int open_regular(const char *path, int write) {
    struct stat st;
    int fd = open_path(path, (write ? O_RDWR : O_RDONLY) | O_NONBLOCK);
    if (fd >= 0 && !regular_fd(fd, &st)) {
        int error = errno;
        close(fd);
        fd = -1;
        errno = error;
    }
    return fd;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    int fd = open_regular(path, write);
    if (fd < 0) {
        int result = fs_err(errno);
        if (err) *err = result;
    }
    return file_handle(fd);
}

PlatFile *plat_fs_create(const char *path, int *err) {
    int fd = open_path(path, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) {
        int result = fs_err(errno);
        if (err) *err = result;
    }
    return file_handle(fd);
}

PlatFile *plat_file_open(const char *path) { return file_handle(open_regular(path, 0)); }

PlatFile *plat_file_open_unbuffered(const char *path) {
    PlatFile *f = plat_file_open(path);
    if (!f || !fcntl(fd_of(f), F_NOCACHE, 1)) return f;
    plat_file_close(f);
    return NULL;
}

void plat_file_close(PlatFile *f) { if (f) close(fd_of(f)); }

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    if (off > INT64_MAX) { errno = EOVERFLOW; return -1; }
    ssize_t got;
    do got = pread(fd_of(f), buf, n, (off_t)off);
    while (got < 0 && errno == EINTR);
    return got < 0 && errno == EFAULT ? -2 : got;
}

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    if (off > INT64_MAX) { errno = EOVERFLOW; return -1; }
    ssize_t wrote;
    do wrote = pwrite(fd_of(f), buf, n, (off_t)off);
    while (wrote < 0 && errno == EINTR);
    return wrote;
}

int plat_file_set_size(PlatFile *f, uint64_t size) {
    if (size > INT64_MAX) return fs_err(EOVERFLOW);
    return ftruncate(fd_of(f), (off_t)size) ? fs_err(errno) : PLAT_FS_OK;
}

int64_t plat_file_size(const char *path) {
    struct stat st;
    return path_stat(path, &st) || !S_ISREG(st.st_mode) ? -1 : st.st_size;
}

static uint64_t time_ns(struct timespec t) { return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }

int plat_file_id(PlatFile *f, PlatFileId *out) {
    struct stat st;
    if (fstat(fd_of(f), &st)) return 0;
    *out = (PlatFileId){(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                        time_ns(st.st_mtimespec), time_ns(st.st_birthtimespec)};
    return 1;
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    PlatFile *f = plat_file_open(path);
    if (!f) return NULL;
    struct stat st;
    uint8_t *data = NULL;
    if (!fstat(fd_of(f), &st) && st.st_size > 0 && st.st_size <= (1ll << 30)) {
        uint32_t n = (uint32_t)st.st_size, got = 0;
        data = malloc((size_t)n + 1);
        while (data && got < n) {
            int64_t k = plat_file_pread(f, data + got, n - got, got);
            if (k <= 0) { free(data); data = NULL; break; }
            got += (uint32_t)k;
        }
        if (data) {
            data[n] = 0;
            *size = n;
        }
    }
    plat_file_close(f);
    return data;
}

static ssize_t write_once(int fd, const void *data, size_t n) {
    ssize_t wrote;
    do wrote = write(fd, data, n);
    while (wrote < 0 && errno == EINTR);
    return wrote;
}

static int write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        ssize_t wrote = write_once(fd, p, n > INT_MAX ? INT_MAX : n);
        if (wrote <= 0) return 0;
        p += wrote;
        n -= (size_t)wrote;
    }
    return 1;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    int fd = open_path(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n);
    return close(fd) == 0 && ok;
}

PlatAppend *plat_append_open(const char *path) {
    int fd = open_path(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND);
    return fd < 0 ? NULL : (PlatAppend *)((intptr_t)fd + 1);
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    // The crash logger can call this from a signal handler. Never split a record across writes.
    return f && n <= SSIZE_MAX && write_once((int)((intptr_t)f - 1), data, n) == (ssize_t)n;
}

int plat_file_lock(const char *path) {
    int fd = open_path(path, O_RDWR | O_CREAT | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (!regular_fd(fd, &st)) { close(fd); return -1; }
    int rc;
    do rc = flock(fd, LOCK_EX | LOCK_NB);
    while (rc < 0 && errno == EINTR);
    if (!rc) return 1;   // Released by the OS when the process exits.
    int error = errno;
    close(fd);
    return error == EWOULDBLOCK ? 0 : -1;
}

int plat_file_set_readonly(const char *path, int on) {
    char buf[PATH_MAX];
    const char *p = native_path(path, buf);
    struct stat st;
    if (!p || stat(p, &st)) return 0;
    mode_t mode = st.st_mode & 07777;
    return chmod(p, on ? mode & ~(mode_t)0222 : mode | S_IWUSR) == 0;
}

static int sync_fd(int fd) {
    int rc;
    do rc = fcntl(fd, F_FULLFSYNC);
    while (rc < 0 && errno == EINTR);
    if (rc == 0 || (errno != ENOTSUP && errno != EOPNOTSUPP && errno != ENOTTY && errno != EINVAL)) return rc == 0;
    // Some filesystems provide only fsync. Preserve actual I/O errors from either operation.
    do rc = fsync(fd);
    while (rc < 0 && errno == EINTR);
    return rc == 0;
}

int plat_file_fsync(PlatFile *f) { return sync_fd(fd_of(f)); }
int plat_file_sync(FILE *f) { return !fflush(f) && sync_fd(fileno(f)); }

// Keep both parent directories open across a durable rename. The leaf retains any trailing slash.
static int open_parent(const char *path, const char **name) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    while (n && path[n - 1] != '/') n--;
    *name = path + n;
    if (!n) return open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    char parent[PATH_MAX];
    memcpy(parent, path, n);
    parent[n] = 0;
    return open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int rename_entry(const char *from, const char *to, int exclusive, int durable) {
    char a[PATH_MAX], b[PATH_MAX];
    from = native_path(from, a);
    if (!from) return fs_err(errno);
    to = native_path(to, b);
    if (!to) return fs_err(errno);

    if (!durable) {
        int rc = exclusive ? renamex_np(from, to, RENAME_EXCL) : rename(from, to);
        return rc ? fs_err(errno) : PLAT_FS_OK;
    }
    int src = open_parent(from, &from);
    if (src < 0) return fs_err(errno);
    int dst = open_parent(to, &to);
    int error = dst < 0 ? errno : 0;
    if (dst >= 0) {
        int rc = exclusive ? renameatx_np(src, from, dst, to, RENAME_EXCL) : renameat(src, from, dst, to);
        if (rc || !sync_fd(src)) error = errno;
        if (!rc && !sync_fd(dst) && !error) error = errno;
        close(dst);
    }
    close(src);
    return error ? fs_err(error) : PLAT_FS_OK;
}

int plat_file_replace(const char *from, const char *to) { return rename_entry(from, to, 0, 1) == PLAT_FS_OK; }

int plat_fs_rename(const char *from, const char *to, int durable) { return rename_entry(from, to, 1, durable); }

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char buf[PATH_MAX];
    path = native_path(path, buf);
    if (!path) return 0;
    const char *name;
    int dir = open_parent(path, &name);
    if (dir < 0) return 0;

    int ok = 0;
    struct stat st;
    mode_t mode = 0644;
    if (!*name || strchr(name, '/')) goto done;
    if (!fstatat(dir, name, &st, 0)) {
        if (!S_ISREG(st.st_mode) || !(st.st_mode & 0222) || faccessat(dir, name, W_OK, AT_EACCESS)) goto done;
        mode = st.st_mode & 0777;
    } else if (errno != ENOENT) goto done;

    char tmp[NAME_MAX + 1];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", name, (unsigned)getpid(), plat_thread_id());
    if (len < 0 || (size_t)len >= sizeof tmp) goto done;
    // An existing temp may belong to an interrupted write. Never truncate it or follow it.
    int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) goto done;
    int staged = write_all(fd, data, n) && sync_fd(fd);
    if (close(fd)) staged = 0;
    if (staged && !renameat(dir, tmp, dir, name)) ok = sync_fd(dir);
    else unlinkat(dir, tmp, 0);
done:
    close(dir);
    return ok;
}

static int fs_kind(mode_t mode) {
    return S_ISLNK(mode) ? PLAT_FS_LINK : S_ISDIR(mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_kind(const char *path) {
    struct stat st;
    char buf[PATH_MAX];
    const char *p = entry_path(path, buf);
    if (p && !lstat(p, &st)) return fs_kind(st.st_mode);
    fs_error = errno;
    return PLAT_FS_MISSING;
}

int plat_fs_mkdir(const char *path) {
    char buf[PATH_MAX];
    const char *p = native_path(path, buf);
    return p && !mkdir(p, 0755) ? PLAT_FS_OK : fs_err(errno);
}

static int remove_entry(const char *path, int dir) {
    char buf[PATH_MAX];
    const char *p = dir ? entry_path(path, buf) : native_path(path, buf);
    if (!p) return -1;
    if (dir) {
        struct stat st;
        if (lstat(p, &st)) return -1;
        dir = !S_ISLNK(st.st_mode);
    }
    return dir ? rmdir(p) : unlink(p);
}

int plat_fs_rmdir(const char *path) { return remove_entry(path, 1) ? fs_err(errno) : PLAT_FS_OK; }
int plat_fs_unlink(const char *path) { return remove_entry(path, 0) ? fs_err(errno) : PLAT_FS_OK; }
void plat_file_delete(const char *path) { (void)remove_entry(path, 0); }

int plat_fs_link(const char *existing, const char *path) {
    char a[PATH_MAX], b[PATH_MAX];
    const char *from = native_path(existing, a);
    if (!from) return fs_err(errno);
    const char *to = native_path(path, b);
    return to && !link(from, to) ? PLAT_FS_OK : fs_err(errno);
}

int plat_fs_links(const char *path) {
    struct stat st;
    if (!path_stat(path, &st)) return (int)st.st_nlink;
    fs_error = errno;
    return 0;
}

void plat_fs_sync_dir(const char *path) {
    int fd = open_path(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { fs_error = errno; return; }
    if (!sync_fd(fd)) fs_error = errno;
    close(fd);
}

void plat_fs_busy_reason(const char *dir, char *out, size_t cap) {
    // The platform contract uses an empty diagnostic on POSIX: open files do not obstruct rename.
    (void)dir;
    if (cap) *out = 0;
}

int plat_fs_list(const char *dir, int (*cb)(const char *, int, uint64_t, void *), void *ctx) {
    char buf[PATH_MAX];
    const char *p = native_path(dir, buf);
    DIR *d = p ? opendir(p) : NULL;
    if (!d) return fs_err(errno);
    int result = PLAT_FS_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(d);
        if (!entry) {
            if (errno) result = fs_err(errno);
            break;
        }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd(d), name, &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT) continue;   // Concurrently removed.
            result = fs_err(errno);
            break;
        }
        int kind = fs_kind(st.st_mode);
        if (!cb(name, kind, kind == PLAT_FS_FILE ? (uint64_t)st.st_size : 0, ctx)) break;
    }
    closedir(d);
    return result;
}

typedef struct {
    const char *ext;
    size_t len;
    int (*cb)(const char *, void *);
    void *ctx;
} FileList;

static int list_file(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    FileList *list = ctx;
    size_t n = strlen(name);
    if (kind == PLAT_FS_DIR || *name == '.' || n < list->len || strcmp(name + n - list->len, list->ext)) return 1;
    return list->cb(name, list->ctx);
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *, void *), void *ctx) {
    FileList list = {ext, strlen(ext), cb, ctx};
    return plat_fs_list(dir, list_file, &list) == PLAT_FS_OK;
}

int plat_temp_dir(char *out, size_t cap) {
    const char *path = getenv("TMPDIR");
    char system[PATH_MAX];
    if (!path || !*path) {
        size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, system, sizeof system);
        path = n && n <= sizeof system ? system : "/tmp";
    }
    char absolute[PATH_MAX];
    if (!plat_full_path(path, absolute, sizeof absolute)) return 0;
    size_t n = strlen(absolute);
    while (n > 1 && absolute[n - 1] == '/') n--;
    if (n >= cap) return 0;
    memcpy(out, absolute, n);
    out[n] = 0;
    return 1;
}
