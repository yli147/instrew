
#include <common.h>

#include <dispatch.h>

#include <elf.h>

#include <dispatcher-info.h>
#include <memory.h>
#include <rtld.h>
#include <state.h>
#include <translator.h>


// Prototype to make compilers happy. This is used in the assembly HHVM
// dispatcher on x86-64 below.
uintptr_t resolve_func(struct CpuState*, uintptr_t, struct RtldPatchData*);

static void
print_trace(struct CpuState* cpu_state, uintptr_t addr) {
    uint64_t* cpu_regs = (uint64_t*) cpu_state->regdata;
    dprintf(2, "Trace 0x%lx\n", addr);
    if (cpu_state->state->tc.tc_print_regs) {
        dprintf(2, "RAX=%lx RBX=%lx RCX=%lx RDX=%lx\n", cpu_regs[1], cpu_regs[4], cpu_regs[2], cpu_regs[3]);
        dprintf(2, "RSI=%lx RDI=%lx RBP=%lx RSP=%lx\n", cpu_regs[7], cpu_regs[8], cpu_regs[6], cpu_regs[5]);
        dprintf(2, "R8 =%lx R9 =%lx R10=%lx R11=%lx\n", cpu_regs[9], cpu_regs[10], cpu_regs[11], cpu_regs[12]);
        dprintf(2, "R12=%lx R13=%lx R14=%lx R15=%lx\n", cpu_regs[13], cpu_regs[14], cpu_regs[15], cpu_regs[16]);
        dprintf(2, "RIP=%lx\n", addr);
        dprintf(2, "XMM0=%lx:%lx XMM1=%lx:%lx\n", cpu_regs[18], cpu_regs[19], cpu_regs[20], cpu_regs[21]);
        dprintf(2, "XMM2=%lx:%lx XMM3=%lx:%lx\n", cpu_regs[22], cpu_regs[23], cpu_regs[24], cpu_regs[25]);
        dprintf(2, "XMM4=%lx:%lx XMM5=%lx:%lx\n", cpu_regs[26], cpu_regs[27], cpu_regs[28], cpu_regs[29]);
        dprintf(2, "XMM6=%lx:%lx XMM7=%lx:%lx\n", cpu_regs[30], cpu_regs[31], cpu_regs[32], cpu_regs[33]);
    }
}

#define QUICK_TLB_BITS 10
#define QUICK_TLB_BITOFF 4 // must be either 1, 2, 3, or 4
// Clang's inline assembly doesn't support expressions for index scale.
// #define QUICK_TLB_IDXSCALE (1 << (4-QUICK_TLB_BITOFF))
#if QUICK_TLB_BITOFF == 4
#define QUICK_TLB_IDXSCALE 1
#elif QUICK_TLB_BITOFF == 3
#define QUICK_TLB_IDXSCALE 2
#elif QUICK_TLB_BITOFF == 2
#define QUICK_TLB_IDXSCALE 4
#elif QUICK_TLB_BITOFF == 1
#define QUICK_TLB_IDXSCALE 8
#else
#error "invalid QUICK_TLB_BITOFF"
#endif
#define QUICK_TLB_HASH(addr) (((addr) >> QUICK_TLB_BITOFF) & ((1 << QUICK_TLB_BITS) - 1))

