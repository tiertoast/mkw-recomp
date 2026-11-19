// Run the Darwin file/log and save transactions on a private temporary directory.
#include "platform/platform.h"
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "Darwin files:%d: %s (errno %d, fs error %d)\n", __LINE__, #cond, errno, plat_fs_os_error()); \
    exit(1); \
} } while (0)

static void content(const char *path, const char *expected) {
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    CHECK(data && size == strlen(expected) && !memcmp(data, expected, size + 1));
    free(data);
}

static int count_file(const char *name, void *ctx) {
    CHECK(!strcmp(name, "file.dat"));
    (*(int *)ctx)++;
    return 0;
}

static int check_link(const char *name, int kind, uint64_t size, void *ctx) {
    if (strcmp(name, "link")) return 1;
    CHECK(kind == PLAT_FS_LINK && size == 0);
    (*(int *)ctx)++;
    return 0;
}

enum { WRITERS = 4, RECORDS = 16 };
typedef struct { unsigned writer, idx; char tag[8]; } Record;
typedef struct { PlatAppend *log; unsigned writer; } Writer;

static void append_records(void *arg) {
    const Writer *w = arg;
    for (unsigned i = 0; i < RECORDS; i++) {
        Record r = {w->writer, i, "record"};
        CHECK(plat_append_write(w->log, &r, sizeof r));
    }
}

