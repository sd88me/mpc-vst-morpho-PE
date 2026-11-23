/* mpc-analog: shared analog-modelling kernels for the MPC OS VST2 ports (header only, no allocation, no globals).
 *   - ramp-core oscillator: band-limited saw / triangle / saw-triangle / pulse (polyBLEP, polyBLAMP) and the pieces for hard sync
 *   - OTA-cascade lowpass: zero-delay feedback, each stage limiting its own input, 2- or 4-pole, resonance from the last stage
 *   - Kaiser half-band decimators (4x -> 2x -> 1x) for oversampling the analog section
 *   - the soft tanh used by the stages and the VCA
 *   - (from Profit-5) CV quantiser, analog drift, SSM 2050 envelope, SSM 2040 / CEM 3320 filter-as-circuit kernels, SSM 2020 control port
 * Written from the papers (Valimaki/Huovilainen, Zavalishin, Valimaki/Pekonen/Nam), no GPL code. MIT. */
#ifndef MPC_ANALOG_H
#define MPC_ANALOG_H
#include <math.h>
#include <stdint.h>

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
static inline float ma_osc_saw(float ph, float inc) {
    float saw = 2 * ph - 1 - ma_polyblep(ph, inc);
    return saw + (0.02f * (1 - saw * saw) - 0.0133f);
}
static inline float ma_osc_tri(float ph, float inc) {
    float t2 = ph + 0.5f;
    if (t2 >= 1) t2 -= 1;
    float tri = ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
    return tri + 4 * (ma_polyblamp(ph, inc) - ma_polyblamp(t2, inc));
}
/* Band-limited ramp-core VCO sample at phase ph in [0,1), -1..1. The saw carries a little 2nd harmonic (a leaky integrator
 * capacitor); the pulse is off (0) at duty <= 0 or >= 1 and DC-free. Only the shape asked for is computed. */
