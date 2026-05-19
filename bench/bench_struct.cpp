#include <stdio.h>
#include <time.h>

struct Vec2 {
    double x, y;
    double mag_sq() { return x*x + y*y; }
};

int main() {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    double total = 0.0;
    for (int i = 0; i < 50000; i++) {
        Vec2 v = {(double)i, (double)(i+1)};
        total += v.mag_sq();
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("%lld\n", (long long)total);
    printf("time: %.6fs\n", elapsed);
    return 0;
}
