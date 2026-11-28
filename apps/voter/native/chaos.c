/* The double pendulum of apps/lockstep/pendulum.h as native C: RK4 at
 * h = 0.001 s from theta1 = 2.0, theta2 = 2.5 at rest, with the C library's
 * sin and cos. Built with different compilers and C libraries, the same
 * source gives different bits for the same steps.
 *
 * Every 2000 steps a line "<step> <theta1> <theta1's bits>"; with an argument
 * N, also every N steps a line "s <step> <theta1> <theta2>" in hex float. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void deriv(const double s[4], double d[4]) {
    const double g = 9.81, l = 1.0, m = 1.0;
    double t1 = s[0], t2 = s[1], w1 = s[2], w2 = s[3], dt = t1 - t2;
    double den = 2*m + m - m*cos(2*dt);
    d[0] = w1; d[1] = w2;
    d[2] = (-g*(2*m+m)*sin(t1) - m*g*sin(t1-2*t2) - 2*sin(dt)*m*(w2*w2*l + w1*w1*l*cos(dt))) / (l*den);
    d[3] = (2*sin(dt)*(w1*w1*l*(m+m) + g*(m+m)*cos(t1) + w2*w2*l*m*cos(dt))) / (l*den);
}
int main(int argc, char** argv) {
    const int sample = argc > 1 ? atoi(argv[1]) : 0;
    double s[4] = {2.0, 2.5, 0.0, 0.0}, k1[4], k2[4], k3[4], k4[4], t[4], h = 0.001;
    for (int step = 1; step <= 40000; step++) {
        deriv(s, k1); for (int i=0;i<4;i++) t[i]=s[i]+h/2*k1[i];
        deriv(t, k2); for (int i=0;i<4;i++) t[i]=s[i]+h/2*k2[i];
        deriv(t, k3); for (int i=0;i<4;i++) t[i]=s[i]+h*k3[i];
        deriv(t, k4); for (int i=0;i<4;i++) s[i]+=h/6*(k1[i]+2*k2[i]+2*k3[i]+k4[i]);
        if (step % 2000 == 0) { uint64_t b; memcpy(&b,&s[0],8); printf("%5d %+.6f %016llx\n", step, s[0], (unsigned long long)b); }
        if (sample > 0 && step % sample == 0) printf("s %d %a %a\n", step, s[0], s[1]);
    }
    return 0;
}
