// Report synchronous faults in mkw's text only. The only protection faults that are retried are the write watch's
// (darwin_mem.c): its handler, once installed, owns SIGBUS/SIGSEGV and sends every other fault here.
#include "platform.h"
#include "darwin_context.h"
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

void darwin_crash_stack(void) {
    static _Thread_local _Alignas(16) unsigned char bytes[65536];
    stack_t current;
    if (!sigaltstack(NULL, &current) && (current.ss_flags & SS_DISABLE)) {
        stack_t stack = {.ss_sp = bytes, .ss_size = sizeof bytes};
        sigaltstack(&stack, NULL);
    }
}

static uintptr_t base, text_begin, text_end;
static void (*report)(const PlatCrash *);

static void fault(int sig, siginfo_t *si, void *context) {
    ucontext_t *uc = context;
    uintptr_t pc = uc->uc_mcontext->__ss.__rip;
    int synchronous = si->si_code > 0 && si->si_code < SI_USER;
    if (synchronous && pc >= text_begin && pc < text_end) {
        unsigned err = uc->uc_mcontext->__es.__err;
        int memory = sig == SIGSEGV || sig == SIGBUS;
        PlatCrash c = {.what = memory ? "access violation" : sig == SIGILL ? "illegal instruction" : "arithmetic fault",
                       .code = sig, .rip = pc, .offset = pc - base, .addr = (uintptr_t)si->si_addr,
                       .access = memory ? (err & 16 ? 8 : (err >> 1) & 1) : -1};
        report(&c);
    }
    // Resume synchronous exceptions at their original PC under SIG_DFL so macOS records the real fault.
    struct sigaction def = {.sa_handler = SIG_DFL};
    sigemptyset(&def.sa_mask);
    sigaction(sig, &def, NULL);
    if (!synchronous) raise(sig);
}

void darwin_crash_forward(int sig, siginfo_t *si, void *context, const struct sigaction *prior) {
    if (report) {
        fault(sig, si, context);
        return;
    }
    if (prior && (prior->sa_flags & SA_SIGINFO) && prior->sa_sigaction) {
        prior->sa_sigaction(sig, si, context);
        return;
    }
    if (prior && prior->sa_handler != SIG_DFL && prior->sa_handler != SIG_IGN) {
        prior->sa_handler(sig);
        return;
    }
    // As fault(): the default action records the real fault when the instruction runs again.
    struct sigaction def = {.sa_handler = SIG_DFL};
    sigemptyset(&def.sa_mask);
    sigaction(sig, &def, NULL);
}

void plat_crash_hook(void (*fn)(const PlatCrash *)) {
    if (!fn) return;
    const struct mach_header_64 *image = (const struct mach_header_64 *)_dyld_get_image_header(0);
    unsigned long length;
    const uint8_t *code = getsectiondata(image, "__TEXT", "__text", &length);
    if (!code || !length) return;
    darwin_crash_stack();
    base = (uintptr_t)image;
    text_begin = (uintptr_t)code;
    text_end = text_begin + length;
    report = fn;
    struct sigaction action = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (unsigned i = 0; i < sizeof signals / sizeof *signals; i++) {
        struct sigaction prior;
        if (!sigaction(signals[i], NULL, &prior) && prior.sa_handler == SIG_DFL)
            sigaction(signals[i], &action, NULL);
    }
}
