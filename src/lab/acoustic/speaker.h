/* speaker.h - a moving-coil loudspeaker as its makers describe it, by its Thiele-Small parameters: the voice coil's
 * circuit and the cone's motion, coupled by the force factor Bl.
 *
 *   Le di/dt + Re i + Bl v = e(t)            (the coil: resistance, inductance, and the back-EMF of its motion)
 *   Mms dv/dt + Rms v + x / Cms = Bl i        (the cone: moving mass with its air load, losses, suspension)
 *   dx/dt = v
 *
 * integrated by fourth-order Runge-Kutta. The cone's velocity drives a piston of area Sd in the acoustic solver's
 * baffle (acoustic.h), which radiates the sound. The coil's inductance is taken as constant (the datasheet's value at
 * 1 kHz); its rise with excursion, the suspension's nonlinearity and cone break-up are not modelled.
 *
 * Units: SI (ohm, H, T m, kg, m/N, kg/s, m^2, V). */
#pragma once

typedef struct SpkDriver {
    double Re, Le, Bl, Mms, Cms, Rms, Sd;
} SpkDriver;

typedef struct SpkState {
    double i, x, v; /* A, m, m/s */
} SpkState;

typedef double (*SpkVoltageFn)(double t, void *ctx);

/* one step of dt from time t; returns the cone's acceleration at the end of the step (m/s^2) */
double spk_step(const SpkDriver *d, SpkState *s, double t, double dt, SpkVoltageFn e, void *ctx);
/* the cone's acceleration for a state at time t */
double spk_acceleration(const SpkDriver *d, const SpkState *s);
