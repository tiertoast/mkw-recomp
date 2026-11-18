// Mach-O x86_64 SysV. Neither a thread pointer nor a signal mask belongs to the saved frame.
.text
.p2align 4
.private_extern _darwin_fiber_swap
_darwin_fiber_swap:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    fxsave64 16(%rdi)
    movq %rsp, (%rdi)
    movq (%rsi), %rsp
    fxrstor64 16(%rsi)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.p2align 4
.private_extern _darwin_fiber_entry
_darwin_fiber_entry:
    .cfi_startproc
    .cfi_undefined rip
    movq %r13, %rdi
    callq *%r12       // The initial return left rsp at the 16-byte-aligned top.
    ud2              // A fiber entry must switch away before returning.
    .cfi_endproc

// A guest setjmp owner can outlive the pthread on which the fiber first ran.
// Resume at the saved C call site without restoring pthread TLS or a signal mask.
.p2align 4
.private_extern _darwin_guest_save
_darwin_guest_save:
    movq %rbp, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret

.p2align 4
.private_extern _darwin_guest_resume
_darwin_guest_resume:
    movq 0(%rdi), %rbp
    movq 8(%rdi), %rbx
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movl $1, %eax
    jmpq *56(%rdi)
