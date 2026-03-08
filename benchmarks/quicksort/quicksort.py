import sys
import time

sys.setrecursionlimit(200000)

def swap(arr, i, j):
    arr[i], arr[j] = arr[j], arr[i]

def partition(arr, low, high):
    pivot = arr[high]
    i = low - 1
    for j in range(low, high):
        if arr[j] <= pivot:
            i += 1
            swap(arr, i, j)
    swap(arr, i + 1, high)
    return i + 1

def quicksort(arr, low, high):
    if low < high:
        pi = partition(arr, low, high)
        quicksort(arr, low, pi - 1)
        quicksort(arr, pi + 1, high)

def main():
    count = 100000
    arr = []

    # Generate pseudo-random numbers using Park-Miller LCG
    seed = 12345
    for _ in range(count):
        seed = (seed * 16807) % 2147483647
        arr.append(seed % 1000000)

    start = time.perf_counter()
    quicksort(arr, 0, count - 1)
    elapsed_ms = (time.perf_counter() - start) * 1000
    print(f"Time: {elapsed_ms:.3f} ms")

    # Verification
    print(f"First: {arr[0]}")
    print(f"Last: {arr[count - 1]}")

    checksum = sum(arr[:10])
    print(f"Checksum (sum of first 10): {checksum}")

if __name__ == "__main__":
    main()
