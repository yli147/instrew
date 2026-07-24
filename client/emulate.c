
#include <common.h>

#include <dispatch.h>
#include <dispatcher-info.h>
#include <elf.h>
#include <emulate.h>
#include <memory.h>
#include <rtld.h>

#include <asm/sigcontext.h>
#include <asm/siginfo.h>
#include <asm/signal.h>
#include <asm/stat.h>
#include <asm/ucontext.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/utsname.h>

#include <state.h>
#include <translator.h>

// RISC-V does not have __NR_renameat, use renameat2 with flags=0
#ifndef __NR_renameat
#define __NR_renameat __NR_renameat2
#endif


// SIG_DFL should be zero, so zero-initializing sigact is sufficient
// Unfortunately, this is not an integer constant expression.
// _Static_assert(SIG_DFL == 0, "SIG_DFL mismtach");
// We currently only support guest--host combinations with identical signal nums
_Static_assert(SIGUSR1 == 10, "SIGUSR10 mismtach");

static _Noreturn void
abort_with_signal(int sig) {
    if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH)
        sig = SIGABRT;
    struct sigaction act;
    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigfillset(&act.sa_mask);
    sigaction(sig, &act, NULL); // change handler to SIG_DFL
    kill(getpid(), sig); // send signal to ourselves
    sigdelset(&act.sa_mask, sig);
    sigsuspend(&act.sa_mask); // wait for signal to get delivered
    __builtin_unreachable();
}

static int
signal_update_hostmask(struct CpuState* cpu_state) {
    sigset_t hostmask = cpu_state->sigmask;
    sigdelset(&hostmask, SIGSEGV);
    sigdelset(&hostmask, SIGBUS);
    return sigprocmask(SIG_SETMASK, &hostmask, NULL);
}

static void
signal_handle(struct CpuState* cpu_state) {
    int sig = cpu_state->sigpending;
    cpu_state->sigpending = 0;

    // Copy act to reduce likelihood of race. TODO: make this thread-safe.
    struct sigaction act = cpu_state->state->sigact[sig - 1];
    if (act.sa_handler == SIG_DFL) {
        abort_with_signal(sig);
    } else if (act.sa_handler == SIG_IGN) {
        // do nothing
    } else {
        dprintf(2, "guest signal handling not implemented! %d\n", sig);
        abort_with_signal(sig);

        // TODO: setup signal stack frame and update registers/guest pc

        for (int i = 1; i <= _NSIG; i++)
            if (sigismember(&act.sa_mask, i))
                sigaddset(&cpu_state->sigmask, i);
        signal_update_hostmask(cpu_state);

        if (act.sa_flags & SA_RESETHAND)
            cpu_state->state->sigact[sig - 1].sa_handler = SIG_DFL;
    }
}

static void
signal_sigreturn(struct CpuState* cpu_state) {
    (void) cpu_state;
    dprintf(2, "guest sigreturn not implemented!\n");
    abort_with_signal(SIGABRT);
}

static void
signal_handler(int sig, struct siginfo* info, void* ucp) {
    struct CpuState* cpu_state = get_thread_area();
    if ((sig == SIGSEGV || sig == SIGBUS) && info->si_code > 0) {
        // Synchronous signal.
        // Need to get out of translated code to handle signal.
        // Maybe we need to invalidate some parts of the code cache in future.
        // But for now, just abort.
        if (cpu_state->state->sigact[sig - 1].sa_handler != SIG_DFL)
            dprintf(2, "handling of synchronous signals not implemented\n");
        abort_with_signal(sig);
    }

    // Should only happen for SIGSEGV and SIGBUS, which we never mask.
    if (sigismember(&cpu_state->sigmask, sig)) {
        dprintf(2, "dropping asynchronous blocked signal %d\n", sig);
        return;
    }

    // Asynchronous signal. Mark as pending and store info in CPU state.
    // But: keep signals masked so that only ever at most one signal is pending.
    // TODO: this is a very simple and INCORRECT implementation of signals.
    cpu_state->sigpending = sig;
    cpu_state->siginfo = *info;
    // Keep all signals masked until guest signal handler executed.
    struct ucontext* uc = ucp;
    sigfillset(&uc->uc_sigmask);
    sigdelset(&uc->uc_sigmask, SIGSEGV);
    sigdelset(&uc->uc_sigmask, SIGBUS);
}

static int
signal_sigaction(struct CpuState* cpu_state, int sig,
                 const struct sigaction* restrict nact,
                 struct sigaction* restrict oact) {
    struct State* state = cpu_state->state;
    if (sig <= 0 || sig > _NSIG)
        return -EINVAL;
    if (oact)
        *oact = state->sigact[sig - 1];
    if (!nact)
        return 0;

    state->sigact[sig - 1] = *nact;
    if (sig == SIGSEGV || sig == SIGBUS)
        return 0;

    struct sigaction act;
    if (nact->sa_handler == SIG_DFL || nact->sa_handler == SIG_IGN)
        act.sa_handler = nact->sa_handler;
    else
        act.sa_handler = (void(*)()) signal_handler;
    // Only SA_RESTART can be passed, other's have to be emulated.
    act.sa_flags = SA_SIGINFO;
    if (nact->sa_flags & SA_RESTART)
        act.sa_flags |= SA_RESTART;
    sigfillset(&act.sa_mask);
    return sigaction(sig, &act, NULL);
}

static int
signal_sigprocmask(struct CpuState* cpu_state, int how,
                   const sigset_t* restrict set, sigset_t* restrict oldset) {
    if (oldset)
        *oldset = cpu_state->sigmask;
    if (!set)
        return 0;
    if (how == SIG_BLOCK) {
        for (int i = 1; i <= _NSIG; i++)
            if (sigismember(set, i))
                sigaddset(&cpu_state->sigmask, i);
    } else if (how == SIG_UNBLOCK) {
        for (int i = 1; i <= _NSIG; i++)
            if (sigismember(set, i))
                sigdelset(&cpu_state->sigmask, i);
    } else if (how == SIG_SETMASK) {
        cpu_state->sigmask = *set;
    } else {
        return -EINVAL;
    }
    sigdelset(&cpu_state->sigmask, SIGKILL);
    sigdelset(&cpu_state->sigmask, SIGSTOP);
    // Only update host mask if there is no signal pending, otherwise, we could
    // end up with multiple queued signals (the handler keeps signals blocked).
    if (!cpu_state->sigpending)
        return signal_update_hostmask(cpu_state);
    return 0;
}

