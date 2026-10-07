/* mpc-analog: shared analog-modelling kernels for the MPC OS VST2 ports (header only, no allocation, no globals).
 *   - ramp-core oscillator: band-limited saw / triangle / saw-triangle / pulse (polyBLEP, polyBLAMP) and the pieces for hard sync
 *   - OTA-cascade lowpass: zero-delay feedback, each stage limiting its own input, 2- or 4-pole, resonance from the last stage
 *   - Kaiser half-band decimators (4x -> 2x -> 1x) for oversampling the analog section
 *   - the soft tanh used by the stages and the VCA
 * Written from the papers (Valimaki/Huovilainen, Zavalishin, Valimaki/Pekonen/Nam), no GPL code. MIT. */
#ifndef MPC_ANALOG_H
#define MPC_ANALOG_H
#include <math.h>

#define MA_PI 3.14159265358979

/* rational tanh, within about 1e-2 over |x| <= 3 and clamped beyond */
static inline float ma_tanh(float x) { x = x < -3 ? -3 : x > 3 ? 3 : x; return x * (27 + x * x) / (27 + 9 * x * x); }

/* ---- oscillator ---- */
enum { MA_SAW, MA_TRI, MA_SAWTRI, MA_PULSE };

/* residual of a unit step at phase 0 (height 2); t = phase, dt = phase increment */
static inline float ma_polyblep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}
/* residual of a slope change (for a step of height 2 a slope change of 8 needs a coefficient of 4) */
static inline float ma_polyblamp(float t, float dt) {
    if (t < dt) { t = t / dt - 1; return -(1.0f / 3) * t * t * t * dt; }
    if (t > 1 - dt) { t = (t - 1) / dt + 1; return (1.0f / 3) * t * t * t * dt; }
    return 0;
}
/* Band-limited ramp-core VCO sample at phase ph in [0,1), -1..1. The saw carries a little 2nd harmonic (a leaky integrator
 * capacitor); the pulse is off (0) at duty <= 0 or >= 1 and DC-free. */
static inline float ma_osc(int shape, float ph, float inc, float duty) {
    float saw = 2 * ph - 1 - ma_polyblep(ph, inc);
    float t2 = ph + 0.5f;
    if (t2 >= 1) t2 -= 1;
    float tri = ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
    tri += 4 * (ma_polyblamp(ph, inc) - ma_polyblamp(t2, inc));
    saw += 0.02f * (1 - saw * saw) - 0.0133f;
    switch (shape) {
    case MA_SAW: return saw;
    case MA_TRI: return tri;
    case MA_SAWTRI: return 0.5f * (saw + tri);
    default: {
        if (duty <= 0.0f || duty >= 1.0f) return 0;
        float x = ph < duty ? 1.0f : -1.0f;
        x += ma_polyblep(ph, inc);
        float t3 = ph - duty + (ph < duty ? 1 : 0);
        x -= ma_polyblep(t3, inc);
        return x - (2 * duty - 1);
    }
    }
}
/* the same waveform without band-limiting (for the jump a hard sync causes) */
static inline float ma_osc_naive(int shape, float ph, float duty) {
    float saw = 2 * ph - 1 + 0.02f * (1 - (2 * ph - 1) * (2 * ph - 1)) - 0.0133f;
    float tri = ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
    switch (shape) {
    case MA_SAW: return saw;
    case MA_TRI: return tri;
    case MA_SAWTRI: return 0.5f * (saw + tri);
    default: return (duty <= 0.0f || duty >= 1.0f) ? 0 : (ph < duty ? 1.0f : -1.0f) - (2 * duty - 1);
    }
}
/* Hard sync, band-limited. The master wrapped d (0..1) of a sample before the end of this one, and the slave (phase ph_after_step
 * after this sample's increment, inc per sample) is reset to 0 there. Add *this_sample to the slave's output now and *next_sample
 * to its next sample, then set its phase. The correction subtracts what ma_osc already applies for the slave's own wrap at phase 0
 * (a saw falls by 2, a pulse rises by 2). */