GNU_FORCE_EXTERN
uintptr_t
resolve_func(struct CpuState* cpu_state, uintptr_t addr,
             struct RtldPatchData* patch_data) {
    struct State* state = cpu_state->state;

    if (patch_data)
        addr = patch_data->sym_addr;

    void* func;
    int retval = rtld_resolve(&state->rtld, addr, &func);
    if (UNLIKELY(retval < 0)) {
        struct timespec start_time;
        struct timespec end_time;
        if (UNLIKELY(state->tc.tc_profile))
            clock_gettime(CLOCK_MONOTONIC, &start_time);

        void* obj_base;
        size_t obj_size;
        retval = translator_get(&state->translator, addr, &obj_base, &obj_size);
        if (retval < 0)
            goto error;

        retval = rtld_add_object(&state->rtld, obj_base, obj_size, addr);
        if (retval < 0)
            goto error;
        retval = rtld_resolve(&state->rtld, addr, &func);
        if (retval < 0)
            goto error;

        if (UNLIKELY(state->tc.tc_profile)) {
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            size_t time_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000
                             + (end_time.tv_nsec - start_time.tv_nsec);
            state->rew_time += time_ns;
        }
    }

    // If we want a trace, don't update quick TLB. This forces a full resolve on
    // every dispatch, yielding a complete trace. Tracing is slow anyway, so we
    // don't care about performance when tracing is active.
    if (LIKELY(!state->tc.tc_print_trace)) {
        // If possible, patch code which caused us to get here.
        rtld_patch(patch_data, func);

        // Update quick TLB
        uintptr_t hash = QUICK_TLB_HASH(addr);
        cpu_state->quick_tlb[hash][0] = addr;
        cpu_state->quick_tlb[hash][1] = (uintptr_t) func;
    } else {
        print_trace(cpu_state, addr);
    }

    return (uintptr_t) func;

error:
    dprintf(2, "error resolving address %lx: %u\n", addr, -retval);
    _exit(retval);
}

// Used for PLT.
void dispatch_cdecl(uint64_t*);

inline void dispatch_cdecl(uint64_t* cpu_regs) {
    struct CpuState* cpu_state = CPU_STATE_FROM_REGS(cpu_regs);

    uintptr_t addr = cpu_regs[0];
    uintptr_t hash = QUICK_TLB_HASH(addr);

    uintptr_t func = cpu_state->quick_tlb[hash][1];
    if (UNLIKELY(cpu_state->quick_tlb[hash][0] != addr))
        func = resolve_func(cpu_state, addr, NULL);

    void(* func_p)(void*);
    *((void**) &func_p) = (void*) func;
    func_p(cpu_regs);
}

static void
dispatch_cdecl_loop(uint64_t* cpu_regs) {
    while (true)
        dispatch_cdecl(cpu_regs);
}

#ifdef __x86_64__

__attribute__((noreturn)) extern void dispatch_hhvm(uint64_t* cpu_state);
void dispatch_hhvm_tail();
void dispatch_hhvm_fullresolve();

__attribute__((noreturn)) extern void dispatch_regcall(uint64_t* cpu_state);
void dispatch_regcall_tail();
void dispatch_regcall_fullresolve();

#define QUICK_TLB_OFFSET_ASM(dest_reg, addr_reg) \
        lea dest_reg, [addr_reg * 4]; \
        and dest_reg, ((1 << QUICK_TLB_BITS) - 1) << (2 + QUICK_TLB_BITOFF);