static void file_and_log(void) {
    char path[PATH_MAX], reason[8] = "busy";
    CHECK(plat_full_path("tree\\sub", path, sizeof path) && path[0] == '/' && strstr(path, "/tree/sub"));
    CHECK(!plat_full_path("tree/sub", path, 2));
    CHECK(!setenv("TMPDIR", "tree/sub///", 1));
    CHECK(plat_temp_dir(path, sizeof path));
    CHECK(plat_temp_dir(path, strlen(path) + 1) && !strcmp(path + strlen(path) - 9, "/tree/sub"));
    plat_file_mkdir_p("tree\\sub");
    CHECK(plat_dir_exists("tree/sub") && !plat_file_exists("tree/sub"));
    CHECK(!plat_fs_mkdir("tree/sub/dir.dat") && plat_fs_mkdir("tree/sub") == PLAT_FS_EXISTS);
    CHECK(plat_fs_os_error() == EEXIST);
    CHECK(plat_file_write("tree\\sub\\file.dat", "abc", 3));
    CHECK(plat_file_exists("tree/sub/file.dat") && plat_file_size("tree/sub/file.dat") == 3);
    content("tree/sub/file.dat", "abc");
    int err = -1;
    CHECK(!plat_fs_create("tree/sub/file.dat", &err) && err == PLAT_FS_EXISTS);
    PlatFile *f = plat_fs_create("tree/sub/position", &err);
    CHECK(f && plat_file_pwrite(f, "Z", 1, 2) == 1 && plat_file_fsync(f));
    CHECK(plat_file_pwrite(f, "Z", 1, UINT64_MAX) == -1);
    CHECK(plat_file_set_size(f, UINT64_MAX) == PLAT_FS_ERROR && plat_fs_os_error() == EOVERFLOW);
    CHECK(!plat_file_set_size(f, 3));
    plat_file_close(f);
    f = plat_fs_open("tree/sub/position", 1, &err);
    CHECK(f && plat_file_pwrite(f, "xy", 2, 0) == 2);
    char data[4] = {0};
    CHECK(plat_file_pread(f, data, 4, 0) == 3 && !strcmp(data, "xyZ"));
    CHECK(plat_file_pread(f, data, 1, 3) == 0 && plat_file_pread(f, data, 1, UINT64_MAX) == -1);
    size_t page = (size_t)getpagesize();
    void *ro = mmap(NULL, page, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(ro != MAP_FAILED && plat_file_pread(f, ro, 1, 0) == -2 && !munmap(ro, page));
    PlatFileId id, hard;
    CHECK(plat_file_id(f, &id) && id.size == 3 && id.write_time && id.create_time);
    plat_file_close(f);
    f = plat_file_open_unbuffered("tree/sub/position");
    CHECK(f && !posix_memalign(&ro, page, page));
    CHECK(plat_file_pread(f, ro, (uint32_t)page, 0) == 3 && !memcmp(ro, "xyZ", 3));
    free(ro);
    plat_file_close(f);

    CHECK(!plat_fs_link("tree/sub/position", "tree/sub/hard") && plat_fs_links("tree/sub/hard") == 2);
    f = plat_file_open("tree/sub/hard");
    CHECK(f && plat_file_id(f, &hard) && hard.file_id == id.file_id && hard.volume == id.volume);
    plat_file_close(f);
    CHECK(!symlink("sub", "tree/link") && plat_fs_kind("tree/link") == PLAT_FS_LINK);
    int count = 0;
    CHECK(!plat_fs_list("tree", check_link, &count) && count == 1);
    CHECK(plat_fs_kind("tree/link/") == PLAT_FS_LINK);
    CHECK(!plat_fs_rmdir("tree/link/") && plat_dir_exists("tree/sub"));
    CHECK(plat_fs_rmdir("tree/sub") == PLAT_FS_NOT_EMPTY && plat_fs_os_error() == ENOTEMPTY);
    CHECK(plat_file_write("tree/sub/.hidden.dat", "x", 1));
    count = 0;
    CHECK(plat_file_list("tree/sub", ".dat", count_file, &count) && count == 1);
    CHECK(plat_fs_list("absent", check_link, &count) == PLAT_FS_NOT_FOUND);
    errno = 0;
    CHECK(plat_fs_os_error() == ENOENT);
    plat_fs_busy_reason("tree", reason, sizeof reason);
    CHECK(!*reason);
    CHECK(!mkfifo("fifo", 0600) && !plat_file_open("fifo"));
    CHECK(!plat_fs_unlink("fifo"));
    CHECK(plat_fs_kind("fifo") == PLAT_FS_MISSING);
    FILE *stream = fopen("stream", "w");
    CHECK(stream && fputs("flush", stream) >= 0 && plat_file_sync(stream) && !fclose(stream));
    content("stream", "flush");
    plat_fs_sync_dir("tree");

    CHECK(plat_file_write("log", "discard", 7));
    PlatAppend *log = plat_append_open("log");
    CHECK(log && plat_file_size("log") == 0);
    Writer jobs[WRITERS];
    PlatThread *threads[WRITERS];
    for (unsigned i = 0; i < WRITERS; i++) {
        jobs[i] = (Writer){log, i};
        threads[i] = plat_thread_new(append_records, &jobs[i], "file fixture");
        CHECK(threads[i]);
    }
    for (unsigned i = 0; i < WRITERS; i++) plat_thread_join(threads[i]);
    f = plat_file_open("log");
    int seen[WRITERS][RECORDS] = {{0}};
    CHECK(f && plat_file_size("log") == WRITERS * RECORDS * sizeof(Record));
    for (unsigned i = 0; i < WRITERS * RECORDS; i++) {
        Record r;
        CHECK(plat_file_pread(f, &r, sizeof r, i * sizeof r) == sizeof r);
        CHECK(r.writer < WRITERS && r.idx < RECORDS && !strcmp(r.tag, "record") && !seen[r.writer][r.idx]++);
    }
    plat_file_close(f);
}

static void no_temp_after_failure(void) {
    struct sigaction sa = {.sa_handler = SIG_IGN}, old;
    sigemptyset(&sa.sa_mask);
    CHECK(!sigaction(SIGXFSZ, &sa, &old));
    struct rlimit saved, limited;
    CHECK(!getrlimit(RLIMIT_FSIZE, &saved));
    limited = saved;
    limited.rlim_cur = 4;
    CHECK(!setrlimit(RLIMIT_FSIZE, &limited));
    CHECK(!plat_file_write_atomic("save", "too long", 8));
    CHECK(!setrlimit(RLIMIT_FSIZE, &saved) && !sigaction(SIGXFSZ, &old, NULL));
}

static void lock_test(void) {
    int ready[2], release[2];
    CHECK(!pipe(ready) && !pipe(release));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(ready[0]);
        close(release[1]);
        char c = 'L';
        _exit(plat_file_lock("save.lock") == 1 && write(ready[1], &c, 1) == 1 && read(release[0], &c, 1) == 1 ? 0 : 2);
    }
    close(ready[1]);
    close(release[0]);
    char c;
    CHECK(read(ready[0], &c, 1) == 1 && c == 'L' && plat_file_lock("save.lock") == 0);
    CHECK(write(release[1], &c, 1) == 1);
    close(ready[0]);
    close(release[1]);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(plat_file_lock("save.lock") == 1 && plat_file_lock("absent/no.lock") == -1);
}

