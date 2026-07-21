/*
 * AVX-512 correctness test for instrew on RISC-V
 * Tests various EVEX-encoded instructions
 * Compile with: x86_64-gcc -O2 -mavx512f -nostdlib -o test_avx512 test_avx512_correctness.c
 */

#include <stdint.h>

/* Minimal syscall wrappers */
static void write_stdout(const char *s, int len) {
    asm volatile("mov $1, %%rax; mov $1, %%rdi; syscall"
                 : : "S"(s), "d"(len) : "rax", "rcx", "r11", "memory");
}

static void exit_prog(int code) {
    asm volatile("mov $60, %%rax; syscall"
                 : : "D"(code) : "rax", "rcx", "r11");
}

static void putch(char c) {
    asm volatile("mov $1, %%rax; mov $1, %%rdi; mov $1, %%rdx; syscall"
                 : : "S"(&c) : "rax", "rcx", "r11");
}

static void putstr(const char *s) {
    int i = 0;
    while (s[i]) i++;
    write_stdout(s, i);
}

static void putint(int n) {
    char buf[12];
    int i = 0;
    if (n < 0) { putch('-'); n = -n; }
    int tmp = n;
    while (tmp > 0) { buf[i++] = '0' + (tmp % 10); tmp /= 10; }
    if (i == 0) buf[i++] = '0';
    for (int j = i-1; j >= 0; j--) putch(buf[j]);
}

static void puthex(uint32_t n) {
    const char *hex = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4) {
        putch(hex[(n >> i) & 0xF]);
    }
}

int failed = 0;
int fail_test = 0;

#define CHECK(cond, testnum) do { \
    if (!(cond)) { failed = 1; fail_test = (testnum); } \
} while(0)

/* AVX-512 test functions using intrinsics */

/* Test 1: vaddps ZMM (EVEX non-0F-map) */
static void test_vaddps(void) {
    __m512 a = _mm512_set_ps(16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1);
    __m512 b = _mm512_set_ps(16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1);
    __m512 c = _mm512_add_ps(a, b);
    float result[16];
    _mm512_storeu_ps(result, c);
    /* Each element should be 2x the original: 2,4,6,...,32 */
    for (int i = 0; i < 16; i++) {
        float expected = (i + 1) * 2.0f;
        if (result[i] != expected) {
            failed = 1; fail_test = 1;
            return;
        }
    }
}

/* Test 2: vmulps ZMM */
static void test_vmulps(void) {
    __m512 a = _mm512_set1_ps(3.0f);
    __m512 b = _mm512_set1_ps(7.0f);
    __m512 c = _mm512_mul_ps(a, b);
    float result[16];
    _mm512_storeu_ps(result, c);
    for (int i = 0; i < 16; i++) {
        if (result[i] != 21.0f) {
            failed = 1; fail_test = 2;
            return;
        }
    }
}

/* Test 3: vsubps ZMM */
static void test_vsubps(void) {
    __m512 a = _mm512_set1_ps(100.0f);
    __m512 b = _mm512_set1_ps(37.0f);
    __m512 c = _mm512_sub_ps(a, b);
    float result[16];
    _mm512_storeu_ps(result, c);
    for (int i = 0; i < 16; i++) {
        if (result[i] != 63.0f) {
            failed = 1; fail_test = 3;
            return;
        }
    }
}

/* Test 4: vpaddb ZMM (byte-wise add) */
static void test_vpaddb(void) {
    __m512i a = _mm512_set1_epi8(10);
    __m512i b = _mm512_set1_epi8(25);
    __m512i c = _mm512_add_epi8(a, b);
    int8_t result[64];
    _mm512_storeu_si64(result, c);
    for (int i = 0; i < 64; i++) {
        if (result[i] != 35) {
            failed = 1; fail_test = 4;
            return;
        }
    }
}

/* Test 5: vcmpeqps ZMM (0F-map EVEX - the tricky one) */
static void test_vcmpeqps(void) {
    __m512 a = _mm512_set1_ps(42.0f);
    __m512 b = _mm512_set1_ps(42.0f);
    __mmask16 k = _mm512_cmpeq_ps_mask(a, b);
    /* All 16 elements equal, mask should be 0xFFFF */
    if (k != 0xFFFF) {
        failed = 1; fail_test = 5;
    }
}

