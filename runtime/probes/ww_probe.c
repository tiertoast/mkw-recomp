// --probe-writewatch: the write watch of platform.h (plat_mem_written, plat_mem_will_write) on a reservation of its
// own, as the back end uses it: a commit is not a write; a write is reported until a reset and not after; max truncates
// and the rest is reported next; a decommitted page is not reported; a kernel write (a file read) into a watched page,
// announced with plat_mem_will_write, lands and is reported; and no write is lost while four threads write during
// resets: a copy of every reported page, taken after each query, equals memory at the end. Then the cost of a write
// fault. Darwin's watch takes a fault per page after each reset (darwin_mem.c), Linux's and Windows' are the kernel's.
// Needs no game files; skips where the platform has no write watch.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL writewatch: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define PG 4096ull
#define RACE_PAGES 48
#define COST_PAGES 4096
#define TOTAL_PAGES (128 + COST_PAGES)   // pages 0-63 checks, 64-111 the threads, 128- the cost

static uint64_t base;
static uint8_t *page(uint64_t i) { return (uint8_t *)(uintptr_t)(base + i * PG); }

// The pages reported in [lo, hi) as page indices, in order, into idx (at most max).
static uint64_t written(uint64_t lo, uint64_t hi, uint64_t *idx, uint64_t max, int reset) {
    uint64_t addrs[RACE_PAGES + 64], n = plat_mem_written(base + lo * PG, (hi - lo) * PG, addrs,
                                                         max < RACE_PAGES + 64 ? max : RACE_PAGES + 64, reset);
    if (n == UINT64_MAX) return n;
    for (uint64_t k = 0; k < n; k++) idx[k] = (addrs[k] - base) / PG;
    return n;
}

static int same(const uint64_t *got, uint64_t n, const uint64_t *want, uint64_t m) {
    if (n != m) return 0;
    for (uint64_t k = 0; k < n; k++) if (got[k] != want[k]) return 0;
    return 1;
}

static uint64_t faults(void) {
    uint64_t s[7];
    return plat_mem_watch_stats(s) ? s[0] : 0;
}

// Four writers over the race pages until stop: 8 bytes (thread, counter) at a pseudo-random page and slot.
typedef struct { int id; volatile int *stop; uint64_t writes; } Writer;
static void writer(void *arg) {
    Writer *w = arg;
    uint64_t x = 0x9e3779b97f4a7c15ull * (uint64_t)(w->id + 1);
    while (!__atomic_load_n(w->stop, __ATOMIC_ACQUIRE)) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        uint64_t v = ((uint64_t)w->id << 56) | ++w->writes;
        memcpy(page(64 + x % RACE_PAGES) + (x >> 32) % (PG / 8) * 8, &v, 8);
    }
}

// Queries with reset over pages 32-63 until stop (test 5b).
static void resetter(void *arg) {
    volatile int *stop = arg;
    uint64_t got[64];
    while (!__atomic_load_n(stop, __ATOMIC_ACQUIRE)) written(32, 64, got, 64, 1);
}

