/* mpc-analog checks:  gcc -O2 -Ianalog -o /tmp/ma_test analog/test/test_analog.c -lm && /tmp/ma_test */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "mpc_analog.h"
static int fails;
#define CHECK(c, ...) do { int ok_ = (c); printf(ok_ ? "ok   " : "FAIL "); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

/* frequency of the self-oscillation of the 4-pole at cutoff hz (zero crossings after settling) */
static double selfosc(float hz, int four, float k, float *amp) {
    float st[4] = {0}, nl[4] = {0}, G = ma_ota_G(hz, 48000), last = 0, mx = 0;
    int cross = 0, first = -1, lastc = -1;
    for (int i = 0; i < 96000; i++) {
        float y = ma_ota_lpf(st, nl, G, k, four, i < 8 ? 0.1f : 0);
        if (i > 48000) { if (last < 0 && y >= 0) { if (first < 0) first = i; lastc = i; cross++; } if (fabsf(y) > mx) mx = fabsf(y); }
        last = y;
    }
    *amp = mx;
    return cross > 1 ? (cross - 1) * 48000.0 / (lastc - first) : 0;
}
int main(void) {
    float amp;
    for (float hz = 250; hz < 5000; hz *= 4) {
        double f = selfosc(hz, 1, 4.5f, &amp);
        CHECK(fabs(12 * log2(f / hz)) < 1.0, "4-pole self-oscillates at %.0f Hz set -> %.0f Hz (amplitude %.2f)", hz, f, amp);
    }
    CHECK(selfosc(1000, 0, 4.5f, &amp) == 0, "2-pole never self-oscillates");

    /* saw alias level: harmonic energy above the band vs. below, C7 at 48 kHz, direct DFT of the fold-back */
    float inc = 215.0f / 4800, ph = 0;
    enum { N = 4800 };
    static float x[N];
    for (int i = 0; i < N; i++) { x[i] = ma_osc(MA_SAW, ph, inc, 0.5f); ph += inc; ph -= floorf(ph); }
    double tot = 0, ali = 0;
    for (int b = 1; b < N / 2; b++) {
        double re = 0, im = 0;
        for (int i = 0; i < N; i++) { re += x[i] * cos(2 * MA_PI * b * i / N); im += x[i] * sin(2 * MA_PI * b * i / N); }
        double p = re * re + im * im;
        tot += p;
        if (b % 215) ali += p;     /* 215 whole cycles in the window: every harmonic sits on a multiple of 215 */
    }
    CHECK(10 * log10(ali / tot) < -25, "saw at 2150 Hz, 1x: non-harmonic energy %.1f dB", 10 * log10(ali / tot));

    float a[MA_HB_A_M], b[MA_HB_B_M];
    ma_hb_design_standard(a, b);
    float dc = 0; ma_hb_t h = {{0}, 0};
    for (int i = 0; i < 100; i++) dc = ma_hb_dec(&h, b, MA_HB_B_M, 1, 1);
    CHECK(fabs(dc - 1) < 1e-3, "half-band passes DC (%.4f)", dc);
    float nyq = 0; ma_hb_t h2 = {{0}, 0};
    for (int i = 0; i < 100; i++) nyq = ma_hb_dec(&h2, b, MA_HB_B_M, 1, -1);
    CHECK(fabs(nyq) < 0.01, "half-band stops the top frequency (%.4f)", nyq);
    /* the 2x filter: self-oscillation within 0.3 semitone of the set cutoff at 44.1 kHz, 2-pole silent */
    float hbA[MA_HB_A_M], hbB[MA_HB_B_M];
    ma_hb_design_standard(hbA, hbB);
    for (float hz = 65; hz < 20000; hz *= 4) {
        ma_ota2x_t f; memset(&f, 0, sizeof f);
        float last = 0, mx = 0; int c = 0, first = -1, lc = -1;
        for (int n = 0; n < 176400; n++) {
            float y = ma_ota2x(&f, hbB, n < 4 ? 0.05f : 0, hz, 1.0f, 1, 44100);
            if (n > 88200) { if (last < 0 && y >= 0) { if (first < 0) first = n; lc = n; c++; } if (y > mx) mx = y; }
            last = y;
        }
        double fo = c > 1 ? (c - 1) * 44100.0 / (lc - first) : 0;
        CHECK(fo > 0 && fabs(12 * log2(fo / hz)) < 0.3, "2x filter set %.0f Hz -> %.0f Hz", hz, fo);
    }
    { ma_ota2x_t f; memset(&f, 0, sizeof f); float mx = 0;
      for (int n = 0; n < 88200; n++) { float y = ma_ota2x(&f, hbB, n < 4 ? 0.05f : 0, 1000, 1.0f, 0, 44100); if (n > 44100 && fabsf(y) > mx) mx = fabsf(y); }
      CHECK(mx < 1e-4, "2x filter, 2-pole at full resonance stays silent (%.1e)", mx); }

    /* DCO: pitch is exact (reset timing), the sub is an octave down, sync makes osc 1 follow osc 2 */
    { ma_dco_t d; memset(&d, 0, sizeof d);
      ma_dco_osc_t o1 = {1, 0.5f, 440.0f / 44100}, o2 = {1, 0.5f, 220.0f / 44100};
      float out[3]; int c1 = 0, c3 = 0; float l1 = 0, l3 = 0;
      for (int i = 0; i < 44100; i++) { ma_dco_tick(&d, &o1, &o2, 0, out); if (l1 < 0 && out[0] >= 0) c1++; if (l3 < 0 && out[2] >= 0) c3++; l1 = out[0]; l3 = out[2]; }
      CHECK(abs(c1 - 440) <= 1 && abs(c3 - 220) <= 1, "DCO: osc 1 %d Hz, sub %d Hz", c1, c3);
      memset(&d, 0, sizeof d); o1.inc = 330.0f / 44100; int c = 0; l1 = 0;
      for (int i = 0; i < 44100; i++) { ma_dco_tick(&d, &o1, &o2, 1, out); if (l1 < 0 && out[0] >= 0) c++; l1 = out[0]; }
      CHECK(abs(c - 220) <= 1, "DCO: synced osc 1 follows osc 2 (%d Hz)", c); }

    printf(fails ? "FAILED\n" : "all ok\n");
    return fails != 0;
}
