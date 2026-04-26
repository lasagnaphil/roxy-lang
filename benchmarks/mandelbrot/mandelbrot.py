import time

def main():
    width = 800
    height = 800
    max_iter = 100

    x_min = -2.0
    x_max = 1.0
    y_min = -1.5
    y_max = 1.5

    dx = (x_max - x_min) / width
    dy = (y_max - y_min) / height

    start = time.perf_counter()

    in_set = 0
    checksum = 0

    for py in range(height):
        cy = y_min + py * dy
        for px in range(width):
            cx = x_min + px * dx
            zx = 0.0
            zy = 0.0
            iter_count = 0
            while iter_count < max_iter:
                zx2 = zx * zx
                zy2 = zy * zy
                if zx2 + zy2 > 4.0:
                    break
                zy = 2.0 * zx * zy + cy
                zx = zx2 - zy2 + cx
                iter_count += 1
            if iter_count == max_iter:
                in_set += 1
            checksum += iter_count

    elapsed_ms = (time.perf_counter() - start) * 1000
    print(f"Time: {elapsed_ms:.3f} ms")
    print(f"In set: {in_set}")
    print(f"Checksum: {checksum}")

if __name__ == "__main__":
    main()
