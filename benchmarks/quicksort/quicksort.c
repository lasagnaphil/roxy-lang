#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void swap(int *arr, int i, int j) {
    int temp = arr[i];
    arr[i] = arr[j];
    arr[j] = temp;
}

int partition(int *arr, int low, int high) {
    int pivot = arr[high];
    int i = low - 1;
    for (int j = low; j < high; j++) {
        if (arr[j] <= pivot) {
            i++;
            swap(arr, i, j);
        }
    }
    swap(arr, i + 1, high);
    return i + 1;
}

void quicksort(int *arr, int low, int high) {
    if (low < high) {
        int pi = partition(arr, low, high);
        quicksort(arr, low, pi - 1);
        quicksort(arr, pi + 1, high);
    }
}

int main(void) {
    int count = 100000;
    int *arr = (int *)malloc(count * sizeof(int));

    /* Generate pseudo-random numbers using Park-Miller LCG */
    long long seed = 12345;
    for (int i = 0; i < count; i++) {
        seed = (seed * 16807) % 2147483647;
        arr[i] = (int)(seed % 1000000);
    }

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    quicksort(arr, 0, count - 1);
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0
                      + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("Time: %.3f ms\n", elapsed_ms);

    /* Verification */
    printf("First: %d\n", arr[0]);
    printf("Last: %d\n", arr[count - 1]);

    long long checksum = 0;
    for (int i = 0; i < 10; i++) {
        checksum += arr[i];
    }
    printf("Checksum (sum of first 10): %lld\n", checksum);

    free(arr);
    return 0;
}