static inline void ma_sync(int shape, float duty, float ph_after_step, float inc, float d, float *this_sample, float *next_sample) {
    static const float own[4] = {-2, 0, -1, 2};
    float pre = ph_after_step - inc * d;
    pre -= floorf(pre);
    float jump = ma_osc_naive(shape, 0, duty) - ma_osc_naive(shape, pre, duty);
    *this_sample = 0.5f * jump * d * d;
    *next_sample = 0.5f * (jump - own[shape < 3 ? shape : 3]) * (2 * d - d * d - 1);
}

/* ---- DCO pair (timer-driven ramp cores, exact sub-sample reset timing) ----
 * For instruments whose oscillators are digitally timed (the Tempest): a ramp that resets exactly at the end of each period, shapes
 * taken from the ramp, every jump band-limited with a 2-point polyBLEP at its exact sub-sample moment (one sample of latency), hard
 * sync as oscillator 2's reset discharging oscillator 1, and a sub oscillator (flip-flop clocked by oscillator 1's resets). */
typedef struct {
    float ph[2];          /* ramp positions 0..1 */
    int flip;             /* sub oscillator flip-flop */
    float y[3], r[3];     /* last naive outputs (osc 1, osc 2, sub) and the BLEP residual owed to them */
} ma_dco_t;
typedef struct {
    int shape;            /* 0 off, 1 saw, 2 tri, 3 saw-tri, 4 pulse, 5 flat pulse */
    float duty, inc;
} ma_dco_osc_t;
/* One sample: osc 1, osc 2, sub (one sample late, band-limited). */
/* tri from the ramp: 1 at the reset, -1 half way (continuous at a natural reset) */
static inline float ma_dco_tri(float p) { return 2 * fabsf(2 * p - 1) - 1; }
static inline float ma_dco_shape(const ma_dco_osc_t *o, float p) {
    switch (o->shape) {
    case 1: return 2 * p - 1;
    case 2: return ma_dco_tri(p);
    case 3: return 0.5f * (2 * p - 1 + ma_dco_tri(p));
    case 4: return p < o->duty ? 1.0f : -1.0f;
    default: return 0;
    }
}
static inline void ma_dco_step(float h, float t, float *rp, float *rn) {
    /* polyBLEP: the sample before the step gets +h/2 (1-s)^2 with s its distance to the step (s = 1 - t), the sample after it
     * gets -h/2 (1-t)^2 */
    *rp += h * 0.5f * t * t;
    *rn -= h * 0.5f * (1 - t) * (1 - t);
}