ASM_BLOCK(
    .intel_syntax noprefix;

    // Stores result in r14, preserves all other registers
    .align 16;
    .type dispatch_hhvm_fullresolve, @function;
dispatch_hhvm_fullresolve: // stack alignment: cdecl
    // Save all cdecl caller-saved registers.
    push rax;
    push rcx;
    push rdx;
    push rsi;
    push rdi;
    push r8;
    push r9;
    push r10;
    push r11;
    mov rdi, [r12 - CPU_STATE_REGDATA_OFFSET]; // cpu_state
    mov rsi, rbx; // addr
    mov rdx, r14; // patch data
    call resolve_func;
    mov r14, rax; // return value
    // Restore callee-saved registers.
    pop r11;
    pop r10;
    pop r9;
    pop r8;
    pop rdi;
    pop rsi;
    pop rdx;
    pop rcx;
    pop rax;
    jmp r14;
    .size dispatch_hhvm_fullresolve, .-dispatch_hhvm_fullresolve;

    .align 16;
    .global dispatch_hhvm_tail;
    .type dispatch_hhvm_tail, @function;
dispatch_hhvm_tail: // stack alignment: cdecl
    mov r14, rbx;
    and r14, ((1 << QUICK_TLB_BITS) - 1) << QUICK_TLB_BITOFF;
    cmp rbx, [r12 + QUICK_TLB_IDXSCALE*r14 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET];
    jne 1f;
    jmp [r12 + QUICK_TLB_IDXSCALE*r14 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET + 8];
    .align 16;
1:  xor r14, r14; // zero patch data
    jmp dispatch_hhvm_fullresolve;
    .size dispatch_hhvm_tail, .-dispatch_hhvm_tail;

    .align 16;
    .global dispatch_hhvm_call;
    .type dispatch_hhvm_call, @function;
dispatch_hhvm_call: // stack alignment: hhvm
    mov r14, rbx;
    and r14, ((1 << QUICK_TLB_BITS) - 1) << QUICK_TLB_BITOFF;
    cmp rbx, [r12 + QUICK_TLB_IDXSCALE*r14 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET];
    jne 1f;
    call [r12 + QUICK_TLB_IDXSCALE*r14 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET + 8];
    ret;
    .align 16;
1:  xor r14, r14; // zero patch data
    call dispatch_hhvm_fullresolve;
    ret;
    .size dispatch_hhvm_call, .-dispatch_hhvm_call;

    .align 16;
    .global dispatch_hhvm;
    .type dispatch_hhvm, @function;
dispatch_hhvm:
    mov r12, rdi; // cpu_regs
    // Load HHVM registers
    mov rbx, [r12 + 0 * 8];
    mov rax, [r12 + 1 * 8];
    mov rcx, [r12 + 2 * 8];
    mov rdx, [r12 + 3 * 8];
    mov rbp, [r12 + 4 * 8];
    mov r15, [r12 + 5 * 8];
    mov r13, [r12 + 6 * 8];
    mov rsi, [r12 + 7 * 8];
    mov rdi, [r12 + 8 * 8];
    mov r8, [r12 + 9 * 8];
    mov r9, [r12 + 10 * 8];
    mov r10, [r12 + 11 * 8];
    mov r11, [r12 + 12 * 8];

    jmp 4f;

    .align 16;
    // This is the quick_tlb hot loop.
2:  call [r12 + QUICK_TLB_IDXSCALE*r14 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET + 8];
3:  mov r14, rbx;
    and r14, ((1 << QUICK_TLB_BITS) - 1) << QUICK_TLB_BITOFF;
    cmp rbx, [r12 + QUICK_TLB_IDXSCALE*r14 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET];
    je 2b;

    // This code isn't exactly cold, but should be executed not that often.
    // If we don't have addr in the quick_tlb, do a full resolve.
4:  xor r14, r14; // zero patch data
    call dispatch_hhvm_fullresolve;
    jmp 3b;
    .size dispatch_hhvm, .-dispatch_hhvm;

    .att_syntax;
);

