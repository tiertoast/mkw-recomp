// A pthread owns its base context and TLS. Only the stack and the SysV call-preserved state follow a fiber.
#include "platform.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

struct PlatFiber {
    void *sp;
    _Alignas(16) unsigned char fp[512];   // FXSAVE64 image
    void *mapping;
    size_t size;
};
_Static_assert(offsetof(PlatFiber, fp) == 16, "darwin_switch.S");
extern void darwin_fiber_swap(PlatFiber *from, PlatFiber *to);
extern void darwin_fiber_entry(void);

static _Thread_local PlatFiber base;
static _Thread_local PlatFiber *current;

PlatFiber *plat_fiber_self(void) {
    current = &base;
    return current;
}

void plat_fiber_unself(void) {
    current = NULL;
}

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *), void *arg) {
    size_t page = (size_t)getpagesize();
    if (!fn || !stack || stack > SIZE_MAX - 2 * page) return NULL;
    size_t usable = (stack + page - 1) & ~(page - 1);
    PlatFiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->size = page + usable;
    f->mapping = mmap(NULL, f->size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (f->mapping == MAP_FAILED || mprotect((char *)f->mapping + page, usable, PROT_READ | PROT_WRITE)) {
        if (f->mapping != MAP_FAILED) munmap(f->mapping, f->size);
        free(f);
        return NULL;
    }
    // Initial x87 control, empty tag word and MXCSR. Restored in exactly the format the CPU subsequently saves.
    uint16_t cw = 0x037f;
    uint32_t mxcsr = 0x1f80;
    __builtin_memcpy(f->fp, &cw, sizeof cw);
    __builtin_memcpy(f->fp + 24, &mxcsr, sizeof mxcsr);
    uintptr_t *sp = (uintptr_t *)((char *)f->mapping + f->size) - 7;
    sp[2] = (uintptr_t)arg;  // r13
    sp[3] = (uintptr_t)fn;   // r12
    sp[6] = (uintptr_t)darwin_fiber_entry;
    f->sp = sp;
    return f;
}

void plat_fiber_switch(PlatFiber *to) {
    PlatFiber *from = current;
    current = to;
    darwin_fiber_swap(from, to);
}

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    munmap(f->mapping, f->size);
    free(f);
}
