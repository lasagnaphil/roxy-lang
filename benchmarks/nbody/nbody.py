"""N-body simulation of the outer Jovian planets, ported from the Computer
Language Benchmarks Game. Five bodies (Sun + Jupiter, Saturn, Uranus, Neptune)
interact via Newtonian gravity; we step the system forward and report the
total energy before and after to verify correctness.
"""

import math
import time

PI = 3.141592653589793
SOLAR_MASS = 4.0 * PI * PI
DAYS_PER_YEAR = 365.24

def make_body(x, y, z, vx, vy, vz, mass):
    return [x, y, z, vx, vy, vz, mass]

def energy(bodies):
    n = len(bodies)
    e = 0.0
    for i in range(n):
        bi = bodies[i]
        e += 0.5 * bi[6] * (bi[3]*bi[3] + bi[4]*bi[4] + bi[5]*bi[5])
        for j in range(i + 1, n):
            bj = bodies[j]
            dx = bi[0] - bj[0]
            dy = bi[1] - bj[1]
            dz = bi[2] - bj[2]
            d = math.sqrt(dx*dx + dy*dy + dz*dz)
            e -= bi[6] * bj[6] / d
    return e

def offset_momentum(bodies, solar_mass):
    px = py = pz = 0.0
    for b in bodies:
        px += b[3] * b[6]
        py += b[4] * b[6]
        pz += b[5] * b[6]
    bodies[0][3] = -px / solar_mass
    bodies[0][4] = -py / solar_mass
    bodies[0][5] = -pz / solar_mass

def advance(bodies, dt):
    n = len(bodies)
    for i in range(n):
        bi = bodies[i]
        for j in range(i + 1, n):
            bj = bodies[j]
            dx = bi[0] - bj[0]
            dy = bi[1] - bj[1]
            dz = bi[2] - bj[2]
            d2 = dx*dx + dy*dy + dz*dz
            mag = dt / (d2 * math.sqrt(d2))
            mi = bi[6]
            mj = bj[6]
            bi[3] -= dx * mj * mag
            bi[4] -= dy * mj * mag
            bi[5] -= dz * mj * mag
            bj[3] += dx * mi * mag
            bj[4] += dy * mi * mag
            bj[5] += dz * mi * mag
    for b in bodies:
        b[0] += dt * b[3]
        b[1] += dt * b[4]
        b[2] += dt * b[5]

def main():
    bodies = [
        # Sun
        make_body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS),
        # Jupiter
        make_body(
            4.84143144246472090,
            -1.16032004402742839,
            -0.103622044471123109,
            0.00166007664274403694 * DAYS_PER_YEAR,
            0.00769901118419740425 * DAYS_PER_YEAR,
            -0.0000690460016972063023 * DAYS_PER_YEAR,
            0.000954791938424326609 * SOLAR_MASS,
        ),
        # Saturn
        make_body(
            8.34336671824457987,
            4.12479856412430479,
            -0.403523417114321381,
            -0.00276742510726862411 * DAYS_PER_YEAR,
            0.00499852801234917238 * DAYS_PER_YEAR,
            0.0000230417297573763929 * DAYS_PER_YEAR,
            0.000285885980666130812 * SOLAR_MASS,
        ),
        # Uranus
        make_body(
            12.8943695621391310,
            -15.1111514016986312,
            -0.223307578892655734,
            0.00296460137564761618 * DAYS_PER_YEAR,
            0.00237847173959480950 * DAYS_PER_YEAR,
            -0.0000296589568540237556 * DAYS_PER_YEAR,
            0.0000436624404335156298 * SOLAR_MASS,
        ),
        # Neptune
        make_body(
            15.3796971148509165,
            -25.9193146099879641,
            0.179258772950371181,
            0.00268067772490389322 * DAYS_PER_YEAR,
            0.00162824170038242295 * DAYS_PER_YEAR,
            -0.0000951592254519715870 * DAYS_PER_YEAR,
            0.0000515138902046611451 * SOLAR_MASS,
        ),
    ]

    offset_momentum(bodies, SOLAR_MASS)

    e0 = energy(bodies)
    print(f"Energy start: {e0:g}")

    steps = 500000
    dt = 0.01

    start = time.perf_counter()
    for _ in range(steps):
        advance(bodies, dt)
    elapsed_ms = (time.perf_counter() - start) * 1000

    e1 = energy(bodies)
    print(f"Energy end:   {e1:g}")
    print(f"Time: {elapsed_ms:.3f} ms")

if __name__ == "__main__":
    main()
