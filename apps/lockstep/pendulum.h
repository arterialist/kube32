/* A double pendulum (two unit masses on unit rods, g = 9.81) integrated with
 * fourth-order Runge-Kutta at h = 0.001 s, from theta1 = 2.0, theta2 = 2.5 at
 * rest.
 *
 * Every value is an IEEE 754 binary64 and every operation is one of + - * /
 * and the conversions between double and int. sin and cos are the fdlibm
 * kernels on [-pi/4, pi/4] after a Cody-Waite reduction by pi/2 in three
 * parts, written out here, so no host math library takes part. Wasm rounds
 * each of these operations the same way on every CPU and never fuses a
 * multiply and an add, so a step gives the same bits on Xtensa, RISC-V, arm64
 * and x86_64. Built for Wasm with -mcpu=mvp, the module holds no float
 * instruction but f64 add, sub, mul, div, neg, compare and the i32 and f64
 * conversions (apps/lockstep/float_ops.py checks it after each build). */
#ifndef LOCKSTEP_PENDULUM_H
#define LOCKSTEP_PENDULUM_H

#include <stdint.h>

#ifndef PENDULUM_NOINLINE
#define PENDULUM_NOINLINE __attribute__((noinline))
#endif

/* theta1, theta2, omega1, omega2 */
typedef struct {
    double v[4];
} pendulum_t;

static inline void pendulum_start(pendulum_t* p) {
    p->v[0] = 2.0;
    p->v[1] = 2.5;
    p->v[2] = 0.0;
    p->v[3] = 0.0;
}

static double pendulum_ksin(double x) {
    const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03, S3 = -1.98412698298579493134e-04,
                 S4 = 2.75573137070700676789e-06, S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    const double z = x * x, r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    return x + x * z * (S1 + z * r);
}

static double pendulum_kcos(double x) {
    const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03, C3 = 2.48015872894767294178e-05,
                 C4 = -2.75573143513906633035e-07, C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    const double z = x * x, r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    return 1.0 - (0.5 * z - z * r);
}

/* x = k * pi/2 + *y with |*y| <= pi/4; returns k mod 4. A state that a fault
 * made huge or NaN takes k = 0 instead of trapping in the conversion, so the
 * lane runs on and is outvoted. */
static int pendulum_reduce(double x, double* y) {
    const double invpio2 = 6.36619772367581382433e-01, p1 = 1.57079632673412561417e+00, p2 = 6.07710050650619224932e-11,
                 p3 = 2.02226624879595063154e-21;
    double fk = x * invpio2;
    const int k = fk > -1e9 && fk < 1e9 ? (int)(fk < 0 ? fk - 0.5 : fk + 0.5) : 0;
    fk = (double)k;
    *y = ((x - fk * p1) - fk * p2) - fk * p3;
    return k & 3;
}

static PENDULUM_NOINLINE double pendulum_sin(double x) {
    double y;
    switch (pendulum_reduce(x, &y)) {
    case 0: return pendulum_ksin(y);
    case 1: return pendulum_kcos(y);
    case 2: return -pendulum_ksin(y);
    default: return -pendulum_kcos(y);
    }
}

static PENDULUM_NOINLINE double pendulum_cos(double x) {
    double y;
    switch (pendulum_reduce(x, &y)) {
    case 0: return pendulum_kcos(y);
    case 1: return -pendulum_ksin(y);
    case 2: return -pendulum_kcos(y);
    default: return pendulum_ksin(y);
    }
}

/* The derivative of state v: the equations of motion of the double pendulum */
static PENDULUM_NOINLINE void pendulum_deriv(const double* v, double* d) {
    const double g = 9.81, l = 1.0, m = 1.0;
    const double t1 = v[0], t2 = v[1], w1 = v[2], w2 = v[3], dt = t1 - t2;
    const double den = 2 * m + m - m * pendulum_cos(2 * dt);
    d[0] = w1;
    d[1] = w2;
    d[2] = (-g * (2 * m + m) * pendulum_sin(t1) - m * g * pendulum_sin(t1 - 2 * t2) -
            2 * pendulum_sin(dt) * m * (w2 * w2 * l + w1 * w1 * l * pendulum_cos(dt))) / (l * den);
    d[3] = (2 * pendulum_sin(dt) * (w1 * w1 * l * (m + m) + g * (m + m) * pendulum_cos(t1) + w2 * w2 * l * m * pendulum_cos(dt))) / (l * den);
}

/* One step of h = 0.001 s */
static PENDULUM_NOINLINE void pendulum_step(pendulum_t* p) {
    double k1[4], k2[4], k3[4], k4[4], t[4];
    const double h = 0.001;
    double* s = p->v;
    pendulum_deriv(s, k1);
    for (int i = 0; i < 4; i++) t[i] = s[i] + h / 2 * k1[i];
    pendulum_deriv(t, k2);
    for (int i = 0; i < 4; i++) t[i] = s[i] + h / 2 * k2[i];
    pendulum_deriv(t, k3);
    for (int i = 0; i < 4; i++) t[i] = s[i] + h * k3[i];
    pendulum_deriv(t, k4);
    for (int i = 0; i < 4; i++) s[i] += h / 6 * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]);
}

/* FNV-1a of the state's 32 bytes, little-endian as Wasm stores them */
static inline uint32_t pendulum_hash(const pendulum_t* p) {
    const unsigned char* b = (const unsigned char*)p->v;
    uint32_t h = 0x811C9DC5u;
    for (unsigned i = 0; i < sizeof p->v; i++) h = (h ^ b[i]) * 0x01000193u;
    return h;
}

#endif /* LOCKSTEP_PENDULUM_H */