static inline float ma_osc(int shape, float ph, float inc, float duty) {
    switch (shape) {
    case MA_SAW: return ma_osc_saw(ph, inc);
    case MA_TRI: return ma_osc_tri(ph, inc);
    case MA_SAWTRI: return 0.5f * (ma_osc_saw(ph, inc) + ma_osc_tri(ph, inc));
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
typedef struct { float z[128]; int p; } ma_hb_t;   /* each sample is kept twice (at q and q + 64) so the taps need no wrap */
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
    int q0 = h->p & 63, q1 = (h->p + 1) & 63;
    h->z[q0] = h->z[q0 + 64] = x0;
    h->z[q1] = h->z[q1 + 64] = x1;
    h->p += 2;
    int K = 2 * m - 1;
    const float *z = h->z + 64 + q1 - K;          /* the centre tap; the newest sample is z[K], the oldest z[-K] (K <= 47) */
    float y = 0.5f * z[0];
    for (int j = 0; j < m; j++) y += c[j] * (z[-2 * j - 1] + z[2 * j + 1]);
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

/* ======== Prophet-style SSM / CEM kernels (added from Profit-5, MIT; Rev 1/2 SSM 2030/2040/2050/2020 and the Rev 3 CEM 3320 filter as circuits,
 * checked against ngspice, see mpc-vst-profit5 docs/CIRCUIT.md). Plain additions: no existing function changed. ======== */

/* ---- control-voltage path: 7-bit ADC/DAC, multiplexed sample-and-hold ----
 * The CPU reads each pot with a 7-bit ADC and writes the value back through the CV DAC into a per-parameter S&H, refreshed
 * once per main-loop pass. Service manual 3.3: every S&H moves in 1/12 V steps; a pot at 10 reads about 10 V (121 steps),
 * the oscillator frequency pots are halved by the firmware to 5 V = 60 semitone steps. Each step is rounded by the S&H RC.
 * q: steps (121 for authentic pots, 61 for frequency, 0 = continuous); tau_s: S&H/filter RC in seconds. */
typedef struct { float y, a; } ma_cv_t;
static inline void ma_cv_init(ma_cv_t *c, float tau_s, float fs) { c->y = 0; c->a = 1 - expf(-1 / (tau_s * fs)); }
static inline float ma_cv_quant(float x01, int q) { return q > 1 ? floorf(x01 * (q - 1) + 0.5f) / (q - 1) : x01; }
static inline float ma_cv_tick(ma_cv_t *c, float target) { c->y += (target - c->y) * c->a; return c->y; }

/* ---- analog drift: per-voice slow random walk (about 1/f over 0.05-5 Hz) plus a fixed tolerance ----
 * Three one-pole-filtered noise sources at octave-spaced corners, summed: cheap pink-ish wander. Call at a control rate
 * (rate_hz = update rate, for example fs/16); returns about -1..1 (unit std), scale outside (cents or octaves). */
typedef struct { float s[3], c[3]; uint32_t rng; } ma_drift_t;
static inline float ma_drift_rnd(uint32_t *r) { *r = *r * 1664525u + 1013904223u; return (float)(int32_t)*r * (1.0f / 2147483648.0f); }
static inline void ma_drift_init(ma_drift_t *d, uint32_t seed, float rate_hz) {
    static const float hz[3] = { 0.08f, 0.6f, 4.0f };
    d->rng = seed * 2654435761u + 1;
    for (int i = 0; i < 3; i++) { d->s[i] = 0; d->c[i] = 1 - expf(-6.2831853f * hz[i] / rate_hz); }
}
static inline float ma_drift_tick(ma_drift_t *d) {
    static const float w[3] = { 1.0f, 0.5f, 0.25f };   /* 1/f-like weights */
    float y = 0;
    for (int i = 0; i < 3; i++) {
        d->s[i] += d->c[i] * (ma_drift_rnd(&d->rng) - d->s[i]);
        /* a one-pole filtered uniform has std 0.577 sqrt(c / (2 - c)): normalise each band to unit std */
        y += w[i] * d->s[i] / (0.577f * sqrtf(d->c[i] / (2 - d->c[i])));
    }
    return y * (1.0f / 1.1456f);                        /* sqrt(1 + 0.25 + 0.0625) */
}

/* ---- RC envelope generator (SSM 2050 style ADSR; board 4 ICs 1-10, 0.1 uF timing cap, per-voice RC trim) ----
 * Each stage is a capacitor charging toward a target through a CV-controlled current: attack aims past the top (over,
 * assumed 1.3 until measured) and switches to decay at 1, so attack is the concave-down start of an RC curve; decay and
 * release are true exponentials toward sustain / 0. Times are the 0 -> 1 (attack) or 1 -> 1/e^k (k below) times in seconds. */
enum { MA_ENV_IDLE, MA_ENV_A, MA_ENV_D, MA_ENV_R };
typedef struct { float y; int st; } ma_env_t;
typedef struct { float ca, cd, cr, s, over; } ma_env_k_t;
static inline float ma_env_coef(float t_s, float fs, float span) {   /* one-pole coefficient so that 'span' time constants take t_s */
    return 1 - expf(-span / (t_s * fs + 1e-9f));
}
static inline void ma_env_set(ma_env_k_t *k, float a_s, float d_s, float s, float r_s, float over, float fs) {
    k->over = over; k->s = s;
    k->ca = ma_env_coef(a_s, fs, -logf(1 - 1 / over));    /* reaches 1 (of target over) in a_s */
    k->cd = ma_env_coef(d_s, fs, 4.6f);                   /* -40 dB toward sustain in d_s */
    k->cr = ma_env_coef(r_s, fs, 4.6f);
}
static inline void ma_env_gate(ma_env_t *e, int on) { e->st = on ? MA_ENV_A : (e->st == MA_ENV_IDLE ? MA_ENV_IDLE : MA_ENV_R); }
static inline float ma_env_tick(ma_env_t *e, const ma_env_k_t *k) {
    switch (e->st) {
    case MA_ENV_A: e->y += (k->over - e->y) * k->ca; if (e->y >= 1) { e->y = 1; e->st = MA_ENV_D; } break;
    case MA_ENV_D: e->y += (k->s - e->y) * k->cd; break;
    case MA_ENV_R: e->y -= e->y * k->cr; if (e->y < 1e-5f) { e->y = 0; e->st = MA_ENV_IDLE; } break;
    default: break;
    }
    return e->y;
}

/* ---- SSM 2040 lowpass (Rev 1/2 voice, board 4 IC 78) ----
 * Four OTA integrator stages, each fed through a 10k/200 attenuator (about -34 dB, so the differential pair limits at a
 * signal of about 1), resonance through an SSM 2020 VCA from the output back to the input summer. No passband compensation:
 * the bass drops by 1 / (1 + k) as resonance rises, as on the instrument. Same structure as ma_ota_lpf (zero-delay feedback,
 * an OTA that limits lowers its own cutoff) with a harder input knee and harder stage limiting, and no input gain.
 * G0 from ma_ota_G; k: 0..~4.3 (self-oscillates from 4). st[4], nl[4]: states. */
static inline float ma_ssm2040_lpf(float *st, float *nl, float G0, float k, float in) {
    float Gs[4], a = 1, b = 0;
    for (int j = 0; j < 4; j++) {
        float n = nl[j];
        Gs[j] = G0 * (1 - n) * (1 + n * G0 * (1 + n * G0));
        b = Gs[j] * b + (1 - Gs[j]) * st[j];
        a *= Gs[j];
    }
    float prev = 1.4f * ma_tanh((in - k * b) / (1 + k * a) * (1.0f / 1.4f));
    for (int j = 0; j < 4; j++) {
        float vv = (prev - st[j]) * Gs[j], yy = vv + st[j];
        st[j] = yy + vv;
        if (fabsf(st[j]) < 1e-20f) st[j] = 0;
        float u = prev - yy;
        nl[j] = u * u / (3 + u * u);
        prev = yy;
    }
    return prev;
}

/* ---- self-oscillation seed: a real filter starts singing from its own noise; an ideal one fed silence never does ----
 * Add to the filter input when k is above the onset (about -70 dBFS). */
static inline float ma_selfosc_seed(uint32_t *rng, float k, float onset) {
    return k > onset ? 3e-4f * ma_drift_rnd(rng) : 0.0f;
}

/* ---- exponential glide (portamento as an RC on the pitch CV, constant time per interval) ---- */
static inline float ma_glide(float cur, float target, float coef) { return cur + (target - cur) * coef; }

/* ---- static per-voice component tolerance: a repeatable -1..1 value for (voice, part) ----
 * The instrument trims every voice by hand (manual 3.4: envelope RC, filter V/oct and initial frequency, oscillator initial
 * frequency, V/oct, high V/oct) and autotune then corrects only the oscillators' offset. Use for the residuals. */
static inline float ma_tol(unsigned voice, unsigned part) {
    uint32_t h = (voice * 0x9E3779B1u) ^ (part * 0x85EBCA77u);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return (float)(int32_t)h * (1.0f / 2147483648.0f);
}

/* ---- SSM 2020 control port wired as a diode (control collector and both bases tied, emitter grounded) fed through R from a CV ----
 * Solves CV = I R + V_T ln(I / I_s) for I (A) by Newton from the larger of the two asymptotes. ASSUMED: I_s = 2.5e-15 A, so the
 * transistor drops 0.65 V at 240 uA (a typical small NPN; the datasheet gives none). Below about 0.5 V the current falls off
 * exponentially instead of stopping. */
static inline float ma_2020_ctl(float cv, float r) {
    const float vt = 0.025693f, is = 2.5e-15f;
    float i = cv > 0.7f ? (cv - 0.65f) / r : is * expf(fminf(cv, 0.7f) / vt) * 0.5f;
    if (i < 1e-15f) i = 1e-15f;
    for (int k = 0; k < 6; k++) {                   /* f(i) = i r + vt ln(i / is) - cv, f' = r + vt / i */
        float fx = i * r + vt * logf(i / is) - cv, d = fx / (r + vt / i);
        i -= d;
        if (i < 1e-15f) i = 1e-15f;
        if (fabsf(d) < 1e-4f * i) break;
    }
    return i;
}

/* ---- SSM 2040 filter section as a circuit (Rev 2 voice SD411; docs/CIRCUIT.md "Filter", circuits/vcf2040.cir) ----
 * Four 2040 lowpass real-pole cells (datasheet Fig. 1): an OTA I = I_ABC tanh(V_IN / 2 V_T) into 1000 pF, a unity buffer, IN on a
 * 10k / 10k / 200 ohm network (stage 1: no R1; its input is a current). The swing is limited to +-1 V at CAP (datasheet: OUT swings
 * +-1 V; where the chip limits is assumed). OUT4 drives U479 (x20, clipped at +-13.5 V) = F; F through C413 0.1 uF / R471 100k
 * (15.9 Hz) is H, which feeds the final VCA and the RES FDBK VCA (SSM 2020: I_c H / 14 V into IN1).
 * Voltages are the circuit's. Inputs per sample: w0 = the cells' small-signal pole (2 pi f, rad/s; the 2040's I_ABC scales it),
 * kres = 10k I_c / 14 V (the loop gain is 20 kres: 4.45 at RES 10), x = the mixer's current into IN1 times 10k (volts).
 * Each sample is solved implicitly: the tanh of each cell is linearised (tanh z / z) at the last estimate, the linear zero-delay
 * loop is solved exactly, and that repeats until no cell's gain moves by 1e-5 (at most MA_VCF_ITER passes; *unconv counts the
 * rest). Returns H (volts). s[0..3]: the cells' integrator states, s[4]: the high-pass state. */
#ifndef MA_VCF_ITER
#define MA_VCF_ITER 12
#endif
typedef struct { float s[5], q[4], o4; } ma_vcf2040_t;   /* q: each cell's tanh(z) / z at its last operating point; o4: OUT4 (probe) */
static inline float ma_tanhz_z(float z) {                 /* tanh(z) / z: Lambert's (7,6) continued fraction, capped at 1 / |z| where it
                                                            would pass 1 (|z| ~ 5). Continuous, within 1e-4 of tanh; no exp, one division */
    float z2 = z * z;
    float r = (135135.0f + z2 * (17325.0f + z2 * (378.0f + z2))) / (135135.0f + z2 * (62370.0f + z2 * (3150.0f + 28.0f * z2)));
    float a = fabsf(z);
    return a * r > 1.0f ? 1.0f / a : r;
}
static inline float ma_vcf2040(ma_vcf2040_t *f, float g0, float gh, float kres, float x, uint32_t *unconv) {
    /* g0 = tan(w0 T / 2) (prewarped at the small-signal pole), gh = tan(w_hp T / 2); the cells' u / 2 V_T scale per volt of
     * (in + out): r / 10k / (2 V_T) with r = 192.3 ohm (stages 2-4) and 196.1 ohm (stage 1) */
    const float kz = 192.31f / 10000.0f / (2 * 0.025693f), kz1 = 196.08f / 10000.0f / (2 * 0.025693f);
    const float w1 = 196.08f / 192.31f;                  /* stage 1's pole against the others' (its IN node lacks R1) */
    if (f->q[0] == 0) f->q[0] = f->q[1] = f->q[2] = f->q[3] = 1;   /* a cleared state: the cells at rest */
    float Gh = gh / (1 + gh), y[4], xin = x, F = 0, H = 0, G[4];
    int it;
    for (it = 0; it < MA_VCF_ITER; it++) {
        float S[4], A = 1, B = 0;
        for (int i = 0; i < 4; i++) {
            float g = g0 * (i ? 1.0f : w1) * f->q[i];
            G[i] = g / (1 + g);
            S[i] = (1 - G[i]) * f->s[i];
            A *= G[i];
            B = -G[i] * B + S[i];                         /* y_i = -G_i y_(i-1) + S_i: B is y4 with the input at 0 */
        }
        /* x1 = x + kres H, H = (1 - Gh) (F - s_h), F = -20 y4 (unless clipped); y4 = A x1 + B */
        float c = kres * (1 - Gh);
        float n4 = (A * (x - c * f->s[4]) + B) / (1 + 20 * c * A);
        F = -20 * n4;
        if (F > 13.5f || F < -13.5f) F = F > 0 ? 13.5f : -13.5f;
        H = (1 - Gh) * (F - f->s[4]);
        xin = x + kres * H;
        /* the cells' operating points this solution gives; converged when none of their gains moves (another pass would
         * return the same solution) */
        float z[4], q[4], dq = 0;
        y[0] = -G[0] * xin + S[0];
        z[0] = kz1 * (xin + y[0]);
        for (int i = 1; i < 4; i++) { y[i] = -G[i] * y[i - 1] + S[i]; z[i] = kz * (y[i - 1] + y[i]); }
        for (int i = 0; i < 4; i++) q[i] = ma_tanhz_z(z[i]);
        for (int i = 0; i < 4; i++) { float d = fabsf(q[i] - f->q[i]); if (d > dq) dq = d; f->q[i] = q[i]; }
        if (dq < 1e-4f) break;
    }
#ifdef MA_VCF_COUNT
    extern unsigned long ma_vcf_passes, ma_vcf_samples; ma_vcf_passes += it + (it < MA_VCF_ITER); ma_vcf_samples++;
#endif
    if (it == MA_VCF_ITER) ++*unconv;
    /* commit with the gains the solution used: the cells' states (CAP limited to +-1 V), the high-pass state */
    float prev = xin;
    for (int i = 0; i < 4; i++) {
        float v = (-prev - f->s[i]) * G[i], yy = v + f->s[i];
        if (yy > 1) yy = 1; else if (yy < -1) yy = -1;
        f->s[i] = yy + v;
        if (f->s[i] > 1) f->s[i] = 1; else if (f->s[i] < -1) f->s[i] = -1;
        prev = yy;
    }
    f->o4 = prev;
    float vh = (F - f->s[4]) * Gh;
    f->s[4] = vh + (vh + f->s[4]);
    return H;
}

/* ---- CEM 3320 filter section as a circuit (Rev 3 voice SD432, TM1000D; the Rev 4 reissue's "Rev 3" filter; circuits/vcf3320.cir) ----
 * Four 3320 sections (the CEM 3320 datasheet, 1980; the V3320 clone's gives the same constants): a current-in, current-out gain cell into its pole cap and
 * a buffer; I_out = (I_ref - I_in) A e^(-Vc/V_T), I_ref = 63 uA. Rev 3's network: 100k feedback (90.9k with the cell's reflected 1M),
 * 91k coupling (stage gain -0.999), the inputs' DC balanced by 240k from -15 V; stage 1's input is a current (the mixer, the Q VCA).
 * ASSUMED: each cell's input current limits softly at +-I_ref (I_ref tanh(i / I_ref)); the buffers' swing +-6 V around 6.9 V (Vcc - 3 V
 * peak to peak). Resonance: OUT D through 2.2 uF / 68k (1.06 Hz) into U475 (x3.4, +-13.5 V), then 51k into Q IN (3.6k || 3k: x0.0311);
 * the Q VCA gives -I_cr tanh(v gm / I_cr) into stage 1, gm = 2.24 mmho I_cr / (I_cr + 122.5 uA) (the CEM 3320 sheet's Figure 6, fitted
 * within 14 umho; 1.0 mmho at 100 uA, its typical). Voltages are the circuit's, against the quiescent point.
 * Inputs per sample: g0 = tan(w0 T / 2) (the stages' small-signal pole), gh = tan(w_hp T / 2) (the 1.06 Hz coupling), icr = the Q VCA's
 * control current (A), x = the mixer's current into stage 1 times 90.9k (volts), kc = 1 - exp(-T / (10 uF x 3k)) (C4146's step, updated a
 * sample late: its corner is 2.5 Hz). Returns U475's output (volts). Solved as ma_vcf2040. */
typedef struct { float s[5], q[5], o4, vc; } ma_vcf3320_t;   /* q[0..3]: cells' tanh(z) / z; q[4]: the Q VCA's; vc: C4146 */
static inline float ma_vcf3320(ma_vcf3320_t *f, float g0, float gh, float icr, float x, float kc, uint32_t *unconv) {
    /* Q IN node: vb through 51k, 3.6k to ground, 3k to C4146: vq = qdiv vb + qoff, with qoff from C4146's voltage */
    const float rf = 90.9e3f, iref = 63e-6f, kz = 1.0f / (rf * iref), alpha = 90.9f / 91.0f, rpar = 1.0f / (1 / 51e3f + 1 / 3.6e3f + 1 / 3e3f),
                qdiv = rpar / 51e3f, qoff = f->vc * rpar / 3e3f, vq0 = (icr + 122.5e-6f) * (1 / 2.24e-3f);   /* I_cr / gm */
    if (f->q[0] == 0) f->q[0] = f->q[1] = f->q[2] = f->q[3] = f->q[4] = 1;
    float Gh = gh / (1 + gh), y[4], xin = x, H = 0, G[4];
    int it;
    for (it = 0; it < MA_VCF_ITER; it++) {
        float S[4], A = 1, B = 0;
        for (int i = 0; i < 4; i++) {
            float g = g0 * f->q[i];
            G[i] = g / (1 + g);
            S[i] = (1 - G[i]) * f->s[i];
            float a = i ? alpha * G[i] : G[i];
            A *= a;
            B = -a * B + S[i];                         /* y_i = -G_i (alpha) y_(i-1) + S_i */
        }
        /* x1 = x - rf Kq vb, vb = 3.4 (1 - Gh) (y4 - s_h) (U475, clipped), Kq = I_cr / 0.1 V x tanh(u)/u x 0.0311 */
        float kq = rf * icr / vq0 * f->q[4] * qdiv, c = kq * 3.4f * (1 - Gh);
        float n4 = (A * (x - rf * icr / vq0 * f->q[4] * qoff + c * f->s[4]) + B) / (1 + A * c);
        float vb = 3.4f * (1 - Gh) * (n4 - f->s[4]);
        if (vb > 13.5f || vb < -13.5f) vb = vb > 0 ? 13.5f : -13.5f;
        H = vb;
        float u = (qdiv * vb + qoff) / vq0;
        xin = x - rf * icr / vq0 * f->q[4] * (qdiv * vb + qoff);
        float z[5], q[5], dq = 0;
        y[0] = -G[0] * xin + S[0];
        z[0] = kz * (xin + y[0]);
        for (int i = 1; i < 4; i++) { y[i] = -G[i] * alpha * y[i - 1] + S[i]; z[i] = kz * (alpha * y[i - 1] + y[i]); }
        z[4] = u;
        for (int i = 0; i < 5; i++) q[i] = ma_tanhz_z(z[i]);
        for (int i = 0; i < 5; i++) { float d = fabsf(q[i] - f->q[i]); if (d > dq) dq = d; f->q[i] = q[i]; }
        if (dq < 1e-4f) break;
    }
#ifdef MA_VCF_COUNT
    extern unsigned long ma_vcf_passes, ma_vcf_samples; ma_vcf_passes += it + (it < MA_VCF_ITER); ma_vcf_samples++;
#endif
    if (it == MA_VCF_ITER) ++*unconv;
    float prev = xin;
    for (int i = 0; i < 4; i++) {
        float in = i ? alpha * prev : prev;
        float v = (-in - f->s[i]) * G[i], yy = v + f->s[i];
        if (yy > 6) yy = 6; else if (yy < -6) yy = -6;
        f->s[i] = yy + v;
        if (f->s[i] > 6) f->s[i] = 6; else if (f->s[i] < -6) f->s[i] = -6;
        prev = yy;
    }
    f->o4 = prev;
    float vh = (prev - f->s[4]) * Gh;
    f->s[4] = vh + (vh + f->s[4]);
    f->vc += kc * (qdiv * H + qoff - f->vc);
    return H;
}

#endif