static inline void ma_dco_tick(ma_dco_t *d, const ma_dco_osc_t *o1, const ma_dco_osc_t *o2, int sync, float out[3]) {
    float rn[3] = {0, 0, 0};
    /* osc 2 first: its reset may discharge osc 1 */
    float p2 = d->ph[1] + o2->inc, t2 = -1;
    if (o2->shape == 4 && d->ph[1] < o2->duty && p2 >= o2->duty) ma_dco_step(-2, (p2 - o2->duty) / o2->inc, &d->r[1], &rn[1]);
    if (p2 >= 1) {
        p2 -= 1;
        t2 = p2 / o2->inc;          /* samples since the reset */
        float before = ma_dco_shape(o2, 1.0f - 1e-7f), after = ma_dco_shape(o2, 0);
        if (after != before) ma_dco_step(after - before, t2, &d->r[1], &rn[1]);
        if (o2->shape == 4 && p2 >= o2->duty) ma_dco_step(-2, (p2 - o2->duty) / o2->inc, &d->r[1], &rn[1]);
    }
    d->ph[1] = p2;
    /* osc 1: its own reset, or osc 2's */
    float p1 = d->ph[0] + o1->inc;
    if (sync && t2 >= 0) {
        float at = p1 - o1->inc * t2;              /* where osc 1 was when osc 2 reset */
        if (at >= 1) {                             /* it reset on its own first */
            at -= 1;
            float tn = (p1 - 1) / o1->inc;
            float jb = ma_dco_shape(o1, 0) - ma_dco_shape(o1, 1.0f - 1e-7f);
            if (jb != 0) ma_dco_step(jb, tn, &d->r[0], &rn[0]);
            ma_dco_step(d->flip ? -2.0f : 2.0f, tn, &d->r[2], &rn[2]);
            d->flip ^= 1;
        } else if (o1->shape == 4 && d->ph[0] < o1->duty && at >= o1->duty)
            ma_dco_step(-2, t2 + (at - o1->duty) / o1->inc, &d->r[0], &rn[0]);
        float j = ma_dco_shape(o1, 0) - ma_dco_shape(o1, at);
        if (j != 0) ma_dco_step(j, t2, &d->r[0], &rn[0]);
        ma_dco_step(d->flip ? -2.0f : 2.0f, t2, &d->r[2], &rn[2]);
        d->flip ^= 1;
        p1 = o1->inc * t2;
        if (o1->shape == 4 && p1 >= o1->duty) ma_dco_step(-2, (p1 - o1->duty) / o1->inc, &d->r[0], &rn[0]);
    } else {
        if (o1->shape == 4 && d->ph[0] < o1->duty && p1 >= o1->duty) ma_dco_step(-2, (p1 - o1->duty) / o1->inc, &d->r[0], &rn[0]);
        if (p1 >= 1) {
            p1 -= 1;
            float t1 = p1 / o1->inc;
            float jb = ma_dco_shape(o1, 0) - ma_dco_shape(o1, 1.0f - 1e-7f);
            if (jb != 0) ma_dco_step(jb, t1, &d->r[0], &rn[0]);
            ma_dco_step(d->flip ? -2.0f : 2.0f, t1, &d->r[2], &rn[2]);
            d->flip ^= 1;
            if (o1->shape == 4 && p1 >= o1->duty) ma_dco_step(-2, (p1 - o1->duty) / o1->inc, &d->r[0], &rn[0]);
        }
    }
    d->ph[0] = p1;
    /* emit the previous sample with its corrections, keep this one */
    float now[3] = {ma_dco_shape(o1, p1), ma_dco_shape(o2, p2), d->flip ? 1.0f : -1.0f};
    for (int k = 0; k < 3; k++) {
        out[k] = d->y[k] + d->r[k];
        d->y[k] = now[k];
        d->r[k] = rn[k];
    }
}

/* ---- OTA-cascade lowpass ---- */
/* st[4]: integrator states, nl[4]: per-stage limiting; G0 = g/(1+g), g = tan(pi f/fs); k: feedback (self-oscillates from about 4
 * in 4-pole mode, never in 2-pole). Returns the output of the last stage. */
static inline float ma_ota_lpf(float *st, float *nl, float G0, float k, int four, float in) {
    int ns = four ? 4 : 2;
    float Gs[4], a = 1, b = 0;
    for (int j = 0; j < ns; j++) {
        /* cutoff of an OTA that has begun to limit: g(1 - n) in G = g / (1 + g) terms, to second order in n (no division) */
        float n = nl[j];
        Gs[j] = G0 * (1 - n) * (1 + n * G0 * (1 + n * G0));
        b = Gs[j] * b + (1 - Gs[j]) * st[j];
        a *= Gs[j];
    }
    /* the input limiter is wide (a clean signal up to about 1, soft above) and the stages limit gently: self-oscillation settles near 0.5 */
    float prev = 3.0f * ma_tanh((in * (1 + (four ? 0.35f : 0.1f) * k) - k * b) / (1 + k * a) * (1.0f / 3));
    for (int j = 0; j < ns; j++) {
        float vv = (prev - st[j]) * Gs[j], yy = vv + st[j];
        st[j] = yy + vv;
        float u = (prev - yy) * 0.5f;
        nl[j] = u * u / (3 + u * u);        /* 1 - tanh(u) / u, roughly */
        prev = yy;
    }
    if (!four) st[2] = st[3] = 0;
    return prev;
}
/* G0 for a cutoff in Hz at sample rate fs (pass fs times the oversampling factor) */
static inline float ma_ota_G(float hz, float fs) {
    if (hz > 0.45f * fs) hz = 0.45f * fs;
    float g = tanf(3.14159265f * hz / fs);
    return g / (1 + g);
}

