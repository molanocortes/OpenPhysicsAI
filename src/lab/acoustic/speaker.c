/* speaker.c - the lumped loudspeaker (speaker.h). */
#include "speaker.h"

static void rates(const SpkDriver *d, const SpkState *s, double e, SpkState *r) {
    r->i = (e - d->Re * s->i - d->Bl * s->v) / d->Le;
    r->v = (d->Bl * s->i - d->Rms * s->v - s->x / d->Cms) / d->Mms;
    r->x = s->v;
}

double spk_acceleration(const SpkDriver *d, const SpkState *s) { return (d->Bl * s->i - d->Rms * s->v - s->x / d->Cms) / d->Mms; }

double spk_step(const SpkDriver *d, SpkState *s, double t, double dt, SpkVoltageFn e, void *ctx) {
    SpkState k1, k2, k3, k4, y;
    rates(d, s, e(t, ctx), &k1);
    y = (SpkState){s->i + 0.5 * dt * k1.i, s->x + 0.5 * dt * k1.x, s->v + 0.5 * dt * k1.v};
    rates(d, &y, e(t + 0.5 * dt, ctx), &k2);
    y = (SpkState){s->i + 0.5 * dt * k2.i, s->x + 0.5 * dt * k2.x, s->v + 0.5 * dt * k2.v};
    rates(d, &y, e(t + 0.5 * dt, ctx), &k3);
    y = (SpkState){s->i + dt * k3.i, s->x + dt * k3.x, s->v + dt * k3.v};
    rates(d, &y, e(t + dt, ctx), &k4);
    s->i += dt / 6 * (k1.i + 2 * k2.i + 2 * k3.i + k4.i);
    s->x += dt / 6 * (k1.x + 2 * k2.x + 2 * k3.x + k4.x);
    s->v += dt / 6 * (k1.v + 2 * k2.v + 2 * k3.v + k4.v);
    return spk_acceleration(d, s);
}
