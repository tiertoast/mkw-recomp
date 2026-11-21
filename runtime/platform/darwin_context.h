// Nonlocal guest returns target a live frame on the same fiber stack, possibly on a different pthread.
// Only the SysV preserved integer registers and the caller continuation belong to that frame.
#pragma once
#include <signal.h>
#include <stdint.h>

typedef struct { uintptr_t rbp, rbx, r12, r13, r14, r15, sp, pc; } DarwinGuestJump;
int darwin_guest_save(DarwinGuestJump *slot) __attribute__((returns_twice));
_Noreturn void darwin_guest_resume(const DarwinGuestJump *slot);

// Preserve any alternate signal stack already installed by the thread owner.
void darwin_crash_stack(void);

// A synchronous SIGBUS/SIGSEGV the write watch's handler (darwin_mem.c) does not own: the crash report when
// plat_crash_hook set one, else the handler prior holds, else the default action once the instruction runs again.
void darwin_crash_forward(int sig, siginfo_t *si, void *context, const struct sigaction *prior);
