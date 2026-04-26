#include <stdio.h>
#include <time.h>

int main(void) {
    int width = 800;
    int height = 800;
    int max_iter = 100;

    double x_min = -2.0;
    double x_max = 1.0;
    double y_min = -1.5;
    double y_max = 1.5;

    double dx = (x_max - x_min) / (double)width;
    double dy = (y_max - y_min) / (double)height;

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int in_set = 0;
    long long checksum = 0;

    for (int py = 0; py < height; py++) {
        double cy = y_min + (double)py * dy;
        for (int px = 0; px < width; px++) {
            double cx = x_min + (double)px * dx;
            double zx = 0.0;
            double zy = 0.0;
            int iter = 0;
            while (iter < max_iter) {
                double zx2 = zx * zx;
                double zy2 = zy * zy;
                if (zx2 + zy2 > 4.0) {
                    break;
                }
                zy = 2.0 * zx * zy + cy;
                zx = zx2 - zy2 + cx;
                iter++;
            }
            if (iter == max_iter) {
                in_set++;
            }
            checksum += iter;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0
                      + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("Time: %.3f ms\n", elapsed_ms);
    printf("In set: %d\n", in_set);
    printf("Checksum: %lld\n", checksum);

    return 0;
}
