/* N-body simulation of the outer Jovian planets, ported from the Computer
 * Language Benchmarks Game. Five bodies (Sun + Jupiter, Saturn, Uranus,
 * Neptune) interact via Newtonian gravity; we step the system forward and
 * report the total energy before and after to verify correctness. */

#include <stdio.h>
#include <math.h>
#include <time.h>

#define N_BODIES 5

typedef struct {
    double x, y, z;
    double vx, vy, vz;
    double mass;
} Body;

static double energy(const Body *bodies, int n) {
    double e = 0.0;
    for (int i = 0; i < n; i++) {
        const Body *bi = &bodies[i];
        e += 0.5 * bi->mass * (bi->vx*bi->vx + bi->vy*bi->vy + bi->vz*bi->vz);
        for (int j = i + 1; j < n; j++) {
            const Body *bj = &bodies[j];
            double dx = bi->x - bj->x;
            double dy = bi->y - bj->y;
            double dz = bi->z - bj->z;
            double d = sqrt(dx*dx + dy*dy + dz*dz);
            e -= bi->mass * bj->mass / d;
        }
    }
    return e;
}

static void offset_momentum(Body *bodies, int n, double solar_mass) {
    double px = 0.0, py = 0.0, pz = 0.0;
    for (int i = 0; i < n; i++) {
        px += bodies[i].vx * bodies[i].mass;
        py += bodies[i].vy * bodies[i].mass;
        pz += bodies[i].vz * bodies[i].mass;
    }
    bodies[0].vx = -px / solar_mass;
    bodies[0].vy = -py / solar_mass;
    bodies[0].vz = -pz / solar_mass;
}

static void advance(Body *bodies, int n, double dt) {
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            double dx = bodies[i].x - bodies[j].x;
            double dy = bodies[i].y - bodies[j].y;
            double dz = bodies[i].z - bodies[j].z;
            double d2 = dx*dx + dy*dy + dz*dz;
            double mag = dt / (d2 * sqrt(d2));
            double mi = bodies[i].mass;
            double mj = bodies[j].mass;
            bodies[i].vx -= dx * mj * mag;
            bodies[i].vy -= dy * mj * mag;
            bodies[i].vz -= dz * mj * mag;
            bodies[j].vx += dx * mi * mag;
            bodies[j].vy += dy * mi * mag;
            bodies[j].vz += dz * mi * mag;
        }
    }
    for (int i = 0; i < n; i++) {
        bodies[i].x += dt * bodies[i].vx;
        bodies[i].y += dt * bodies[i].vy;
        bodies[i].z += dt * bodies[i].vz;
    }
}

int main(void) {
    double pi = 3.141592653589793;
    double solar_mass = 4.0 * pi * pi;
    double days_per_year = 365.24;

    Body bodies[N_BODIES] = {
        /* Sun */
        {0.0, 0.0, 0.0,
         0.0, 0.0, 0.0,
         solar_mass},
        /* Jupiter */
        { 4.84143144246472090,
         -1.16032004402742839,
         -0.103622044471123109,
          0.00166007664274403694 * 365.24,
          0.00769901118419740425 * 365.24,
         -0.0000690460016972063023 * 365.24,
          0.000954791938424326609 * (4.0 * 3.141592653589793 * 3.141592653589793)},
        /* Saturn */
        { 8.34336671824457987,
          4.12479856412430479,
         -0.403523417114321381,
         -0.00276742510726862411 * 365.24,
          0.00499852801234917238 * 365.24,
          0.0000230417297573763929 * 365.24,
          0.000285885980666130812 * (4.0 * 3.141592653589793 * 3.141592653589793)},
        /* Uranus */
        {12.8943695621391310,
        -15.1111514016986312,
         -0.223307578892655734,
          0.00296460137564761618 * 365.24,
          0.00237847173959480950 * 365.24,
         -0.0000296589568540237556 * 365.24,
          0.0000436624404335156298 * (4.0 * 3.141592653589793 * 3.141592653589793)},
        /* Neptune */
        {15.3796971148509165,
        -25.9193146099879641,
          0.179258772950371181,
          0.00268067772490389322 * 365.24,
          0.00162824170038242295 * 365.24,
         -0.0000951592254519715870 * 365.24,
          0.0000515138902046611451 * (4.0 * 3.141592653589793 * 3.141592653589793)},
    };

    offset_momentum(bodies, N_BODIES, solar_mass);

    double e0 = energy(bodies, N_BODIES);
    printf("Energy start: %g\n", e0);

    int steps = 500000;
    double dt = 0.01;

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    for (int i = 0; i < steps; i++) {
        advance(bodies, N_BODIES, dt);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0
                      + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;

    double e1 = energy(bodies, N_BODIES);
    printf("Energy end:   %g\n", e1);
    printf("Time: %.3f ms\n", elapsed_ms);

    return 0;
}
