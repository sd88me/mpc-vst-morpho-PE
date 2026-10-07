# analog/ (mpc-analog)

Shared analog-modelling kernels for the MPC OS VST2 ports (Morpho-PE, Sturm). One header, `mpc_analog.h`: no allocation, no
globals, plain C99, MIT. Written from the papers (Valimaki/Huovilainen, Zavalishin, Valimaki/Pekonen/Nam); no GPL code.

| Piece | Functions |
|---|---|
| Ramp-core oscillator | `ma_osc` (saw, triangle, saw-triangle, pulse; polyBLEP/polyBLAMP), `ma_osc_naive`, `ma_sync` (band-limited hard sync) |
| OTA-cascade lowpass | `ma_ota_lpf` (zero-delay feedback, per-stage limiting, 2/4-pole), `ma_ota_G` |
| Oversampling | `ma_hb_design_standard`, `ma_hb_dec` (Kaiser half-band decimators, 4x -> 2x -> 1x) |
| Saturation | `ma_tanh` |

Tests: `gcc -O2 -Ianalog -o /tmp/ma_test analog/test/test_analog.c -lm && /tmp/ma_test` (self-oscillation tuning of both filters, 2-pole never
oscillates, alias level, half-band response, DCO pitch and sync). See `PLAN.md`.

## Using it
This directory is the source of truth: Morpho-PE builds with `-Ianalog`. Other ports (Sturm) keep a copy of the header so a release
builds from a single repo: `analog/sync.sh ../PORT/src` copies it in and `analog/sync.sh --check ../PORT/src` fails if the copy has
drifted. A change here must null-test every port: render a fixed set of programs before and after and compare a hash (Morpho-PE and
Sturm's DCO were bit-identical when they moved over; Sturm's filter changed on purpose).