int probe_writewatch(void) {
    for (uint64_t a = 0x7f00000000ull; a >= 0x7c00000000ull && !base; a -= 0x40000000ull)
        if (plat_mem_reserve(a, TOTAL_PAGES * PG, 1)) base = a;
    if (!base) { fprintf(stderr, "FAIL writewatch: no free address for the test reservation\n"); return 1; }
    if (!plat_mem_commit(base, 128 * PG)) { fprintf(stderr, "FAIL writewatch: commit\n"); return 1; }
    uint64_t got[RACE_PAGES + 64], n = written(0, 64, got, 64, 0);
    if (n == UINT64_MAX) { printf("writewatch: no write watch on this platform (skipped)\n"); return 0; }

    // 1. A commit is not a write.
    CHECK(n == 0, "%llu pages reported written right after the commit", (unsigned long long)n);

    // 2. Reported until a reset, and not after; one fault per page per reset.
    uint64_t f0 = faults();
    page(3)[5] = 1, page(7)[9] = 2, page(7)[10] = 3;
    const uint64_t w37[] = {3, 7};
    n = written(0, 64, got, 64, 0);
    CHECK(same(got, n, w37, 2), "after writing pages 3 and 7, %llu pages reported (no reset)", (unsigned long long)n);
    n = written(0, 64, got, 64, 1);
    CHECK(same(got, n, w37, 2), "the reset query reported %llu pages, not pages 3 and 7", (unsigned long long)n);
    n = written(0, 64, got, 64, 0);
    CHECK(n == 0, "%llu pages still reported after the reset", (unsigned long long)n);
    uint64_t f1 = faults();
    page(3)[6] = 4, page(3)[7] = 5;
    uint64_t f2 = faults();
    if (f0 || f1 || f2) {   // a platform whose watch counts its own faults (Darwin)
        CHECK(f1 - f0 == 2, "%llu faults for the first writes of two pages", (unsigned long long)(f1 - f0));
        CHECK(f2 - f1 == 1, "%llu faults for two writes to one page after a reset", (unsigned long long)(f2 - f1));
    }
    CHECK(page(3)[5] == 1 && page(3)[7] == 5 && page(7)[10] == 3, "written bytes did not stay");
    written(0, 64, got, 64, 1);

    // 2b. A page rewritten right after each report (Darwin: a quick fault, < 40 ms after it was protected again) turns
    // hot: it stays writable and every report lists it, written or not, until it is protected again after a while.
    if (f0 || f1 || f2) {
        uint64_t h[64];
        const uint64_t w120[] = {120};
        for (int k = 0; k < 4; k++) page(120)[k] = (uint8_t)k, written(112, 128, h, 64, 1);
        uint64_t fh = faults();
        page(120)[9] = 9;
        CHECK(faults() == fh, "a write to a hot page faulted");
        n = written(112, 128, h, 64, 1);
        CHECK(same(h, n, w120, 1), "a hot page reported %llu pages after a write", (unsigned long long)n);
        n = written(112, 128, h, 64, 1);
        CHECK(same(h, n, w120, 1), "a hot page reported %llu pages without a write", (unsigned long long)n);
        for (int k = 0; k < 300; k++) written(112, 128, h, 64, 1);   // past the reports after which it is tried again
        fh = faults();
        page(120)[10] = 10;
        CHECK(faults() == fh + 1, "a hot page tried again took %llu faults on a write, not 1",
              (unsigned long long)(faults() - fh));
        written(112, 128, h, 64, 1);
    }

    // 3. max truncates; only what was reported is reset.
    for (uint64_t i = 10; i < 15; i++) page(i)[0] = (uint8_t)i;
    const uint64_t first2[] = {10, 11}, rest3[] = {12, 13, 14};
    n = written(0, 64, got, 2, 1);
    CHECK(same(got, n, first2, 2), "max 2 reported %llu pages, not pages 10 and 11", (unsigned long long)n);
    n = written(0, 64, got, 64, 1);
    CHECK(same(got, n, rest3, 3), "the next query reported %llu pages, not pages 12-14", (unsigned long long)n);

    // 4. A decommitted page is not reported, and reads zero once committed again.
    page(20)[0] = 0xaa;
    CHECK(plat_mem_decommit(base + 20 * PG, PG) && plat_mem_commit(base + 20 * PG, PG), "decommit and commit page 20");
    n = written(0, 64, got, 64, 1);
    CHECK(n == 0, "%llu pages reported after a decommit (page %llu first)", (unsigned long long)n,
          (unsigned long long)(n ? got[0] : 0));
    CHECK(page(20)[0] == 0, "page 20 kept a byte across decommit and commit");

    // 5. A kernel write into a watched page: announced, it lands and is reported.
    char dir[1024], path[1100];
    if (plat_temp_dir(dir, sizeof dir)) {
        snprintf(path, sizeof path, "%s/mkw-writewatch-probe.bin", dir);
        static uint8_t data[PG];
        memset(data, 0x5a, sizeof data);
        PlatFile *f = plat_file_write(path, data, sizeof data) ? plat_file_open(path) : NULL;
        CHECK(f != NULL, "temporary file %s", path);
        if (f) {
#ifdef __APPLE__
            // The premise of plat_mem_will_write: unannounced, the kernel refuses a protected page (EFAULT).
            int64_t r = plat_file_pread(f, page(30), PG, 0);
            CHECK(r == -2, "an unannounced read into a protected page returned %lld, not EFAULT (-2)", (long long)r);
#endif
            CHECK(plat_mem_will_write(base + 30 * PG, PG), "plat_mem_will_write");
            int64_t got_bytes = plat_file_pread(f, page(30), PG, 0);
            plat_mem_wrote(base + 30 * PG, PG);
            CHECK(got_bytes == (int64_t)PG && page(30)[0] == 0x5a && page(30)[PG - 1] == 0x5a,
                  "an announced read returned %lld bytes", (long long)got_bytes);
            const uint64_t w30[] = {30};
            n = written(0, 64, got, 64, 1);
            CHECK(same(got, n, w30, 1), "after the file read, %llu pages reported, not page 30", (unsigned long long)n);
            n = written(0, 64, got, 64, 1);
            CHECK(n == 0, "%llu pages reported again after the read's report", (unsigned long long)n);
            // 5a. While a kernel write is in flight a reset still clears a reported page (a caller that queries again
            // while the answer is full, vk_mem_gen.c, must come to an end), but leaves it writable; plat_mem_wrote
            // makes it reported again, with what was written in between.
            const uint64_t w31[] = {31};
            plat_mem_will_write(base + 31 * PG, PG);
            n = written(0, 64, got, 64, 1);
            CHECK(same(got, n, w31, 1), "announced page 31 reported %llu pages", (unsigned long long)n);
            n = written(0, 64, got, 64, 1);
            CHECK(n == 0, "a page in flight reported %llu times more after a reset", (unsigned long long)n);
            uint64_t fb = faults();
            page(31)[1] = 0x77;   // in flight: writable, no fault
            CHECK(faults() == fb, "a write to a page in flight faulted");
            plat_mem_wrote(base + 31 * PG, PG);
            n = written(0, 64, got, 64, 1);
            CHECK(same(got, n, w31, 1), "after plat_mem_wrote, %llu pages reported, not page 31", (unsigned long long)n);
            n = written(0, 64, got, 64, 1);
            CHECK(n == 0, "page 31 still reported after its last report (%llu)", (unsigned long long)n);
            // 5b. Reports with reset on another thread while reads are announced and run: no read fails (the first
            // races: a RomFS read refused between the announcement and the read).
            volatile int stop5 = 0;
            PlatThread *rs = plat_thread_new(resetter, (void *)&stop5, "ww resetter");
            int refused = 0;
            for (int k = 0; k < 2000; k++) {
                uint64_t at = base + (40 + k % 8) * PG;
                plat_mem_will_write(at, PG);
                refused += plat_file_pread(f, page(40 + k % 8), PG, 0) != (int64_t)PG;
                plat_mem_wrote(at, PG);
            }
            __atomic_store_n(&stop5, 1, __ATOMIC_RELEASE);
            plat_thread_join(rs);
            CHECK(refused == 0, "%d of 2000 announced reads failed while another thread reset", refused);
            plat_file_close(f);
        }
        remove(path);
    }

    // 6. No write lost: four writers during resets; the copy of every reported page equals memory at the end.
    static uint8_t copy[RACE_PAGES * PG];
    written(64, 64 + RACE_PAGES, got, RACE_PAGES, 1);
    memcpy(copy, page(64), sizeof copy);
    volatile int stop = 0;
    Writer w[4];
    PlatThread *t[4];
    for (int k = 0; k < 4; k++) w[k] = (Writer){k, &stop, 0}, t[k] = plat_thread_new(writer, &w[k], "ww writer");
    uint64_t rounds = 0, reported = 0, start = plat_now_ns();
    while (plat_now_ns() - start < 300000000ull) {
        n = written(64, 64 + RACE_PAGES, got, RACE_PAGES, 1);
        for (uint64_t k = 0; k < n; k++) memcpy(copy + (got[k] - 64) * PG, page(got[k]), PG);
        rounds++, reported += n;
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    uint64_t writes = 0;
    for (int k = 0; k < 4; k++) plat_thread_join(t[k]), writes += w[k].writes;
    n = written(64, 64 + RACE_PAGES, got, RACE_PAGES, 1);
    for (uint64_t k = 0; k < n; k++) memcpy(copy + (got[k] - 64) * PG, page(got[k]), PG);
    uint64_t lost = 0;
    for (uint64_t i = 0; i < RACE_PAGES; i++) lost += memcmp(copy + i * PG, page(64 + i), PG) != 0;
    CHECK(lost == 0, "%llu of %d pages differ from their copy: a write was not reported", (unsigned long long)lost,
          RACE_PAGES);
    printf("writewatch: race: %llu writes by 4 threads, %llu queries with reset, %llu pages reported\n",
           (unsigned long long)writes, (unsigned long long)rounds, (unsigned long long)reported);

    // 7. The cost of a write fault: the first write to each of COST_PAGES committed pages, then the same writes again.
    if (plat_mem_commit(base + 128 * PG, COST_PAGES * PG)) {
        uint64_t s0[7] = {0}, s1[7] = {0};
        int stats = plat_mem_watch_stats(s0);
        uint64_t a = plat_now_ns();
        for (uint64_t i = 0; i < COST_PAGES; i++) page(128 + i)[0] = 1;
        uint64_t b = plat_now_ns();
        for (uint64_t i = 0; i < COST_PAGES; i++) page(128 + i)[1] = 1;
        uint64_t c = plat_now_ns();
        if (stats) plat_mem_watch_stats(s1);
        printf("writewatch: first write to %d pages %.2f us each, again %.3f us each", COST_PAGES,
               (b - a) / 1e3 / COST_PAGES, (c - b) / 1e3 / COST_PAGES);
        if (stats)
            printf("; %llu faults, %.2f us each inside the handler", (unsigned long long)(s1[0] - s0[0]),
                   s1[0] > s0[0] ? (s1[1] - s0[1]) / 1e3 / (double)(s1[0] - s0[0]) : 0.0);
        printf("\n");
        uint64_t all[RACE_PAGES + 64], left = COST_PAGES, total = 0;
        while (left) {   // reported in batches of the buffer; each batch is reset, so the loop ends
            uint64_t k = plat_mem_written(base + 128 * PG, COST_PAGES * PG, all, RACE_PAGES + 64, 1);
            if (!k || k == UINT64_MAX) break;
            total += k, left = left > k ? left - k : 0;
        }
        CHECK(total == COST_PAGES, "%llu of %d written pages reported", (unsigned long long)total, COST_PAGES);
    } else CHECK(0, "commit of the cost pages");

    printf("writewatch: %s (%d failures)\n", fails ? "FAILED" : "passed", fails);
    return fails ? 1 : 0;
}
