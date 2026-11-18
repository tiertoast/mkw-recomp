// Set different values on both sides of the switch so an omitted register save cannot pass by coincidence.
.text
.globl _fiber_check_registers
_fiber_check_registers:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rsi              // seed; rsp is aligned for the call
    leaq 1(%rsi), %rbp
    leaq 2(%rsi), %rbx
    leaq 3(%rsi), %r12
    leaq 4(%rsi), %r13
    leaq 5(%rsi), %r14
    leaq 6(%rsi), %r15
    callq _plat_fiber_switch
    popq %rcx
    xorl %eax, %eax
    incq %rcx
    cmpq %rcx, %rbp
    jne 1f
    incq %rcx
    cmpq %rcx, %rbx
    jne 1f
    incq %rcx
    cmpq %rcx, %r12
    jne 1f
    incq %rcx
    cmpq %rcx, %r13
    jne 1f
    incq %rcx
    cmpq %rcx, %r14
    jne 1f
    incq %rcx
    cmpq %rcx, %r15
    jne 1f
    movl $1, %eax
1:  popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.globl _fiber_test_entry
_fiber_test_entry:
    movq %rsp, %rax
    andl $15, %eax
    cmpl $8, %eax
    jne 1f
    movq %rdi, -128(%rsp)   // bottom of the SysV red zone, before a call
    jmp _fiber_test_body
1:  ud2