static int
signal_sigaltstack(struct CpuState* cpu_state, const stack_t *restrict ss,
                   stack_t *restrict old_ss) {
    if (old_ss)
        *old_ss = cpu_state->sigaltstack;
    if (!ss)
        return 0;

    // TODO: verify flags, size, and that current stack is not in use (EPERM).
    cpu_state->sigaltstack = *ss;
    return 0;
}

void
signal_init(struct State* state) {
    // state->sigact will be zero-initialized, i.e. SIG_DFL.
    (void) state;

    struct sigaction act;
    // actually this should be act.sa_sigaction.
    act.sa_handler = (void(*)()) signal_handler;
    act.sa_flags = SA_SIGINFO;
    sigfillset(&act.sa_mask);
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
}

// Arguments passed from the creating thread to the new guest thread.
// Placed at the top of the host stack so __clone can find them.
struct ThreadStartArgs {
    struct CpuState* cpu_state;
    const struct DispatcherInfo* disp_info;
};

// Entry function for every new guest thread. Called by __clone's child path.
// Sets up TP to point to this thread's CpuState, then enters dispatch loop.
static int
thread_entry(void* arg) {
    struct ThreadStartArgs* tsa = (struct ThreadStartArgs*) arg;
    set_thread_area(tsa->cpu_state);
    tsa->disp_info->loop_func((uint64_t*) tsa->cpu_state->regdata);
    return 0; // unreachable — loop_func never returns
}