/* Test 6: vpcmpeqd ZMM */
static void test_vpcmpeqd(void) {
    __m512i a = _mm512_set1_epi32(12345);
    __m512i b = _mm512_set1_epi32(12345);
    __mmask16 k = _mm512_cmpeq_epu32_mask(a, b);
    if (k != 0xFFFF) {
        failed = 1; fail_test = 6;
    }
}

/* Test 7: masked add (EVEX with mask) */
static void test_masked_add(void) {
    float src[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    float a[16] = {10,20,30,40,50,60,70,80,90,100,110,120,130,140,150,160};
    float b[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    float result[16];
    
    __m512 va = _mm512_loadu_ps(a);
    __m512 vb = _mm512_loadu_ps(b);
    __mmask16 mask = 0x5555; /* even elements active */
    __m512 vc = _mm512_mask_add_ps(va, mask, va, vb);
    _mm512_storeu_ps(result, vc);
    
    for (int i = 0; i < 16; i++) {
        if (i % 2 == 0) {
            /* Masked on: a[i] + b[i] */
            if (result[i] != a[i] + 1.0f) {
                failed = 1; fail_test = 7;
                return;
            }
        } else {
            /* Masked off: src[i] */
            if (result[i] != src[i]) {
                failed = 1; fail_test = 7;
                return;
            }
        }
    }
}

/* Test 8: vbroadcastss (memory broadcast) */
static void test_broadcastss(void) {
    float val = 99.0f;
    __m512 a = _mm512_broadcast_ss(&val);
    float result[16];
    _mm512_storeu_ps(result, a);
    for (int i = 0; i < 16; i++) {
        if (result[i] != 99.0f) {
            failed = 1; fail_test = 8;
            return;
        }
    }
}

/* Test 9: vpslld ZMM (dword shift left) */
static void test_vpslld(void) {
    __m512i a = _mm512_set1_epi32(0x00010001);
    __m512i b = _mm512_slli_epi32(a, 8);
    int32_t result[16];
    _mm512_storeu_si512(result, b);
    for (int i = 0; i < 16; i++) {
        if (result[i] != 0x00100010) {
            failed = 1; fail_test = 9;
            return;
        }
    }
}

/* Test 10: vzeroupper (should not crash) */
static void test_vzeroupper(void) {
    _mm512_zeroall();  /* or vzeroupper */
    /* Just check it doesn't crash */
}

void _start(void) {
    /* Align stack to 64 bytes for AVX-512 */
    asm volatile("and $-64, %rsp");
    
    putstr("AVX-512 Tests:\n");
    
    test_vaddps();
    putstr(failed ? "  Test 1 (vaddps ZMM): FAIL\n" : "  Test 1 (vaddps ZMM): OK\n");
    
    test_vmulps();
    putstr(failed ? "  Test 2 (vmulps ZMM): FAIL\n" : "  Test 2 (vmulps ZMM): OK\n");
    
    test_vsubps();
    putstr(failed ? "  Test 3 (vsubps ZMM): FAIL\n" : "  Test 3 (vsubps ZMM): OK\n");
    
    test_vpaddb();
    putstr(failed ? "  Test 4 (vpaddb ZMM): FAIL\n" : "  Test 4 (vpaddb ZMM): OK\n");
    
    test_vcmpeqps();
    putstr(failed ? "  Test 5 (vcmpeqps 0F-map): FAIL\n" : "  Test 5 (vcmpeqps 0F-map): OK\n");
    
    test_vpcmpeqd();
    putstr(failed ? "  Test 6 (vpcmpeqd): FAIL\n" : "  Test 6 (vpcmpeqd): OK\n");
    
    test_masked_add();
    putstr(failed ? "  Test 7 (masked add): FAIL\n" : "  Test 7 (masked add): OK\n");
    
    test_broadcastss();
    putstr(failed ? "  Test 8 (broadcastss): FAIL\n" : "  Test 8 (broadcastss): OK\n");
    
    test_vpslld();
    putstr(failed ? "  Test 9 (vpslld): FAIL\n" : "  Test 9 (vpslld): OK\n");
    
    test_vzeroupper();
    putstr(failed ? "  Test 10 (vzeroupper): FAIL\n" : "  Test 10 (vzeroupper): OK\n");
    
    if (failed) {
        putstr("FAILED at test ");
        putint(fail_test);
        putstr("\n");
        exit_prog(1);
    } else {
        putstr("ALL 10 TESTS PASSED\n");
        exit_prog(0);
    }
}
