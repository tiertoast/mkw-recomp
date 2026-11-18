// Nonlocal guest returns target a live frame on the same fiber stack, possibly on a different pthread.
// Only the SysV preserved integer registers and the caller continuation belong to that frame.
#pragma once
#include <stdint.h>

typedef struct { uintptr_t rbp, rbx, r12, r13, r14, r15, sp, pc; } DarwinGuestJump;
int darwin_guest_save(DarwinGuestJump *slot) __attribute__((returns_twice));
_Noreturn void darwin_guest_resume(const DarwinGuestJump *slot);