ASM_BLOCK(
    .intel_syntax noprefix;

    // Stores result in r10, preserves all other registers
    .align 16;
    .type dispatch_regcall_fullresolve, @function;
dispatch_regcall_fullresolve:
    // Save all cdecl caller-saved registers.
    push rax;
    push rcx;
    push rdx;
    push rsi;
    push rdi;
    push r8;
    push r9;
    mov rdi, [rax - CPU_STATE_REGDATA_OFFSET]; // cpu_state
    mov rsi, rcx; // addr
    mov rdx, r10; // patch data
    call resolve_func;
    mov r10, rax; // return value
    // Restore callee-saved registers.
    pop r9;
    pop r8;
    pop rdi;
    pop rsi;
    pop rdx;
    pop rcx;
    pop rax;
    jmp r10;
    .size dispatch_regcall_fullresolve, .-dispatch_regcall_fullresolve;

    .align 16;
    .global dispatch_regcall_tail;
    .type dispatch_regcall_tail, @function;
dispatch_regcall_tail:
    mov r10, rcx;
    and r10, ((1 << QUICK_TLB_BITS) - 1) << QUICK_TLB_BITOFF;
    cmp rcx, [rax + QUICK_TLB_IDXSCALE*r10 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET];
    jne 1f;
    jmp [rax + QUICK_TLB_IDXSCALE*r10 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET + 8];
    .align 16;
1:  xor r10, r10; // zero patch data
    jmp dispatch_regcall_fullresolve;
    .size dispatch_regcall_tail, .-dispatch_regcall_tail;

    .align 16;
    .global dispatch_regcall;
    .type dispatch_regcall, @function;
dispatch_regcall:
    mov rax, rdi; // cpu_regs
    // Load regcall registers
    mov rcx, [rax + 0 * 8];
    mov rdx, [rax + 1 * 8];
    mov rdi, [rax + 2 * 8];
    mov rsi, [rax + 3 * 8];
    mov r8, [rax + 4 * 8];
    mov r9, [rax + 5 * 8];
    mov r12, [rax + 7 * 8];
    mov r13, [rax + 8 * 8];
    mov r14, [rax + 9 * 8];
    mov r15, [rax + 10 * 8];

    jmp 4f;

    .align 16;
    // This is the quick_tlb hot loop.
2:  call [rax + QUICK_TLB_IDXSCALE*r10 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET + 8];
3:  mov r10, rcx;
    and r10, ((1 << QUICK_TLB_BITS) - 1) << QUICK_TLB_BITOFF;
    cmp rcx, [rax + QUICK_TLB_IDXSCALE*r10 - CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET];
    je 2b;

    // This code isn't exactly cold, but should be executed not that often.
    // If we don't have addr in the quick_tlb, do a full resolve.
4:  xor r10, r10; // zero patch data
    call dispatch_regcall_fullresolve;
    jmp 3b;
    .size dispatch_regcall, .-dispatch_regcall;

    .att_syntax;
);

#endif // defined(__x86_64__)

#if defined(__aarch64__)

void dispatch_aapcsx();
void dispatch_aapcsx_fullresolve();
void dispatch_aapcsx_loop();

