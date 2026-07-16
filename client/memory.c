
#include <common.h>
#include <elf.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/unistd.h>

#include <memory.h>

#ifdef __riscv
#ifndef __NR_riscv_flush_icache
#define __NR_riscv_flush_icache 259
#endif
#endif


#define MEM_BASE ((void*) 0x0000400000000000ull)
#define MEM_CODE_SIZE 0x40000000
#define MEM_DATA_SIZE 0x08000000

typedef struct Arena Arena;
struct Arena {
    char* start;
    char* end;
    // brk/brkp accessed via __atomic_* builtins (pointer-width word).
    // brk: atomic bump pointer — claimed by CAS in arena_alloc.
    // brkp: high-water mark of OS-backed pages — advanced under mprotect_lock.
    char* brk;
    char* brkp;
    // 0 = unlocked, 1 = locked. Only held during mprotect (rare path).
    int mprotect_lock;
    bool exec;
};

static int
arena_init(Arena* arena, void* base, size_t size, bool exec) {
    void* mem = mmap(base, size, PROT_NONE,
                     MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (BAD_ADDR(mem))
        return (int) (uintptr_t) mem;

    arena->start = mem;
    arena->end = (char*) mem + size;
    __atomic_store_n(&arena->brk,  (char*) mem, __ATOMIC_RELAXED);
    __atomic_store_n(&arena->brkp, (char*) mem, __ATOMIC_RELAXED);
    __atomic_store_n(&arena->mprotect_lock, 0, __ATOMIC_RELAXED);
    arena->exec = exec;
    return 0;
}

// Slow path: make sure [result, result+size) is backed by mprotect.
// Called after the bump pointer has already reserved the space.
static void
arena_ensure_backed(Arena* arena, char* result, size_t size) {
    char* need_end = result + size;

    // Check if the region is already backed (fast path, no lock needed).
    if (need_end <= __atomic_load_n(&arena->brkp, __ATOMIC_ACQUIRE))
        return;

    // Slow path: take lock and extend brkp.
    int exp = 0;
    while (!__atomic_compare_exchange_n(&arena->mprotect_lock, &exp, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        exp = 0;

    // Re-check under lock: another thread may have extended brkp already.
    char* cur_brkp;
    __atomic_load(&arena->brkp, &cur_brkp, __ATOMIC_RELAXED);
    if (need_end > cur_brkp) {
        size_t newpgsz = ALIGN_UP((size_t)(need_end - cur_brkp), getpagesize());
        int prot = PROT_READ|PROT_WRITE | (arena->exec ? PROT_EXEC : 0);
        mprotect(cur_brkp, newpgsz, prot);
        char* new_brkp = cur_brkp + newpgsz;
        __atomic_store_n(&arena->brkp, new_brkp, __ATOMIC_RELEASE);
    }

    __atomic_store_n(&arena->mprotect_lock, 0, __ATOMIC_RELEASE);
}

static void*
arena_alloc(Arena* arena, size_t size, size_t alignment) {
    if (alignment < 0x40)
        alignment = 0x40;
    if (alignment & (alignment - 1))
        return (void*) (uintptr_t) -EINVAL;

    // Atomically bump brk to claim [result, result+size).
    // CAS loop: read current brk, align it, add size, CAS to new value.
    char* old_brk;
    char* new_brk;
    char* result;
    do {
        old_brk = __atomic_load_n(&arena->brk, __ATOMIC_RELAXED);
        result  = (char*) ALIGN_UP((uintptr_t) old_brk, alignment);
        new_brk = result + size;
        if (new_brk > arena->end)
            return (void*) (uintptr_t) -ENOMEM;
    } while (!__atomic_compare_exchange_n(&arena->brk, &old_brk, new_brk,
                                          /*weak=*/1,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));

    // Ensure the pages we claimed are backed by the OS.
    arena_ensure_backed(arena, result, size);
    return result;
}

Arena main_arena_code;
Arena main_arena_data;

int
mem_init(void) {
    int ret = arena_init(&main_arena_data, MEM_BASE, MEM_DATA_SIZE, /*exec=*/false);
    if (ret)
        return ret;
    void* code_arena_base = (void*) ((uintptr_t) MEM_BASE + MEM_DATA_SIZE);
    ret = arena_init(&main_arena_code, code_arena_base, MEM_CODE_SIZE, /*exec=*/true);
    if (ret)
        return ret;
    return 0;
}

void*
mem_alloc_data(size_t size, size_t alignment) {
    return arena_alloc(&main_arena_data, size, alignment);
}

void*
mem_alloc_code(size_t size, size_t alignment) {
    return arena_alloc(&main_arena_code, size, alignment);
}

int
mem_write_code(void* dst, const void* src, size_t size) {
    // Note: if W^X is enforced, the pages need to be mapped somewhere else for
    // writing (e.g., using memfd).
    memcpy(dst, src, size);

    // Flush ICache, except for x86-64.
#if defined(__x86_64__)
    // Do nothing; x86-64 flushes ICache automatically.
#elif defined(__aarch64__)
    uintptr_t dstu = (uintptr_t) dst;
    // Procedure from AArch64 Manual, B2.4.4
    uint64_t ctr_el0 = 0;
    __asm__("mrs %0, ctr_el0" : "=r"(ctr_el0));
    // Encoding of cache line sizes is log2(#words), one word is 4 bytes.
    size_t dc_line_sz = 4 << ((ctr_el0 >> 16) & 0xf); // DCache min line size
    size_t ic_line_sz = 4 << ((ctr_el0 >> 0) & 0xf); // ICache min line size

    if (!(ctr_el0 & (1 << 28))) { // IDC == 0 => DC invalidation required
        for (uintptr_t p = dstu & ~dc_line_sz; p < dstu + size; p += dc_line_sz)
            __asm__ volatile("dc cvau, %0" : : "r"(p));
        __asm__ volatile("dsb ish");
    }
    if (!(ctr_el0 & (1 << 29))) { // DIC == 0 => IC invalidation required
        for (uintptr_t p = dstu & ~ic_line_sz; p < dstu + size; p += ic_line_sz)
            __asm__ volatile("ic ivau, %0" : : "r"(p));
        __asm__ volatile("dsb ish");
    }
    __asm__ volatile("isb");
#elif defined(__riscv)
    // RISC-V requires icache flush via syscall on Linux.
    // Use SYS_riscv_flush_icache (syscall __NR_riscv_flush_icache).
    // Do not use fence.i directly - it is insufficient for user-space on Linux.
    // SYS_riscv_flush_icache is available since Linux 4.15.
    // syscall(int nr, uintptr_t start, uintptr_t end, uintptr_t flags)
    // start and end are pointers, flags should be 0 (FLUSH_ICACHE_LOCAL).
    {
        uintptr_t start = (uintptr_t) dst;
        uintptr_t end = start + size;
        register uintptr_t a0 __asm__("a0") = start;
        register uintptr_t a1 __asm__("a1") = end;
        register uintptr_t a2 __asm__("a2") = 0;
        register long a7 __asm__("a7") = __NR_riscv_flush_icache;
        __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    }
#else
#error "Implement ICache flush for unknown target"
#endif
    return 0;
}