static int
handle_clone(struct State* state, struct clone_args* uargs, size_t usize) {
    struct clone_args args = {0};
    if (usize <= sizeof(args)) {
        memcpy(&args, uargs, usize);
    } else {
        // TODO: accept if excess arguments are all-zero.
        return -E2BIG;
    }

    if (args.flags & CLONE_VM) {
        // --- Thread creation (CLONE_VM) ---
        // Each thread gets its own CpuState (registers, quick TLB) and its
        // own State (translator socket, rtld). They share the host address
        // space but the JIT code arena is thread-safe via arena_spinlock.
        struct CpuState* parent_cs = get_thread_area();
        const struct DispatcherInfo* disp_info = dispatch_get(state);
        if (!disp_info)
            return -EOPNOTSUPP;

        // 1. Allocate child CpuState; copy parent registers as starting point.
        struct CpuState* child_cs =
            mem_alloc_data(sizeof(*child_cs), _Alignof(*child_cs));
        if (BAD_ADDR(child_cs))
            return -ENOMEM;
        memset(child_cs, 0, sizeof(*child_cs));
        child_cs->self = child_cs;
        child_cs->sigmask = parent_cs->sigmask;
        memcpy(child_cs->regdata, parent_cs->regdata, sizeof(parent_cs->regdata));

        // 2. Allocate and initialize child State (own translator + own rtld).
        struct State* child_state =
            mem_alloc_data(sizeof(*child_state), _Alignof(*child_state));
        if (BAD_ADDR(child_state))
            return -ENOMEM;
        memcpy(child_state, state, sizeof(*child_state));
        child_cs->state = child_state;

        // 3. Connect new translator for the child thread (each thread needs
        //    its own socket — sharing would interleave request/response frames).
        //    translator_fork_prepare sends C_FORK and receives a new socket fd
        //    from the server. translator_thread_init sends C_INIT on that fd
        //    so the forked server enters its normal message-handling loop.
        int new_fd = translator_fork_prepare(&state->translator);
        if (new_fd < 0)
            return new_fd;
        int tret = translator_thread_init(&child_state->translator, new_fd,
                                          &child_state->tsc);
        if (tret < 0) {
            close(new_fd);
            return tret;
        }

        // 4. Initialize child rtld and load the initial dispatch object.
        //    Protocol: C_INIT → S_INIT (translator_config_fetch) → S_OBJECT.
        //    Must read S_INIT before S_OBJECT or the socket gets out of sync.
        int ret = translator_config_fetch(&child_state->translator,
                                          &child_state->tc);
        if (ret < 0)
            return ret;

        ret = rtld_init(&child_state->rtld, disp_info);
        if (ret < 0) {
            close(new_fd);
            return ret;
        }
        ret = rtld_perf_init(&child_state->rtld, child_state->tc.tc_perf);
        (void) ret; // perf init failure is non-fatal

        void* initobj;
        size_t initobj_size;
        ret = translator_get_object(&child_state->translator, &initobj,
                                    &initobj_size);
        if (ret < 0)
            return ret;
        if (initobj_size > 0) {
            ret = rtld_add_object(&child_state->rtld, initobj, initobj_size, 0);
            if (ret < 0)
                return ret;
        }

        // 5. Set guest stack pointer, TLS, and clone return value (0) in child.
        //
        // Layout is GUEST ISA (from callconv.cc / dispatch.c trace output),
        // independent of host ISA. On RISC-V host running x86-64 guest we must
        // use the x86-64 offsets, NOT the host RISC-V offsets.
        //   x86-64 guest: [0]=RIP [1]=RAX [5]=RSP [18]=FS_BASE
        //   AArch64 guest: [0]=PC [2]=X0 [33]=SP
        //   RISC-V guest:  [0]=PC [11]=a0 [3]=X2/SP
        //
        // For clone (case 56) args.stack is already the RSP (top of stack).
        // For clone3 (case 435) args.stack is the BASE; RSP = stack + stack_size.
        uint64_t child_rsp = (args.stack_size > 0)
            ? args.stack + args.stack_size
            : args.stack;

        unsigned guest_arch = state->tsc.tsc_guest_arch;
        if (guest_arch == EM_X86_64) {
            ((uint64_t*) child_cs->regdata)[1]  = 0;         // RAX = 0
            ((uint64_t*) child_cs->regdata)[5]  = child_rsp; // RSP
            ((uint64_t*) child_cs->regdata)[18] = args.tls;  // FS_BASE
        } else if (guest_arch == EM_AARCH64) {
            ((uint64_t*) child_cs->regdata)[2]  = 0;         // X0 = 0
            ((uint64_t*) child_cs->regdata)[33] = child_rsp; // SP
        } else if (guest_arch == EM_RISCV) {
            ((uint64_t*) child_cs->regdata)[11] = 0;         // a0 = 0
            ((uint64_t*) child_cs->regdata)[3]  = child_rsp; // X2/SP
        }

        // 6. Write PARENT_SETTID (child's TID) — done by clone syscall itself
        //    when we pass CLONE_PARENT_SETTID to the host clone. We propagate
        //    that flag through.

        // 7. Allocate a host stack for the new thread and place the start args
        //    at its top. __clone stores (fn, arg) there before the syscall and
        //    reads them back in the child after clone returns 0.
#define THREAD_HOST_STACK_SIZE (1u << 20) // 1 MiB host stack per thread
        void* host_stack = mmap(NULL, THREAD_HOST_STACK_SIZE,
                                PROT_READ|PROT_WRITE,
                                MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK, -1, 0);
        if (BAD_ADDR(host_stack))
            return -ENOMEM;
        void* host_stack_top = (char*) host_stack + THREAD_HOST_STACK_SIZE;

        // Place ThreadStartArgs just below the stack top (16-byte aligned).
        struct ThreadStartArgs* tsa =
            (struct ThreadStartArgs*)
            ((uintptr_t) host_stack_top - sizeof(*tsa));
        tsa->cpu_state = child_cs;
        tsa->disp_info = disp_info;

        // Host clone flags: share VM + thread group + signal handlers.
        // Forward SETTID/CLEARTID so futex-based join works correctly.
        int host_flags = CLONE_VM | CLONE_THREAD | CLONE_SIGHAND |
                         CLONE_FS | CLONE_FILES | CLONE_SYSVSEM;
        if (args.flags & CLONE_PARENT_SETTID) host_flags |= CLONE_PARENT_SETTID;
        if (args.flags & CLONE_CHILD_CLEARTID) host_flags |= CLONE_CHILD_CLEARTID;
        if (args.flags & CLONE_CHILD_SETTID)   host_flags |= CLONE_CHILD_SETTID;

        // Use host TP to point to child CpuState (CLONE_SETTLS sets tp/tpidr).
        host_flags |= CLONE_SETTLS;

        ssize_t res = __clone(thread_entry, (void*) tsa,
                              host_flags, tsa,
                              (pid_t*) (uintptr_t) args.parent_tid,
                              child_cs,                   // new TLS = CpuState*
                              (pid_t*) (uintptr_t) args.child_tid);
        if (res < 0) {
            munmap(host_stack, THREAD_HOST_STACK_SIZE);
            close(new_fd);
        }
        return (int) res;
    }

    // Flags used by fork().
    uint64_t supp_flags = CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;
    if (args.flags & ~supp_flags) {
        dprintf(2, "unhandled syscall clone(%lx, ...) = -EINVAL"
                " -- unsupported clone flags, please file a bug\n", args.flags);
        return -EINVAL;
    }

    if (args.stack || args.stack_size) {
        dprintf(2, "unhandled syscall clone(stack != NULL, ...) = -EINVAL"
                " -- fork with stack unsupported, please file a bug\n");
        return -EINVAL;
    }

    // Ensure that we can pass the exit_signal as flag to clone().
    if (args.exit_signal & ~(uint64_t) CSIGNAL)
        return -EINVAL;

    uint64_t flags = args.flags | args.exit_signal;

    // Ok, everything good so far, we will attempt the clone.
    int forked_translator = translator_fork_prepare(&state->translator);
    if (forked_translator < 0) {
        return forked_translator;
    }

    // Signature depends on architecture.
#if defined(__x86_64__)
    // clone(flags, stack, parent_tid, child_tid, tls)
    ssize_t res = syscall(__NR_clone, flags, 0, args.parent_tid, args.child_tid,
                          args.tls, 0);
#elif defined(__aarch64__) || defined(__riscv)
    // clone(flags, stack, parent_tid, tls, child_tid)
    ssize_t res = syscall(__NR_clone, flags, 0, args.parent_tid, args.tls,
                          args.child_tid, 0);
#else
#error "clone not implemented for target"
#endif

    if (res < 0) {
        close(forked_translator);
        return res;
    }

    if (res == 0) {
        // Child: use forked translator
        translator_fork_finalize(&state->translator, forked_translator);
    } else {
        // Parent: close socket for child translator
        close(forked_translator);
    }

    return res;
}

#ifdef __aarch64__
static unsigned
emulate_openat_flags(unsigned uapi_flags) {
    unsigned res = uapi_flags & ~00740000;
    if (uapi_flags & 00040000)
        res |= O_DIRECT;
    if (uapi_flags & 00100000)
        res |= O_LARGEFILE;
    if (uapi_flags & 00200000)
        res |= O_DIRECTORY;
    if (uapi_flags & 00400000)
        res |= O_NOFOLLOW;
    return res;
}
#endif

struct CpuidResult {
    uint32_t res[4];
};

