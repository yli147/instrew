/* STREAM memory bandwidth benchmark — single-threaded, no OpenMP.
 * Compile: gcc -static -O3 stream.c -o stream -lm
 * Usage:   ./stream                    (native)
 *          instrew ./stream_x86        (via Instrew on RISC-V host)
 */
#include <stdio.h>
#include <time.h>

#define STREAM_ARRAY_SIZE 10000000
#define NTIMES            10

static double a[STREAM_ARRAY_SIZE],
              b[STREAM_ARRAY_SIZE],
              c[STREAM_ARRAY_SIZE];

static double gettime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void)
{
    int j, k;
    double scalar = 3.0, t;
    double times[4][NTIMES];
    double avgtime[4] = {0}, maxtime[4] = {0};
    double mintime[4] = {1e36, 1e36, 1e36, 1e36};

    printf("STREAM (single-threaded)\n");
    printf("Array size : %d elements (%.1f MB each, %.1f MB total)\n",
           STREAM_ARRAY_SIZE,
           sizeof(double) * STREAM_ARRAY_SIZE / 1048576.0,
           3.0 * sizeof(double) * STREAM_ARRAY_SIZE / 1048576.0);
    printf("Iterations : %d\n\n", NTIMES);

    for (j = 0; j < STREAM_ARRAY_SIZE; j++) { a[j]=1.0; b[j]=2.0; c[j]=0.0; }

    for (k = 0; k < NTIMES; k++) {
        t = gettime(); for (j=0;j<STREAM_ARRAY_SIZE;j++) c[j]=a[j];
        times[0][k] = gettime()-t;
        t = gettime(); for (j=0;j<STREAM_ARRAY_SIZE;j++) b[j]=scalar*c[j];
        times[1][k] = gettime()-t;
        t = gettime(); for (j=0;j<STREAM_ARRAY_SIZE;j++) c[j]=a[j]+b[j];
        times[2][k] = gettime()-t;
        t = gettime(); for (j=0;j<STREAM_ARRAY_SIZE;j++) a[j]=b[j]+scalar*c[j];
        times[3][k] = gettime()-t;
    }

    const char   *label[4] = {"Copy","Scale","Add","Triad"};
    const double  bytes[4] = {
        2.0*sizeof(double)*STREAM_ARRAY_SIZE,
        2.0*sizeof(double)*STREAM_ARRAY_SIZE,
        3.0*sizeof(double)*STREAM_ARRAY_SIZE,
        3.0*sizeof(double)*STREAM_ARRAY_SIZE,
    };

    printf("%-10s  %12s  %11s  %11s  %11s\n",
           "Function","Best MB/s","Avg time","Min time","Max time");
    printf("----------------------------------------------------------\n");
    for (int i = 0; i < 4; i++) {
        for (k = 1; k < NTIMES; k++) {
            if (times[i][k] < mintime[i]) mintime[i] = times[i][k];
            if (times[i][k] > maxtime[i]) maxtime[i] = times[i][k];
            avgtime[i] += times[i][k];
        }
        avgtime[i] /= (NTIMES-1);
        printf("%-10s  %12.1f  %11.6f  %11.6f  %11.6f\n",
               label[i], bytes[i]/mintime[i]/1e6,
               avgtime[i], mintime[i], maxtime[i]);
    }
    printf("----------------------------------------------------------\n");
    return 0;
}
