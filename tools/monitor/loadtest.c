/* loadtest.c: one thread's worth of steady floating-point work (AVX2 fused multiply-adds held in registers), for a
 * given number of seconds; prints the operations it did. Run one per thread to load the whole machine. */
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
int main(int argc, char **argv) {
  double seconds = argc > 1 ? atof(argv[1]) : 10, t0 = now();
  __m256d a = _mm256_set1_pd(0.9999999), b = _mm256_set1_pd(1e-7), x[10];
  for (int k = 0; k < 10; k++) x[k] = _mm256_set1_pd(1.0 + 0.01 * k);
  long rounds = 0;
  while (now() - t0 < seconds) {
    for (int i = 0; i < 2000000; i++)
      for (int k = 0; k < 10; k++) x[k] = _mm256_fmadd_pd(x[k], a, b);
    rounds++;
  }
  double s = 0, v[4];
  for (int k = 0; k < 10; k++) { _mm256_storeu_pd(v, x[k]); s += v[0] + v[1] + v[2] + v[3]; }
  printf("%.0f operations in %.1f s (check %.6f)\n", rounds * 2000000.0 * 10 * 8, now() - t0, s);
  return 0;
}