ASM_BLOCK(
    .align 16;
    .global dispatch_aapcsx;
    .type dispatch_aapcsx, @function;
dispatch_aapcsx:
    add x17, x20, -CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET;
    and x16, x0, ((1 << QUICK_TLB_BITS) - 1) << QUICK_TLB_BITOFF;
    add x17, x17, x16, lsl (4-QUICK_TLB_BITOFF);
    ldp x16, x17, [x17];
    cmp x16, x0;
    b.ne 1f;
    br x17;
1:  mov x16, xzr; // zero dispatch data
    b dispatch_aapcsx_fullresolve;
    .size dispatch_aapcsx, .-dispatch_aapcsx;

    .align 16;
    .type dispatch_aapcsx_loop, @function;
dispatch_aapcsx_loop:
    mov x20, x0; // reg_data
    ldr x0, [x0]; // addr
    ldr x1, [x20, 0x8];
    ldr x2, [x20, 0x10];
    ldr x3, [x20, 0x18];
    ldr x4, [x20, 0x20];
    ldr x5, [x20, 0x28];
    ldr x6, [x20, 0x38];
    ldr x7, [x20, 0x40];
    b 2f;

    .align 16;
1:  blr x17;
2:  add x17, x20, -CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET;
    and x16, x0, ((1 << QUICK_TLB_BITS) - 1) << QUICK_TLB_BITOFF;
    add x17, x17, x16, lsl (4-QUICK_TLB_BITOFF);
    ldp x16, x17, [x17];
    cmp x16, x0;
    b.eq 1b;

    mov x16, xzr; // zero patch data
    bl dispatch_aapcsx_fullresolve;
    b 2b;
    .size dispatch_aapcsx_loop, .-dispatch_aapcsx_loop;

    .align 16;
    .global dispatch_aapcsx_fullresolve;
    .type dispatch_aapcsx_fullresolve, @function;
dispatch_aapcsx_fullresolve:
    sub sp, sp, 0x2b0;
    stp x18, x30, [sp];
    stp x0, x1, [sp, 0x10];
    stp x2, x3, [sp, 0x20];
    stp x4, x5, [sp, 0x30];
    stp x6, x7, [sp, 0x40];
    stp x8, x9, [sp, 0x50];
    stp x10, x11, [sp, 0x60];
    stp x12, x13, [sp, 0x70];
    stp x14, x15, [sp, 0x80];
    stp q0, q1, [sp, 0x90];
    stp q2, q3, [sp, 0xb0];
    stp q4, q5, [sp, 0xd0];
    stp q6, q7, [sp, 0xf0];
    stp q6, q7, [sp, 0x110];
    stp q8, q9, [sp, 0x130];
    stp q10, q11, [sp, 0x150];
    stp q12, q13, [sp, 0x170];
    stp q14, q15, [sp, 0x190];
    stp q16, q17, [sp, 0x1b0];
    stp q18, q19, [sp, 0x1d0];
    stp q20, q21, [sp, 0x1f0];
    stp q22, q23, [sp, 0x210];
    stp q24, q25, [sp, 0x230];
    stp q26, q27, [sp, 0x250];
    stp q28, q29, [sp, 0x270];
    stp q30, q31, [sp, 0x290];

    mov x1, x0; // addr
    sub x0, x20, CPU_STATE_REGDATA_OFFSET; // cpu_state
    mov x2, x16; // patch_data
    bl resolve_func;
    mov x16, x0;

    ldp x18, x30, [sp];
    ldp x0, x1, [sp, 0x10];
    ldp x2, x3, [sp, 0x20];
    ldp x4, x5, [sp, 0x30];
    ldp x6, x7, [sp, 0x40];
    ldp x8, x9, [sp, 0x50];
    ldp x10, x11, [sp, 0x60];
    ldp x12, x13, [sp, 0x70];
    ldp x14, x15, [sp, 0x80];
    ldp q0, q1, [sp, 0x90];
    ldp q2, q3, [sp, 0xb0];
    ldp q4, q5, [sp, 0xd0];
    ldp q6, q7, [sp, 0xf0];
    ldp q6, q7, [sp, 0x110];
    ldp q8, q9, [sp, 0x130];
    ldp q10, q11, [sp, 0x150];
    ldp q12, q13, [sp, 0x170];
    ldp q14, q15, [sp, 0x190];
    ldp q16, q17, [sp, 0x1b0];
    ldp q18, q19, [sp, 0x1d0];
    ldp q20, q21, [sp, 0x1f0];
    ldp q22, q23, [sp, 0x210];
    ldp q24, q25, [sp, 0x230];
    ldp q26, q27, [sp, 0x250];
    ldp q28, q29, [sp, 0x270];
    ldp q30, q31, [sp, 0x290];
    add sp, sp, 0x2b0;
    br x16;
    .size dispatch_aapcsx_fullresolve, .-dispatch_aapcsx_fullresolve;
);

#endif // defined(__aarch64__)

#if defined(__riscv)

// RISC-V LP64 dispatcher (A1' design). Physical register layout after
// LLVM's sret lowering of the 7-element return struct:
//   a0 = sret pointer (points to a 7-i64 slot; unused by dispatcher)
//   a1 = sptr (regdata pointer) - persists through translated code
//   a2 = PC (arg 1, mapped to guest PC/RIP)
//   a3..a7 = 5 additional packed guest regs (guest-specific mapping;
//   see SptrFields in server/callconv.cc)
//
// patch_data_reg = t6 (x31). No SwiftSelf: LLVM RISC-V ignores it.

void dispatch_lp64();
void dispatch_lp64_fullresolve();

// Hot loop, called with a0..a7 already in position (via C ABI).
extern void dispatch_lp64_hot(void* sret_slot, uint64_t* cpu_regs,
                              uint64_t a2_pc, uint64_t a3, uint64_t a4,
                              uint64_t a5, uint64_t a6, uint64_t a7);

