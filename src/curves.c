#include <math.h>
#include "curves.h"

float pe_note_hz(float s) { return 8.1757989f * exp2f(s * (1.0f / 12)); }
float pe_lpf_hz(float v) { return 16.351598f * exp2f(v * (1.0f / 12)); }
/* The highpass coefficient sets step one semitone per value (measured -3 dB points: 99 = 21.55 kHz, 50 = 1.27 kHz). */
float pe_hpf_hz(float v) { return 21551.0f * exp2f((v - 99.0f) * (1.0f / 12)); }

static float interp_log(const float (*bp)[2], int n, float v) {
    if (v <= bp[0][0]) return bp[0][1];
    for (int i = 1; i < n; i++)
        if (v <= bp[i][0]) {
            float t = (v - bp[i - 1][0]) / (bp[i][0] - bp[i - 1][0]);
            return expf(logf(bp[i - 1][1]) + t * (logf(bp[i][1]) - logf(bp[i - 1][1])));
        }
    return bp[n - 1][1];
}

/* Envelope timing (DSP 3.5, docs/FIRMWARE.md section 6). The envelope routines run once per four samples: 12 kHz. Two 111-entry
 * tables serve them, both breakpoint-fitted here (log interpolation, within 4 % of every entry):
 *  - attack: a linear ramp that takes exactly the table's round millisecond counts (1 ms ... 10 s at 100, 44.7 s at 110); the
 *    same numbers are the full-scale time of the linear decay (and 4x that for the linear release);
 *  - decay and release: the level closes a fixed fraction of the way to its target every tick, i.e. an exponential; this table is
 *    the time constant in ms (decay; the release is 4x slower). */
float pe_env_seconds(float v) {
    static const float bp[][2] = {{0, 1}, {1, 2}, {2, 3}, {4, 5}, {6, 7}, {9, 10}, {11, 14}, {15, 22}, {25, 52}, {35, 100}, {40, 150},
        {45, 200}, {49, 280}, {63, 680}, {72, 1100}, {76, 1500}, {81, 2000}, {86, 3000}, {91, 4000}, {96, 6000}, {99, 8998},
        {105, 16947}, {107, 23797}, {110, 44739}};
    return interp_log(bp, sizeof bp / sizeof bp[0], v) * 0.001f;
}
float pe_env_tau_seconds(float v) {
    static const float bp[][2] = {{0, 0.5556f}, {1, 1.312f}, {5, 4.386f}, {9, 9.259f}, {12, 13.89f}, {16, 20.83f}, {19, 33.3f},
        {23, 50.1f}, {28, 73.8f}, {41, 160.6f}, {51, 227.6f}, {59, 341.3f}, {75, 582.5f}, {84, 932.1f}, {91, 1645}, {104, 6214},
        {106, 7989}, {108, 11185}, {110, 18641}};
    return interp_log(bp, sizeof bp / sizeof bp[0], v) * 0.0005f;     /* the breakpoints are twice the time constant: see FIRMWARE.md */
}
/* The exponential-shape attack: the output is this curve of the linear ramp x (0..1), 128 entries in the firmware, the first
 * ones to x = 1/128 steps; within 0.6 % of full scale of every entry. */
float pe_env_curve(float x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return (1 - expf(-1.3f * x)) / (1 - expf(-1.3f));
}

/* Unsynced LFO: round decimal frequencies up to 89 (piecewise linear), then semitones from 8.18 Hz (C-2) at 90 to 261.6 Hz at 150. */
float pe_lfo_hz(int v) {
    static const float bp[][2] = {{0, 0.0333f}, {1, 0.04f}, {13, 0.16f}, {14, 0.18f}, {15, 0.2f}, {16, 0.23f}, {17, 0.26f}, {18, 0.3f},
        {19, 0.35f}, {20, 0.4f}, {30, 0.9f}, {31, 1.0f}, {38, 1.35f}, {39, 1.45f}, {42, 1.6f}, {43, 1.7f}, {46, 2.0f}, {76, 5.0f},
        {86, 7.0f}, {87, 7.3f}, {88, 7.6f}, {89, 7.7f}};
    if (v >= 90) return 8.1757989f * exp2f((v - 90) * (1.0f / 12));
    int n = sizeof bp / sizeof bp[0];
    for (int i = 1; i < n; i++)
        if (v <= bp[i][0]) return bp[i - 1][1] + (v - bp[i - 1][0]) * (bp[i][1] - bp[i - 1][1]) / (bp[i][0] - bp[i - 1][0]);
    return bp[n - 1][1];
}

/* Delay taps: 1..21 samples, then semitones (22 = C7 = 22.93 samples, 94 = C1), then round sample counts up to 48000 (1 s). */
float pe_delay_seconds(int v) {
    static const short mid[] = {1550, 1650, 1750, 1850, 2000, 2200, 2500, 2900, 3400, 3900, 4600, 5200, 5900, 6600, 7200, 8000};
    float s;
    if (v <= 0) s = 0;
    else if (v <= 21) s = v;
    else if (v <= 94) s = 48000.0f / (2093.0045f * exp2f((22 - v) * (1.0f / 12)));
    else if (v <= 110) s = mid[v - 95];
    else s = v >= 150 ? 48000 : 9000 + 1000 * (v - 111);
    return s * (1.0f / 48000);
}

/* Glide: seconds for an octave. The voice CPU (docs/FIRMWARE.md section 8) steps each oscillator's 8.8 pitch toward its target by a
 * byte R(glide) every 2.5 timer ticks; R is a 101-entry table (255 at 0 and 1, 1 at 100, breakpoints here within 1 %). The tick is
 * Timer 2: 4 x 10 x 51 = 2040 instruction clocks; at a 40 MHz clock (assumed: the clock is not in the file) that is 204 us, so a
 * step every 510 us and 7.66 semitones per second per unit of R. */
float pe_glide_seconds(int v) {
    static const short bp[][2] = {{0, 255}, {2, 250}, {10, 170}, {11, 205}, {14, 190}, {21, 120}, {31, 90}, {51, 50}, {100, 1}};
    if (v <= 0) return 0;
    if (v > 100) v = 100;
    float r = (float)bp[8][1];
    for (int i = 1; i < 9; i++)
        if (v <= bp[i][0]) { r = bp[i - 1][1] + (float)(v - bp[i - 1][0]) * (bp[i][1] - bp[i - 1][1]) / (bp[i][0] - bp[i - 1][0]); break; }
    return 12.0f / (r * 7.66f);
}
