#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main() {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    long total = 0;
    for (int i = 0; i < 100000; i++) {
        int* arr = (int*)malloc(5 * sizeof(int));
        arr[0] = i; arr[1] = i+1; arr[2] = i+2; arr[3] = i+3; arr[4] = i+4;
        total += arr[2];
        free(arr);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("%ld\n", total);
    printf("time: %.6fs\n", elapsed);
    return 0;
}