struct CpuidResult
emulate_cpuid(uint32_t rax, uint32_t rcx) {
    struct CpuidResult res = {0};
    if (rax == 0) {
        res.res[0] = 7; // eax = max input value for basic CPUID
        res.res[1] = 0x6c65746e; // ecx = "ntel"
        res.res[2] = 0x49656e69; // edx = "ineI"
        res.res[3] = 0x756e6547; // ebx = "Genu"
    } else if (rax == 1) { // page 1 (feature information)
        res.res[0] = 0; // eax = <not implemented>
        // ECX: report x86-64-v2 baseline so glibc's IFUNC selector doesn't abort.
        // bits: SSE3(0), SSSE3(9), CX16(13), SSE4.1(19), SSE4.2(20),
        //       MOVBE(22), POPCNT(23)
        res.res[1] = 0x00D82201; // ecx = sse3+ssse3+cx16+sse4.1+sse4.2+movbe+popcnt
        res.res[2] = 0x07808141; // edx = pae+cmov+fxsr+sse+sse2+mmx+cx8+fpu
        res.res[3] = 0; // ebx = <not implemented>
    } else if (rax == 2) { // page 2 (TLB/cache/prefetch information)
        // TODO: retrieve actual cache information
        res.res[0] = 0x80000001; // eax = reserved (with al=01)
        // Note: some (~2.36) glibc versions depend on knowing the shared cache
        // size to determine the non-temporal threshold for memset/memcpy.
        // Without data, memcpy/memset behave wrong for medium-sized copies.
        // Therefore, spoof some arbitrary data about the L3 cache size.
        res.res[1] = 0x000000ec; // ecx = null; null; null; L3=24MiB/24w/64l
        res.res[2] = 0x80000000; // edx = reserved
        res.res[3] = 0x80000000; // ebx = reserved
    } else if (rax == 3) { // page 3 (processor serial number; not supported)
        res.res[0] = 0x00000000; // eax = reserved
        res.res[1] = 0x00000000; // ecx = reserved
        res.res[2] = 0x00000000; // edx = reserved
        res.res[3] = 0x00000000; // ebx = reserved
    } else if (rax == 4) { // page 4 (deterministic cache params; not implemented)
        res.res[0] = 0x00000000; // eax = reserved
        res.res[1] = 0x00000000; // ecx = reserved
        res.res[2] = 0x00000000; // edx = reserved
        res.res[3] = 0x00000000; // ebx = reserved
    } else if (rax == 5) { // page 5 (monitor/mwait; mot implemented)
        res.res[0] = 0x00000000; // eax = reserved
        res.res[1] = 0x00000000; // ecx = reserved
        res.res[2] = 0x00000000; // edx = reserved
        res.res[3] = 0x00000000; // ebx = reserved
    } else if (rax == 6) { // page 6 (thermal and power management; not supported)
        res.res[0] = 0x00000000; // eax = reserved
        res.res[1] = 0x00000000; // ecx = reserved
        res.res[2] = 0x00000000; // edx = reserved
        res.res[3] = 0x00000000; // ebx = reserved
    } else if (rax == 7) { // page 7 (structured extended feature flags)
        if (rcx == 0) {
            res.res[0] = 0; // eax = maximum subleave supported
            res.res[1] = 0x00000000; // ecx = reserved
            res.res[2] = 0x00000000; // edx = reserved
            res.res[3] = 0x00002200; // ebx = erms+deprecate fpu-cs/ds
        }
    } else if (rax == 0x80000000) { // extended CPUID: max extended leaf
        res.res[0] = 0x80000001; // eax = max extended leaf supported
    } else if (rax == 0x80000001) { // extended CPUID: extended feature flags
        // ECX bit 0: LAHF/SAHF in 64-bit mode (required for x86-64-v2)
        res.res[1] = 0x00000001; // ecx = lahf64
    } else {
        // All other leaves: return zeros
    }
    return res;
}

static ssize_t
unhandled_syscall(uint64_t nr, uint64_t arg0, uint64_t arg1, uint64_t arg2,
                  uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    // warn only once about each syscall; 1024 should be high enough.
    static uint64_t warnmap[16];
    if (nr < sizeof(warnmap) * 8) {
        if (warnmap[nr / 64] & (1 << nr % 64))
            goto skipwarning;
        warnmap[nr / 64] |= 1 << nr % 64;
    }
    dprintf(2, "unhandled syscall %u (%lx %lx %lx %lx %lx %lx) = -ENOSYS"
            " -- please file a bug with these numbers and the architecture\n",
            nr, arg0, arg1, arg2, arg3, arg4, arg5);

skipwarning:
    return -ENOSYS;
}