static void atomic_and_lock(void) {
    CHECK(plat_file_write_atomic("save", "old", 3));
    CHECK(!chmod("save", 0600));
    PlatFile *before = plat_file_open("save");
    CHECK(before && plat_file_write_atomic("save", "fresh", 5));
    struct stat st;
    CHECK(!stat("save", &st) && (st.st_mode & 0777) == 0600);
    char data[4] = {0};
    CHECK(plat_file_pread(before, data, 3, 0) == 3 && !strcmp(data, "old"));
    plat_file_close(before);
    content("save", "fresh");
    CHECK(plat_file_set_readonly("save", 1) && !plat_file_write_atomic("save", "bad", 3));
    CHECK(plat_file_set_readonly("save", 0));
    char tmp[128];
    snprintf(tmp, sizeof tmp, "save.%u.%u.tmp", (unsigned)getpid(), plat_thread_id());
    CHECK(plat_file_write("keep", "unchanged", 9) && !symlink("keep", tmp));
    CHECK(!plat_file_write_atomic("save", "bad", 3) && plat_fs_kind(tmp) == PLAT_FS_LINK);
    content("keep", "unchanged");
    plat_file_delete(tmp);
    no_temp_after_failure();
    CHECK(plat_fs_kind(tmp) == PLAT_FS_MISSING);
    content("save", "fresh");

    CHECK(!plat_fs_mkdir("archive"));
    CHECK(plat_file_write_atomic("archive/save", "first", 5));
    CHECK(plat_fs_rename("save", "archive\\save", 1) == PLAT_FS_EXISTS && plat_fs_os_error() == EEXIST);
    content("save", "fresh");
    content("archive/save", "first");
    CHECK(plat_file_replace("save", "archive/save") && !plat_file_exists("save"));
    content("archive/save", "fresh");
    CHECK(!plat_fs_rename("archive/save", "restored", 1) && !plat_file_exists("archive/save"));
    CHECK(!plat_fs_rename("archive", "renamed", 0));
    content("restored", "fresh");
    lock_test();
}

static int clear(const char *path, const struct stat *st, int kind, struct FTW *level) {
    (void)st; (void)kind; (void)level;
    return remove(path);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    char parent[PATH_MAX], dir[PATH_MAX];
    CHECK(plat_temp_dir(parent, sizeof parent));
    int n = snprintf(dir, sizeof dir, "%s/mkw-fs.XXXXXX", parent);
    CHECK(n > 0 && (size_t)n < sizeof dir && mkdtemp(dir) && !chdir(dir));
    if (!strcmp(argv[1], "files")) file_and_log();
    else {
        CHECK(!strcmp(argv[1], "saves"));
        atomic_and_lock();
    }
    CHECK(!chdir("/") && !nftw(dir, clear, 20, FTW_DEPTH | FTW_PHYS));
    return 0;
}
