#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main() {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    // Dynamic string building like an interpreted language would
    size_t cap = 256, len = 0;
    char* result = (char*)malloc(cap);
    result[0] = '\0';
    for (int i = 0; i < 10000; i++) {
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d", i);
        while (len + n + 1 > cap) { cap *= 2; result = (char*)realloc(result, cap); }
        memcpy(result + len, buf, n);
        len += n;
        result[len] = '\0';
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("%zu\n", len);
    printf("time: %.6fs\n", elapsed);
    free(result);
    return 0;
}