/* ---- half-band decimators ---- */
/* lowpass at fs/4 (Kaiser window): the centre tap is 0.5, even taps are zero, tap 2j+1 is c[j] on both sides; m taps per side */
typedef struct { float z[64]; int p; } ma_hb_t;
static inline double ma_bessel0(double x) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
static inline void ma_hb_design(float *c, int m, double beta) {
    int K = 2 * m - 1;
    for (int j = 0; j < m; j++) {
        int k = 2 * j + 1;
        double w = ma_bessel0(beta * sqrt(1 - (double)k * k / ((K + 1.0) * (K + 1.0)))) / ma_bessel0(beta);
        c[j] = (float)(sin(MA_PI * k / 2) / (MA_PI * k) * w);
    }
}
/* two input samples in (oldest first), one out at half the rate */
static inline float ma_hb_dec(ma_hb_t *h, const float *c, int m, float x0, float x1) {
    h->z[h->p & 63] = x0; h->z[(h->p + 1) & 63] = x1; h->p += 2;
    int K = 2 * m - 1, mid = h->p - 1 - K;
    float y = 0.5f * h->z[mid & 63];
    for (int j = 0; j < m; j++) y += c[j] * (h->z[(mid - 2 * j - 1) & 63] + h->z[(mid + 2 * j + 1) & 63]);
    return y;
}
/* the sets the ports use: 4x -> 2x (6 taps, beta 6.0) and 2x -> 1x (12 taps, beta 8.6; about 70 dB down above the band) */
#define MA_HB_A_M 6
#define MA_HB_B_M 12
static inline void ma_hb_design_standard(float a[MA_HB_A_M], float b[MA_HB_B_M]) { ma_hb_design(a, MA_HB_A_M, 6.0); ma_hb_design(b, MA_HB_B_M, 8.6); }

/* Ready-made 2x-oversampled filter for ports that run at the host rate (no oversampling of their own): two inner steps at 2 fs, the
 * cutoff interpolated across the sample (G is computed once per sample, the middle one is the mean of this and the last), decimated by
 * the standard 2x -> 1x half-band (hb_b from ma_hb_design_standard). Zero it to reset. res is 0..1: four-pole feedback k = 4.6 res
 * (self-oscillates near the top), two-pole k = 1.2 res (never does). The gate-leak and noise-floor tricks are the caller's
 * (st[0] += x, noise added to the input). */
typedef struct { float st[4], nl[4]; ma_hb_t hb; float Gp; } ma_ota2x_t;
static inline float ma_ota2x(ma_ota2x_t *f, const float *hb_b, float in, float hz, float res, int four, float fs) {
    float k = four ? 4.6f * res : 1.2f * res, y[2];
    /* the stages' limiting lowers the self-oscillation pitch by about 1.15 semitones at low cutoffs, less towards the top (measured at
     * 65-14900 Hz): raise the cutoff to compensate, in proportion to res^4 (the amplitude, so the limiting, grows with it) */
    float comp = 1;
    if (four && res > 0.4f) {
        float r2 = res * res;
        comp = 1 + 0.0578f * 1.15f * r2 * r2 * (1 - sqrtf(hz * (1.0f / 16000)));
        if (comp < 1) comp = 1;
    }
    float G = ma_ota_G(comp * hz, 2 * fs);
    if (f->Gp == 0) f->Gp = G;
    y[0] = ma_ota_lpf(f->st, f->nl, 0.5f * (f->Gp + G), k, four, in);
    y[1] = ma_ota_lpf(f->st, f->nl, G, k, four, in);
    f->Gp = G;
    return ma_hb_dec(&f->hb, hb_b, MA_HB_B_M, y[0], y[1]);
}

#endif