void
emulate_syscall(uint64_t* cpu_regs) {
    struct CpuState* cpu_state = CPU_STATE_FROM_REGS(cpu_regs);
    struct State* state = cpu_state->state;

    uint64_t arg0 = cpu_regs[8], arg1 = cpu_regs[7], arg2 = cpu_regs[3],
             arg3 = cpu_regs[11], arg4 = cpu_regs[9], arg5 = cpu_regs[10];
    uint64_t nr = cpu_regs[1];
    ssize_t res = -ENOSYS;

    switch (nr) {
        struct stat tmp_struct;

    native:
        res = syscall(nr, arg0, arg1, arg2, arg3, arg4, arg5);
        break;

    default:
    unhandled:
        res = unhandled_syscall(nr, arg0, arg1, arg2, arg3, arg4, arg5);
        break;

    // Some syscalls are easy.
    case 0: nr = __NR_read; goto native;
    case 1: nr = __NR_write; goto native;
    case 3: nr = __NR_close; goto native;
    case 8: nr = __NR_lseek; goto native;
    case 9: nr = __NR_mmap; goto native; // TODO: catch dangerous maps
    case 10: nr = __NR_mprotect; goto native; // TODO: catch dangerous maps
    case 11: nr = __NR_munmap; goto native; // TODO: catch dangerous maps
    case 12: nr = __NR_brk; goto native; // TODO: catch dangerous maps
    case 16: nr = __NR_ioctl; goto native; // TODO: can something strange happen?
    case 17: nr = __NR_pread64; goto native;
    case 18: nr = __NR_pwrite64; goto native;
    case 19: nr = __NR_readv; goto native;
    case 20: nr = __NR_writev; goto native;
#ifdef __x86_64__
    case 23: nr = __NR_select; goto native;
#endif
    case 24: nr = __NR_sched_yield; goto native;
    case 25: nr = __NR_mremap; goto native;
    case 28: nr = __NR_madvise; goto native; // TODO: catch dangerous maps
    case 32: nr = __NR_dup; goto native;
    case 39: nr = __NR_getpid; goto native;
    case 41: nr = __NR_socket; goto native;
    case 42: nr = __NR_connect; goto native;
    // case 58: nr = __NR_fork; goto native; // Treat vfork as fork.
    case 59: nr = __NR_execve; goto native; // TODO: maybe check executable arch
    case 61: nr = __NR_wait4; goto native;
    case 63: nr = __NR_uname; goto native;
    case 76: nr = __NR_truncate; goto native;
    case 77: nr = __NR_ftruncate; goto native;
    // case 78: nr = __NR_getdents; goto native; // Needs mapping to getdents64
    case 79: nr = __NR_getcwd; goto native;
    case 80: nr = __NR_chdir; goto native;
    case 81: nr = __NR_fchdir; goto native;
    case 91: nr = __NR_fchmod; goto native;
    case 96: nr = __NR_gettimeofday; goto native;
    case 97: nr = __NR_getrlimit; goto native;
    case 99: nr = __NR_sysinfo; goto native;
    case 102: nr = __NR_getuid; goto native;
    case 104: nr = __NR_getgid; goto native;
    case 107: nr = __NR_geteuid; goto native;
    case 108: nr = __NR_getegid; goto native;
    case 110: nr = __NR_getppid; goto native;
    case 115: nr = __NR_getgroups; goto native;
    case 116: nr = __NR_setgroups; goto native;
    case 121: nr = __NR_getpgid; goto native;
    case 137: nr = __NR_statfs; goto native; // FIXME: handle buffer argument
    case 161: nr = __NR_chroot; goto native;
    case 186: nr = __NR_gettid; goto native;
    case 191: nr = __NR_getxattr; goto native;
    case 192: nr = __NR_lgetxattr; goto native;
    case 193: nr = __NR_fgetxattr; goto native;
    case 202: nr = __NR_futex; goto native;
    case 217: nr = __NR_getdents64; goto native;
    case 218: nr = __NR_set_tid_address; goto native;
    case 221: nr = __NR_fadvise64; goto native;
    case 228: nr = __NR_clock_gettime; goto native;
    case 229: nr = __NR_clock_getres; goto native;
    case 230: nr = __NR_clock_nanosleep; goto native;
    case 257: goto common_openat;
    case 260: nr = __NR_fchownat; goto native;
    case 268: nr = __NR_fchmodat; goto native;
    case 270: nr = __NR_pselect6; goto native;
    case 271: nr = __NR_ppoll; goto native;
    case 273: nr = __NR_set_robust_list; goto native;
    case 274: nr = __NR_get_robust_list; goto native;
    case 292: nr = __NR_dup3; goto native;
    case 293: nr = __NR_pipe2; goto native;
    case 302: nr = __NR_prlimit64; goto native;
    case 318: nr = __NR_getrandom; goto native;

    // Some are too old to work on newer platforms, but have replacements.
    case 2: // open
        arg3 = arg2, arg2 = arg1, arg1 = arg0, arg0 = AT_FDCWD;
    common_openat:
#ifdef __aarch64__
        arg2 = emulate_openat_flags(arg2);
#endif
        nr = __NR_openat;
        goto native;
    case 21: // access
        res = syscall(__NR_faccessat, AT_FDCWD, arg0, arg1, 0, 0, 0);
        break;
    case 22: // pipe
        res = syscall(__NR_pipe2, arg0, 0, 0, 0, 0, 0);
        break;
    case 82: // rename
        res = syscall(__NR_renameat, AT_FDCWD, arg0, AT_FDCWD, arg1, 0, 0);
        break;
    case 83: // mkdir
        res = syscall(__NR_mkdirat, AT_FDCWD, arg0, arg1, 0, 0, 0);
        break;
    case 84: // rmdir
        res = syscall(__NR_unlinkat, AT_FDCWD, arg0, AT_REMOVEDIR, 0, 0, 0);
        break;
    case 87: // unlink
        res = syscall(__NR_unlinkat, AT_FDCWD, arg0, 0, 0, 0, 0);
        break;
    case 89: // readlink
        res = syscall(__NR_readlinkat, AT_FDCWD, arg0, arg1, arg2, 0, 0);
        break;
    case 90: // chmod
        res = syscall(__NR_fchmodat, AT_FDCWD, arg0, arg1, 0, 0, 0);
        break;
    case 92: // chown
        res = syscall(__NR_fchownat, AT_FDCWD, arg0, arg1, arg2, 0, 0);
        break;
    case 94: // lchown
        res = syscall(__NR_fchownat, AT_FDCWD, arg0, arg1, arg2,
                      AT_SYMLINK_NOFOLLOW, 0);
        break;
    case 111: // getpgrp
        res = syscall(__NR_getpgid, 0, 0, 0, 0, 0, 0);
        break;

    // And some syscalls need special handling, e.g. to different structs.
    case 262: { // newfstatat
        uintptr_t tmp_addr = (uintptr_t) &tmp_struct;
        res = syscall(__NR_newfstatat, arg0, arg1, tmp_addr, arg3, 0, 0);
        arg1 = arg2;
        goto fstat_common;
    }
    case 4: // stat
    case 5: // fstat
    case 6: { // lstat
        uintptr_t tmp_addr = (uintptr_t) &tmp_struct;
        if (nr == 4) // stat
            res = syscall(__NR_newfstatat, AT_FDCWD, arg0, tmp_addr, 0, 0, 0);
        else if (nr == 5) // fstat
            res = syscall(__NR_fstat, arg0, tmp_addr, 0, 0, 0, 0);
        else if (nr == 6) // lstat
            res = syscall(__NR_newfstatat, AT_FDCWD, arg0, tmp_addr, AT_SYMLINK_NOFOLLOW, 0, 0);
    fstat_common:
        if (res == 0) {
            struct {
                unsigned long           st_dev;
                unsigned long           st_ino;
                unsigned long           st_nlink;

                unsigned int            st_mode;
                unsigned int            st_uid;
                unsigned int            st_gid;
                unsigned int            __pad0;
                unsigned long           st_rdev;
                long                    st_size;
                long                    st_blksize;
                long                    st_blocks;

                unsigned long           st_atime;
                unsigned long           st_atime_nsec;
                unsigned long           st_mtime;
                unsigned long           st_mtime_nsec;
                unsigned long           st_ctime;
                unsigned long           st_ctime_nsec;
                long                    __unused[3];
            } __attribute__((packed))* tgt = (void*) arg1;
            tgt->st_dev = tmp_struct.st_dev;
            tgt->st_ino = tmp_struct.st_ino;
            tgt->st_nlink = tmp_struct.st_nlink;
            tgt->st_mode = tmp_struct.st_mode;
            tgt->st_uid = tmp_struct.st_uid;
            tgt->st_gid = tmp_struct.st_gid;
            tgt->st_rdev = tmp_struct.st_rdev;
            tgt->st_size = tmp_struct.st_size;
            tgt->st_blksize = tmp_struct.st_blksize;
            tgt->st_blocks = tmp_struct.st_blocks;
            tgt->st_atime = tmp_struct.st_atime;
            tgt->st_atime_nsec = tmp_struct.st_atime_nsec;
            tgt->st_mtime = tmp_struct.st_mtime;
            tgt->st_mtime_nsec = tmp_struct.st_mtime_nsec;
            tgt->st_ctime = tmp_struct.st_ctime;
            tgt->st_ctime_nsec = tmp_struct.st_ctime_nsec;
        }
        break;
    }
    case 7: // poll
        res = syscall(__NR_ppoll, arg0, arg1, (long) arg2 >= 0 ?
                      (uintptr_t) &((struct timespec) { arg2/1000, arg2%1000*1000000 }) : 0,
                      0, _NSIG/8, 0);
        break;
    case 33: // dup2
        if (arg0 == arg1) { // If oldfd == newfd return EBADF if oldfd is invalid
            res = syscall(__NR_fcntl, arg0, F_GETFL, 0, 0, 0, 0);
            if (res >= 0)
                res = arg1;
            goto end;
        }
        res = syscall(__NR_dup3, arg0, arg1, 0, 0, 0, 0);
        break;
    case 34: // pause (wait for signal)
        res = syscall(__NR_ppoll, 0, 0, 0, 0, 0, 0);
        break;
    case 56: { // clone
        struct clone_args args = {
            .flags = arg0 & ~0xfful,
            .child_tid = arg3,
            .parent_tid = arg2,
            .exit_signal = arg0 & 0xff,
            .stack = arg1,
            .tls = arg4,
        };
        if (arg0 & CLONE_PIDFD) {
            if (arg0 & CLONE_PARENT_SETTID) {
                res = -EINVAL;
                break;
            }
            args.pidfd = args.parent_tid;
            args.parent_tid = 0;
        }
        res = handle_clone(state, &args, sizeof(args));
        break;
    }
    case 72: // fcntl
        switch (arg0) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_GETFD:
        case F_SETFD:
        case F_GETFL:
        case F_SETFL:
        case F_SETOWN:
        case F_SETLK: // note: uses struct flock*
        case F_SETLKW: // note: uses struct flock*
        case F_GETLK: // note: uses struct flock*
            nr = __NR_fcntl;
            goto native;
        default: goto unhandled;
        }

    case 157: // prctl
        switch (arg0) {
        default:
            res = -EINVAL;
            break;
        }
        break;
    case 158: // arch_prctl
        switch (arg0) {
        default:
            res = -EINVAL;
            break;
        case 0x1001: // ARCH_SET_GS
            cpu_regs[19] = arg1;
            res = 0;
            break;
        case 0x1002: // ARCH_SET_FS
            cpu_regs[18] = arg1;
            res = 0;
            break;
        }
        break;

    case 201: { // time
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME_COARSE, &ts);
        if (arg0)
            *(long*) arg0 = ts.tv_sec;
        res = ts.tv_sec;
        break;
    }

    case 60:  // exit (single thread exit — used by guest threads)
        nr = __NR_exit;
        goto native;

    case 231: {
        if (UNLIKELY(state->tc.tc_profile)) {
            dprintf(2, "Rewriting %u bytes took %u ms\n",
                    (uint32_t) state->translator.written_bytes,
                    (uint32_t) (state->rew_time / 1000000));
        }
        nr = __NR_exit_group;
        goto native;
    }

    case 435: { // clone3 — newer clone interface using struct clone_args
        res = handle_clone(state, (struct clone_args*) arg0, arg1);
        break;
    }

    // Some syscalls aren't implemented, but ok to ignore.
    case 203: // sched_setaffinity
    case 204: // sched_getaffinity
    case 334: // rseq
        res = -ENOSYS;
        break;

    // Finally, signal handling is a mess and requires a lot more effort.
    case 13: // rt_sigaction
        if (arg3 != sizeof(sigset_t))
            res = -EINVAL;
        else
            res = signal_sigaction(cpu_state, arg0, (void*) arg1, (void*) arg2);
        break;
    case 14: // rt_sigprocmask
        if (arg3 != sizeof(sigset_t))
            res = -EINVAL;
        else
            res = signal_sigprocmask(cpu_state, arg0, (void*) arg1, (void*) arg2);
        break;
    case 15: // rt_sigreturn
        signal_sigreturn(cpu_state);
        return; // Note -- we don't have a result here.
    case 127: // rt_sigpending
        if (arg1 != sizeof(sigset_t))
            res = -EINVAL;
        else
            res = syscall(__NR_rt_sigpending, arg0, arg1, 0, 0, 0, 0);
        break;
    // case 128: // rt_sigtimedwait
    // case 129: // rt_sigqueueinfo
    case 130: // rt_sigsuspend
        if (arg1 != sizeof(sigset_t))
            res = -EINVAL;
        else
            res = syscall(__NR_rt_sigsuspend, arg0, arg1, 0, 0, 0, 0);
        break;
    case 131: // sigaltstack
        res = signal_sigaltstack(cpu_state, (void*) arg0, (void*) arg1);
        break;
    }

