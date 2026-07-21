#define _GNU_SOURCE
#include <stdint.h>
#include <immintrin.h>
#include <stdio.h>

int main(void) {
    int pass = 1;

    /* Test 1: VADDPS - add two vectors of 16 floats */
    {
        __m512 a = _mm512_set_ps(16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1);
        __m512 b = _mm512_set_ps(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16);
        __m512 c = _mm512_add_ps(a, b);
        float result[16];
        _mm512_store_ps(result, c);
        for (int i = 0; i < 16; i++) {
            if (result[i] != 17.0f) {
                printf("FAIL VADDPS[%d] = %f (expected 17.0)\n", i, result[i]);
                pass = 0;
            }
        }
    }

    /* Test 2: VSUBPS */
    {
        __m512 a = _mm512_set1_ps(100.0f);
        __m512 b = _mm512_set1_ps(37.0f);
        __m512 c = _mm512_sub_ps(a, b);
        float result[16];
        _mm512_store_ps(result, c);
        for (int i = 0; i < 16; i++) {
            if (result[i] != 63.0f) {
                printf("FAIL VSUBPS[%d] = %f (expected 63.0)\n", i, result[i]);
                pass = 0;
            }
        }
    }

    /* Test 3: VMULPS */
    {
        __m512 a = _mm512_set1_ps(3.0f);
        __m512 b = _mm512_set1_ps(7.0f);
        __m512 c = _mm512_mul_ps(a, b);
        float result[16];
        _mm512_store_ps(result, c);
        for (int i = 0; i < 16; i++) {
            if (result[i] != 21.0f) {
                printf("FAIL VMULPS[%d] = %f (expected 21.0)\n", i, result[i]);
                pass = 0;
            }
        }
    }

    /* Test 4: VDIVPS */
    {
        __m512 a = _mm512_set1_ps(100.0f);
        __m512 b = _mm512_set1_ps(4.0f);
        __m512 c = _mm512_div_ps(a, b);
        float result[16];
        _mm512_store_ps(result, c);
        for (int i = 0; i < 16; i++) {
            if (result[i] != 25.0f) {
                printf("FAIL VDIVPS[%d] = %f (expected 25.0)\n", i, result[i]);
                pass = 0;
            }
        }
    }

    /* Test 5: VPBROADCASTD via set1 (broadcasts scalar to all lanes) */
    {
        __m512i b = _mm512_set1_epi32(42);
        int32_t result[16];
        _mm512_store_si512(result, b);
        for (int i = 0; i < 16; i++) {
            if (result[i] != 42) {
                printf("FAIL VPBROADCASTD[%d] = %d (expected 42)\n", i, result[i]);
                pass = 0;
            }
        }
    }

    /* Test 6: VPADDD */
    {
        __m512i a = _mm512_set1_epi32(1000);
        __m512i b = _mm512_set1_epi32(2000);
        __m512i c = _mm512_add_epi32(a, b);
        int32_t result[16];
        _mm512_store_si512(result, c);
        for (int i = 0; i < 16; i++) {
            if (result[i] != 3000) {
                printf("FAIL VPADDD[%d] = %d (expected 3000)\n", i, result[i]);
                pass = 0;
            }
        }
    }

    /* Test 7: VPSUBD */
    {
        __m512i a = _mm512_set1_epi32(5000);
        __m512i b = _mm512_set1_epi32(3000);
        __m512i c = _mm512_sub_epi32(a, b);
        int32_t result[16];
        _mm512_store_si512(result, c);
        for (int i = 0; i < 16; i++) {
            if (result[i] != 2000) {
                printf("FAIL VPSUBD[%d] = %d (expected 2000)\n", i, result[i]);
                pass = 0;
            }
        }
    }

    /* Test 8: VPBROADCASTQ */
    {
        __m512i b = _mm512_set1_epi64(0xDEADBEEFULL);
        int64_t result[8];
        _mm512_store_si512(result, b);
        for (int i = 0; i < 8; i++) {
            if (result[i] != (int64_t)0xDEADBEEFULL) {
                printf("FAIL VPBROADCASTQ[%d] = %lx (expected DEADBEEF)\n", i, (unsigned long)result[i]);
                pass = 0;
            }
        }
    }

    if (pass) {
        printf("ALL AVX-512 TESTS PASSED\n");
    } else {
        printf("SOME TESTS FAILED\n");
    }
    return pass ? 0 : 1;
}
