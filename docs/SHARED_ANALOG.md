# A shared analog engine for Morpho-PE and Sturm?

2026-10-07. Compares `src/engine.c` here (`analog_osc`, `ota_lpf`, the oversampler) with `../mpc-vst-sturm/src/analog.c`
(`dco_tick`, `cem_tick`), both read-only for this note.

## Do the two instruments use the same oscillator and filter?

Not provably, and not exactly the same kind of oscillator.
- **Filter.** Sturm's notes call the Tempest's filter "a Curtis 2/4-pole" (its spec sheet and manual); the Poly Evolver's manual says only
  "analog 2/4-pole resonant lowpass" and names no chip. Both behave like a Curtis OTA cascade (four cells, resonance fed back from the
  fourth, or the second in 2-pole mode where it cannot self-oscillate), and that is what both models are. The chip numbers I used in the
  README ("in the manner of a CEM3340 / CEM3320") are from memory, not from these files; treat them as an assumption.
- **Oscillator.** They differ. The Tempest's oscillators are DCOs: the voice firmware times each period with a 40 MHz timer and a 3 us
  reset pulse, i.e. a reset integrator with exact periods (Sturm's `dco_tick`). The Evolver's analog oscillators are controlled by
  voltages the DSP computes from calibration tables (manual: "the DSP also computes the control voltages for the analog
  circuitry"), so they are VCOs: pitch comes through a DAC and exponential converter, with drift. Both produce a ramp-core saw,
  triangle, saw-triangle and pulse, so one waveform core can serve both; the pitch behaviour is a mode (exact vs. calibrated with drift).

## What each repo has

| | Morpho-PE | Sturm |
|---|---|---|
| Oscillator core | phase accumulator, polyBLEP saw/pulse, polyBLAMP triangle, ramp bend, drift | reset integrator, exact sub-sample reset times, BLEP on every jump |
| Hard sync | BLEP of the jump, corrected for the kernel the oscillator already applies | exact (reset time t2), also drives the sub oscillator |
| Sub oscillator | none | flip-flop on oscillator 1's resets |
| Pulse extremes | off at 0 and 99 | flat at 0 % and 99 % |
| Filter | zero-delay-feedback trapezoid cascade, per-stage limiting, 1x/2x/4x | Huovilainen explicit cascade, 2x inside, tuning polynomials, half-sample feedback average |
| Self-oscillation pitch (set -> measured, Hz) | 65 -> 61, 262 -> 248, 1047 -> 1002, 4186 -> 4117, 8000 -> 7958 | 65 -> 63, 262 -> 254, 1047 -> 1034, 4186 -> 4477, 8000 -> 8820 |
| Cost on x86 (ns per output sample) | 38 at 1x (the ZDF needs no inner loop; 2x doubles it) | 87 (two passes inside) |
| Oversampling | Kaiser half-band decimators, 1x/2x/4x, Quality parameter | 2x, fixed, averaging |
| Extras | flush-to-zero, drift | noise floor, gate leak, feedback path, 2-pole HPF |

Both self-oscillate within about a semitone of the set cutoff over most of the range. Morpho's runs about a semitone flat at the
bottom (the tanh limiting) and is correct near the top; Sturm's is a little flat at the bottom and 5-10 % sharp at the top. Neither has
been measured against hardware.

## What a shared library would hold

A small MIT C library (`mpc-analog`: no allocation, no I/O, portable to armhf and x86):
1. **`ramp_osc`**: one reset-integrator core with the union of both features: exact reset times, polyBLEP on every jump and polyBLAMP on
   the triangle corner, sync with the exact jump size, sub oscillator, flat/off pulse extremes, optional slow drift and ramp bend.
   Sturm uses it exactly (DCO mode); Morpho feeds it a calibrated pitch with drift (VCO mode).
2. **`ota_cascade`**: the zero-delay-feedback cascade (cheaper, stable at the top of the range), with Sturm's feedback-path inputs
   (noise floor, gate leak) and a self-oscillation tuning correction fitted to both measurements above; 2-pole and 4-pole; resonance
   0..1 mapped to the loop gain.
3. **Oversampling:** the Kaiser half-band decimator and the 1x/2x/4x switch.
4. **Small pieces:** the tanh used by both, the soft VCA, polyBLEP/BLAMP kernels, flush-to-zero.
5. **A shared test and bench harness:** the self-oscillation table, the aliasing test, the OB-Xd comparison and an armhf profile program
   (what found the on-device cost of the filter).

Left in each plugin: everything instrument-specific (Morpho's firmware curves, digital oscillators, effects; Sturm's samples, kit mode,
feedback loop).

## How to share the code

The release workflow builds from one repository, so the library has to be inside each repo when it is built:
- **git submodule** (`third_party/mpc-analog`, checked out by the workflow): one source of truth, a version pin per plugin; or
- **vendored copy** synced by a script (`tools/sync_analog.sh`): simplest for CI, risks drift.
`vst.json` lists sources relative to the repo root, so either works with `"sources": [..., "third_party/mpc-analog/src/*.c"]`.

## Order of work (proposed; nothing here is done yet)
1. Create `mpc-analog` locally with the oscillator and cascade ported from both, plus the tests above; check each against its plugin's
   current output (null test: the plugin's rendered audio must not change beyond the intended improvements).
2. Move Morpho-PE onto it (it has the oversampler and the ZDF cascade already), measure on the Force (`prof_arm` harness).
3. Move Sturm onto it once its session is at a quiet point; both plugins keep their own tests.
4. Then publish the library and add it to the catalog's notes for other ports.

Risks: the two plugins are developed in parallel sessions, so the library needs a version and a changelog, and Sturm's voice loop
depends on `dco_tick`'s one-sample latency.