end:
    // dprintf(2, "syscall %u (%lx %lx %lx %lx %lx %lx) = 0x%lx(%u)\n",
    //         nr, arg0, arg1, arg2, arg3, arg4, arg5, res, res);

    cpu_regs[1] = res;

    if (cpu_state->sigpending)
        signal_handle(cpu_state);
}

static bool
emulate_syscall_generic(struct CpuState* cpu_state, uint64_t* resp, uint64_t nr,
                        uint64_t arg0, uint64_t arg1, uint64_t arg2,
                        uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    ssize_t res = -ENOSYS;

    switch (nr) {
        struct stat tmp_struct;

    native:
        res = syscall(nr, arg0, arg1, arg2, arg3, arg4, arg5);
        break;

    default:
    unhandled:
        res = unhandled_syscall(nr, arg0, arg1, arg2, arg3, arg4, arg5);
        break;

    case 17: nr = __NR_getcwd; goto native;
    case 23: nr = __NR_dup; goto native;
    case 24: nr = __NR_dup3; goto native;
    case 25: // fcntl
        switch (arg0) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_GETFD:
        case F_SETFD:
        case F_GETFL:
        case F_SETFL:
        case F_SETOWN:
        case F_SETLK: // note: uses struct flock*
        case F_SETLKW: // note: uses struct flock*
        case F_GETLK: // note: uses struct flock*
            nr = __NR_fcntl;
            goto native;
        default: goto unhandled;
        }
    case 29: nr = __NR_ioctl; goto native; // TODO: catch dangerous commands
    case 35: nr = __NR_unlinkat; goto native;
    case 38: nr = __NR_renameat; goto native;
    case 46: nr = __NR_ftruncate; goto native;
    case 48: nr = __NR_faccessat; goto native;
    case 49: nr = __NR_chdir; goto native;
    case 50: nr = __NR_fchdir; goto native;
    case 51: nr = __NR_chroot; goto native;
    case 52: nr = __NR_fchmod; goto native;
    case 53: nr = __NR_fchmodat; goto native;
    case 54: nr = __NR_fchownat; goto native;
    case 55: nr = __NR_fchown; goto native;
    case 56:
        nr = __NR_openat;
#ifdef __aarch64__
        arg2 = emulate_openat_flags(arg2);
#endif
        goto native;
    case 57: nr = __NR_close; goto native;
    case 59: nr = __NR_pipe2; goto native;
    case 61: nr = __NR_getdents64; goto native;
    case 62: nr = __NR_lseek; goto native;
    case 63: nr = __NR_read; goto native;
    case 64: nr = __NR_write; goto native;
    case 65: nr = __NR_readv; goto native;
    case 66: nr = __NR_writev; goto native;
    case 67: nr = __NR_pread64; goto native;
    case 68: nr = __NR_pwrite64; goto native;
    case 72: nr = __NR_pselect6; goto native;
    case 73: nr = __NR_ppoll; goto native;
    case 78: nr = __NR_readlinkat; goto native;
    case 80: // fstat
        res = syscall(__NR_fstat, arg0, (uintptr_t) &tmp_struct, 0, 0, 0, 0);
        arg2 = arg1;
        goto fstat_common;
    case 79:; // fstatat
        uintptr_t tmp_addr = (uintptr_t) &tmp_struct;
        res = syscall(__NR_newfstatat, arg0, arg1, tmp_addr, arg3, 0, 0);
    fstat_common:
        if (res == 0) {
            struct {
                unsigned long           st_dev;
                unsigned long           st_ino;
                unsigned int            st_mode;
                unsigned int            st_nlink;
                unsigned int            st_uid;
                unsigned int            st_gid;
                unsigned long           st_rdev;
                long                    __pad0;
                long                    st_size;
                int                     st_blksize;
                int                     __pad1;
                long                    st_blocks;
                unsigned long           st_atime;
                unsigned long           st_atime_nsec;
                unsigned long           st_mtime;
                unsigned long           st_mtime_nsec;
                unsigned long           st_ctime;
                unsigned long           st_ctime_nsec;
                int                     __unused[2];
            } __attribute__((packed))* tgt = (void*) arg2;
            tgt->st_dev = tmp_struct.st_dev;
            tgt->st_ino = tmp_struct.st_ino;
            tgt->st_nlink = tmp_struct.st_nlink;
            tgt->st_mode = tmp_struct.st_mode;
            tgt->st_uid = tmp_struct.st_uid;
            tgt->st_gid = tmp_struct.st_gid;
            tgt->st_rdev = tmp_struct.st_rdev;
            tgt->st_size = tmp_struct.st_size;
            tgt->st_blksize = tmp_struct.st_blksize;
            tgt->st_blocks = tmp_struct.st_blocks;
            tgt->st_atime = tmp_struct.st_atime;
            tgt->st_atime_nsec = tmp_struct.st_atime_nsec;
            tgt->st_mtime = tmp_struct.st_mtime;
            tgt->st_mtime_nsec = tmp_struct.st_mtime_nsec;
            tgt->st_ctime = tmp_struct.st_ctime;
            tgt->st_ctime_nsec = tmp_struct.st_ctime_nsec;
        }
        break;

    case 93: nr = __NR_exit; goto native;
    case 94: nr = __NR_exit_group; goto native;
    case 96: nr = __NR_set_tid_address; goto native;
    case 98: nr = __NR_futex; goto native;
    case 99: nr = __NR_set_robust_list; goto native;
    case 100: nr = __NR_get_robust_list; goto native;
    case 113: nr = __NR_clock_gettime; goto native;
    case 114: nr = __NR_clock_getres; goto native;
    case 115: nr = __NR_clock_nanosleep; goto native;
    case 124: nr = __NR_sched_yield; goto native;
    case 131: nr = __NR_tgkill; goto native;
    case 155: nr = __NR_getpgid; goto native;
    case 160:
        res = syscall(__NR_uname, arg0, 0, 0, 0, 0, 0);
        if (res == 0) {
            // Emulate kernel 5.0.0 -- glibc checks kernel versions.
            struct new_utsname* buf = (void*) arg0;
            if (buf->release[0] <= '4' && buf->release[1] == '.')
                memcpy(buf->release, "5.0.0", sizeof "5.0.0");
        }
        break;
    case 169: nr = __NR_gettimeofday; goto native;
    case 172: nr = __NR_getpid; goto native;
    case 173: nr = __NR_getppid; goto native;
    case 174: nr = __NR_getuid; goto native;
    case 175: nr = __NR_geteuid; goto native;
    case 176: nr = __NR_getgid; goto native;
    case 177: nr = __NR_getegid; goto native;
    case 178: nr = __NR_gettid; goto native;
    case 179: nr = __NR_sysinfo; goto native;
    case 214: nr = __NR_brk; goto native;
    case 215: nr = __NR_munmap; goto native;
    case 216: nr = __NR_mremap; goto native;
    case 220: { // clone
        struct clone_args args = {
            .flags = arg0 & ~0xfful,
            .child_tid = arg4,
            .parent_tid = arg2,
            .exit_signal = arg0 & 0xff,
            .stack = arg1,
            .tls = arg3,
        };
        if (arg0 & CLONE_PIDFD) {
            if (arg0 & CLONE_PARENT_SETTID) {
                res = -EINVAL;
                break;
            }
            args.pidfd = args.parent_tid;
            args.parent_tid = 0;
        }
        res = handle_clone(cpu_state->state, &args, sizeof(args));
        break;
    }
    case 221: nr = __NR_execve; goto native; // TODO: maybe check executable arch
    case 222: nr = __NR_mmap; goto native;
    case 223: nr = __NR_fadvise64; goto native;
    case 226: nr = __NR_mprotect; goto native;
    case 233: nr = __NR_madvise; goto native;
    case 260: nr = __NR_wait4; goto native;
    case 261: nr = __NR_prlimit64; goto native;
    case 276: nr = __NR_renameat2; goto native;
    case 278: nr = __NR_getrandom; goto native;

    // Some syscalls aren't implemented, but ok to ignore.
    case 122: // sched_setaffinity
    case 123: // sched_getaffinity
    case 293: // rseq
        res = -ENOSYS;
        break;

    // Finally, signal handling is a mess and requires a lot more effort.
    case 134: // rt_sigaction
        if (arg3 != sizeof(sigset_t))
            res = -EINVAL;
        else
            res = signal_sigaction(cpu_state, arg0, (void*) arg1, (void*) arg2);
        break;
    case 135: // rt_sigprocmask
        if (arg3 != sizeof(sigset_t))
            res = -EINVAL;
        else
            res = signal_sigprocmask(cpu_state, arg0, (void*) arg1, (void*) arg2);
        break;
    case 139: // rt_sigreturn
        signal_sigreturn(cpu_state);
        return false; // Note -- we don't have a result here.
    case 136: // rt_sigpending
        if (arg1 != sizeof(sigset_t))
            res = -EINVAL;
        else
            res = syscall(__NR_rt_sigpending, arg0, arg1, 0, 0, 0, 0);
        break;
    // case 137: // rt_sigtimedwait
    // case 138: // rt_sigqueueinfo
    case 133: // rt_sigsuspend
        if (arg1 != sizeof(sigset_t))
            res = -EINVAL;
        else
            res = syscall(__NR_rt_sigsuspend, arg0, arg1, 0, 0, 0, 0);
        break;
    case 132: // sigaltstack
        res = signal_sigaltstack(cpu_state, (void*) arg0, (void*) arg1);
        break;
    }

    *resp = res;
    return true;
}

