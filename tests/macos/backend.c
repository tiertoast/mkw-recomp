// Native runtime services exercised without the launcher or a game module.
#include "platform/platform.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void ipc(void) {
    char dir[] = "/tmp/mkw-backend.XXXXXX";
    CHECK(mkdtemp(dir) && !setenv("XDG_RUNTIME_DIR", dir, 1));
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s/test", dir);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(server >= 0 && !bind(server, (struct sockaddr *)&addr, sizeof addr) && !listen(server, 1));
    PlatIpc *client = plat_ipc_open("test");
    CHECK(client);
    int peer = accept(server, NULL, NULL);
    char buf[4];
    CHECK(peer >= 0 && plat_ipc_recv(client, buf, sizeof buf) == 0);
    CHECK(plat_ipc_send(client, "ping", 4) && read(peer, buf, 4) == 4 && !memcmp(buf, "ping", 4));
    CHECK(write(peer, "pong", 4) == 4 && plat_ipc_recv(client, buf, 4) == 4 && !memcmp(buf, "pong", 4));
    close(peer);
    CHECK(plat_ipc_recv(client, buf, 4) == -1 && !plat_ipc_send(client, "!", 1));
    plat_ipc_close(client);
    close(server);
    unlink(addr.sun_path);
    CHECK(!plat_ipc_open("test") && !rmdir(dir));
}

static void worker(void *arg) {
    plat_mutex_lock(arg);
    plat_mutex_unlock(arg);
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    CHECK(plat_thread_priority_get() == PLAT_PRIO_BELOW_NORMAL);
    plat_thread_priority(PLAT_PRIO_NORMAL);
    CHECK(plat_thread_priority_get() == PLAT_PRIO_NORMAL);
    CHECK(plat_threads_cpu_ns_named("darwin worker") >= 0);
    CHECK(!plat_thread_affinity(NULL, 1) && !plat_process_affinity(1));
}

static uint64_t page;
static void report(const PlatCrash *c) {
    _exit(c->addr == page && c->access == 1 && c->rip && c->offset && (c->code == SIGSEGV || c->code == SIGBUS) ? 42 : 2);
}
static void prior(int sig) { (void)sig; _exit(43); }
static void crash(int previous) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        if (previous) { signal(SIGBUS, prior); signal(SIGSEGV, prior); }
        plat_crash_hook(report);
        page = (uintptr_t)plat_mem_alloc(4096);
        CHECK(page && plat_mem_protect(page, 4096, PLAT_PROT_READ));
        *(volatile char *)(uintptr_t)page = 1;
        _exit(3);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status)) fprintf(stderr, "fault status %#x\n", status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == (previous ? 43 : 42));
}

int main(void) {
    ipc();
    crash(0);
    crash(1);
    uint64_t all, first, covered = 0;
    int cores = plat_cpu_cores(&all, &first);
    CHECK(all && cores == __builtin_popcountll(first));
    for (int i = 0; i < 64; i++) if (first & (UINT64_C(1) << i)) {
        uint64_t group = plat_cpu_core_of(i);
        CHECK((group & first) == (UINT64_C(1) << i) && !(covered & group));
        covered |= group;
    }
    CHECK(!cores || covered == all);
    printf("%d physical cores, %u logical, representatives %#llx\n", cores, plat_cpu_count(), (unsigned long long)first);
    PlatMutex gate = PLAT_MUTEX_INIT;
    CHECK(!setenv("MKW_LOCK_PROF", "1", 1));
    plat_mutex_lock(&gate);
    PlatThread *thread = plat_thread_new(worker, &gate, "darwin worker");
    CHECK(thread);
    // Wait until the Darwin lock has an announced writer before releasing it.
    while (!(__atomic_load_n(&gate.state, __ATOMIC_ACQUIRE) >> 32)) plat_yield();
    plat_mutex_unlock(&gate);
    plat_thread_join(thread);
    char waits[256];
    CHECK(plat_lock_report(0, waits, sizeof waits) > 0 && strstr(waits, "from exe+"));
    CHECK(!plat_lock_report(0, waits, sizeof waits));
    CHECK(plat_threads_cpu_ns_named("darwin worker") == -1);
    float total, proc, temp, used, ram, rss;
    PlatLoad *load = plat_load_open(NULL);
    plat_cpu_load(load, &total, &proc, &temp);
    CHECK(total == -1 && proc == -1);
    uint64_t age = plat_process_uptime_ns();
    plat_sleep_ms(120);
    CHECK(age && plat_process_uptime_ns() > age);
    plat_cpu_load(load, &total, &proc, &temp);
    CHECK(total >= 0 && total <= 100 && proc >= 0 && proc <= 100);
    plat_mem_info(&used, &ram, &rss);
    CHECK(used > 0 && used <= ram && rss > 0);
    plat_gpu_load(load, &total, &used);
    printf("GPU %.0f%%, VRAM used %.0f MiB, GA102 2204 capacity %llu MiB\n", total, used,
           (unsigned long long)plat_gpu_vram_mib(0x10de, 0x2204, NULL));
    plat_load_close(load);
    return 0;
}