// Entry: figure out guest-specific initial values for a2..a7, then
// call into the hot loop with sret slot pre-allocated on the stack.
static void
dispatch_lp64_loop(uint64_t* cpu_regs) {
    struct CpuState* cpu_state = CPU_STATE_FROM_REGS(cpu_regs);
    unsigned guest_arch = cpu_state->state->tsc.tsc_guest_arch;
    // Byte offsets into regdata for the 6 in-reg guest regs (PC + 5).
    // Field mapping mirrors server/callconv.cc CallConv::*_RV64_LP64.
    uint64_t off[6] = {0};
    if (guest_arch == EM_X86_64) {
        // RIP, RAX, RCX, RDX, RBX, RSP
        off[0] = 0;  off[1] = 8;  off[2] = 16; off[3] = 24;
        off[4] = 32; off[5] = 40;
    } else if (guest_arch == EM_RISCV) {
        // RIP, X10, X11, X12, X13, X14
        off[0] = 0;  off[1] = 88;  off[2] = 96;  off[3] = 104;
        off[4] = 112; off[5] = 120;
    } else if (guest_arch == EM_AARCH64) {
        // PC, X0, X1, X2, X3, X4
        off[0] = 0;  off[1] = 16; off[2] = 24; off[3] = 32;
        off[4] = 40; off[5] = 48;
    }
    uint8_t* r = (uint8_t*) cpu_regs;
    uint64_t sret_slot[7];  // matches LLVM 7-element ret struct
    dispatch_lp64_hot(sret_slot, cpu_regs,
                      *(uint64_t*)(r + off[0]),
                      *(uint64_t*)(r + off[1]),
                      *(uint64_t*)(r + off[2]),
                      *(uint64_t*)(r + off[3]),
                      *(uint64_t*)(r + off[4]),
                      *(uint64_t*)(r + off[5]));
}

ASM_BLOCK(
    // dispatch_lp64: quick-TLB tail dispatcher. Tail-called from
    // translated code at BB boundaries. Physical regs on entry:
    //   a0=sret_slot, a1=sptr, a2=PC. a3..a7 = packed guest regs.
    // We stash a1..a7 into the sret slot first, so that any subsequent
    // fullresolve→ret→dispatch_lp64_hot.Llp64_after_call reload path
    // sees the current values (translated tail chains bypass the normal
    // struct-return, so the slot could otherwise be stale).
    .align 16;
    .global dispatch_lp64;
    .type dispatch_lp64, @function;
dispatch_lp64:
    sd a1, 0(a0);
    sd a2, 8(a0);
    sd a3, 16(a0);
    sd a4, 24(a0);
    sd a5, 32(a0);
    sd a6, 40(a0);
    sd a7, 48(a0);
    li t3, ((1 << QUICK_TLB_BITS) - 1) << QUICK_TLB_BITOFF;
    and t0, a2, t3;
    add t0, a1, t0;
    li t3, -CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET;
    add t0, t0, t3;
    ld t1, 0(t0);
    ld t2, 8(t0);
    bne t1, a2, 1f;
    jr t2;
1:  mv t6, x0;
    j dispatch_lp64_fullresolve;
    .size dispatch_lp64, .-dispatch_lp64;

    // dispatch_lp64_hot: hot loop. On entry a0..a7 already set by C.
    // s0 keeps the sret slot pointer; a0 (sret ptr) is caller-saved by
    // the RISC-V ABI, so we reload it after every `jalr`.
    .align 16;
    .global dispatch_lp64_hot;
    .type dispatch_lp64_hot, @function;
dispatch_lp64_hot:
    addi sp, sp, -16;
    sd ra, 0(sp);
    sd s0, 8(sp);
    mv s0, a0;
    j .Llp64_check;

.Llp64_after_call:
    // Reload sret slot into arg regs. s0 is callee-saved so it survived.
    mv a0, s0;
    ld a1, 0(s0);
    ld a2, 8(s0);
    ld a3, 16(s0);
    ld a4, 24(s0);
    ld a5, 32(s0);
    ld a6, 40(s0);
    ld a7, 48(s0);
.Llp64_check:
    li t3, ((1 << QUICK_TLB_BITS) - 1) << QUICK_TLB_BITOFF;
    and t0, a2, t3;
    add t0, a1, t0;
    li t3, -CPU_STATE_REGDATA_OFFSET + CPU_STATE_QTLB_OFFSET;
    add t0, t0, t3;
    ld t1, 0(t0);
    ld t2, 8(t0);
    beq t1, a2, .Llp64_hit;
    // TLB miss: call the C resolver in-line here so ra points back to
    // this frame's reload sequence.
    mv t6, x0;
    jal dispatch_lp64_fullresolve;
    jalr t0;
    j .Llp64_after_call;
.Llp64_hit:
    jalr t2;
    j .Llp64_after_call;
    .size dispatch_lp64_hot, .-dispatch_lp64_hot;

    // dispatch_lp64_fullresolve: TLB miss → resolve_func in C. Preserves
    // a0..a7 (sret,sptr,PC,packed) and t6; returns target address in t0.
    // ret to caller (which will jalr t0 to enter the translated code).
    .align 16;
    .global dispatch_lp64_fullresolve;
    .type dispatch_lp64_fullresolve, @function;
dispatch_lp64_fullresolve:
    addi sp, sp, -80;
    sd ra, 0(sp);
    sd a0, 8(sp);
    sd a1, 16(sp);
    sd a2, 24(sp);
    sd a3, 32(sp);
    sd a4, 40(sp);
    sd a5, 48(sp);
    sd a6, 56(sp);
    sd a7, 64(sp);
    sd t6, 72(sp);

    // resolve_func(cpu_state, addr, patch_data)
    addi a0, a1, -CPU_STATE_REGDATA_OFFSET; // arg0 = cpu_state
    mv a1, a2;                              // arg1 = addr (PC)
    mv a2, t6;                              // arg2 = patch_data
    jal resolve_func;
    mv t0, a0;                              // save target (a-reg → t-reg)

    ld ra, 0(sp);
    ld a0, 8(sp);
    ld a1, 16(sp);
    ld a2, 24(sp);
    ld a3, 32(sp);
    ld a4, 40(sp);
    ld a5, 48(sp);
    ld a6, 56(sp);
    ld a7, 64(sp);
    ld t6, 72(sp);
    addi sp, sp, 80;
    ret;
    .size dispatch_lp64_fullresolve, .-dispatch_lp64_fullresolve;
);