void emulate_rv64_syscall(uint64_t* cpu_regs);

void
emulate_rv64_syscall(uint64_t* cpu_regs) {
    struct CpuState* cpu_state = CPU_STATE_FROM_REGS(cpu_regs);
    uint64_t a0 = cpu_regs[11], a1 = cpu_regs[12], a2 = cpu_regs[13],
             a3 = cpu_regs[14], a4 = cpu_regs[15], a5 = cpu_regs[16];
    uint64_t nr = cpu_regs[18]; // a7/x17
    bool normal_cont = emulate_syscall_generic(cpu_state, &cpu_regs[11], nr,
                                               a0, a1, a2, a3, a4, a5);
    // TODO: support non-normal continuations (i.e., sigreturn)
    if (normal_cont && cpu_state->sigpending)
        signal_handle(cpu_state);
}

void emulate_aarch64_syscall(uint64_t* cpu_regs);

void
emulate_aarch64_syscall(uint64_t* cpu_regs) {
    struct CpuState* cpu_state = CPU_STATE_FROM_REGS(cpu_regs);
    uint64_t a0 = cpu_regs[2], a1 = cpu_regs[3], a2 = cpu_regs[4],
             a3 = cpu_regs[5], a4 = cpu_regs[6], a5 = cpu_regs[7];
    uint64_t nr = cpu_regs[10]; // x8
    bool normal_cont = true;

    switch (nr) {
    default:
    passthrough:
        normal_cont = emulate_syscall_generic(cpu_state, &cpu_regs[2], nr,
                                              a0, a1, a2, a3, a4, a5);
        break;

    case 56: {// openat on AArch64 has some swapped flags
        // See Linux arch/arm64/include/uapi/asm/fcntl.h
        uint64_t new_a2 = a2 & ~00740000;
        if (a2 & 00040000) // O_DIRECTORY
            new_a2 |= 00200000;
        if (a2 & 00100000) // O_NOFOLLOW
            new_a2 |= 00400000;
        if (a2 & 00200000) // O_DIRECT
            new_a2 |= 00040000;
        if (a2 & 00400000) // O_LARGEFILE
            new_a2 |= 00100000;
        a2 = new_a2;
        goto passthrough;
    }
    }
    // TODO: support non-normal continuations (i.e., sigreturn)
    if (normal_cont && cpu_state->sigpending)
        signal_handle(cpu_state);
}