#endif // defined(__riscv)

const struct DispatcherInfo*
dispatch_get(struct State* state) {
    static const struct DispatcherInfo infos[] = {
        [0] = {
            .loop_func = dispatch_cdecl_loop,
            .quick_dispatch_func = (uintptr_t) dispatch_cdecl,
            .full_dispatch_func = (uintptr_t) dispatch_cdecl,
            .patch_data_reg = 6, // rsi
        },
#if defined(__x86_64__)
        [1] = {
            .loop_func = dispatch_hhvm,
            .quick_dispatch_func = (uintptr_t) dispatch_hhvm_tail,
            .full_dispatch_func = (uintptr_t) dispatch_hhvm_fullresolve,
            .patch_data_reg = 14, // r14
        },
        [2] = {
            .loop_func = dispatch_regcall,
            .quick_dispatch_func = (uintptr_t) dispatch_regcall_tail,
            .full_dispatch_func = (uintptr_t) dispatch_regcall_fullresolve,
            .patch_data_reg = 10, // r10
        },
#endif // defined(__x86_64__)
#if defined(__aarch64__)
        [3] = {
            .loop_func = dispatch_aapcsx_loop,
            .quick_dispatch_func = (uintptr_t) dispatch_aapcsx,
            .full_dispatch_func = (uintptr_t) dispatch_aapcsx_fullresolve,
            .patch_data_reg = 16, // x16
        },
#endif // defined(__aarch64__)
#if defined(__riscv)
        [4] = {
            .loop_func = dispatch_lp64_loop,
            .quick_dispatch_func = (uintptr_t) dispatch_lp64,
            .full_dispatch_func = (uintptr_t) dispatch_lp64_fullresolve,
            .patch_data_reg = 31, // t6
        },
#endif // defined(__riscv)
    };

    unsigned callconv = state->tc.tc_callconv;
    if (callconv < sizeof infos / sizeof infos[0])
        return &infos[callconv];
    return NULL;
}
