/* Parameter-to-physical curves. Each one is a formula or a short breakpoint list fitted to what the DSP firmware 3.5 tables
 * hold (docs/FIRMWARE.md has the measurements); no firmware data is copied here. */
#pragma once
float pe_note_hz(float semis);          /* osc frequency: 0 = C-2 (8.18 Hz), semitone steps (MIDI note numbers) */
float pe_lpf_hz(float v);               /* lowpass cutoff 0..164, semitones from C0 */
float pe_hpf_hz(float v);               /* 4-pole highpass 1..99, semitones; 99 = 21.55 kHz */
float pe_env_seconds(float v);          /* envelope attack ramp time 0..110 (also the linear decay's full-scale time) */
float pe_env_tau_seconds(float v);      /* exponential decay time constant 0..110 (release: 4x) */
float pe_env_curve(float x);            /* exponential-shape attack: output for the linear ramp x in 0..1 */
float pe_lfo_hz(int v);                 /* LFO frequency 0..150 (unsynced) */
float pe_delay_seconds(int v);          /* delay tap time 0..150 (unsynced), the firmware's samples at 48 kHz */
float pe_glide_seconds(int v);          /* glide 1..100: seconds per octave */
float pe_dist_gain(float p);            /* distortion 0..99: gain before the hard clip */
float pe_env_delay_seconds(int v);      /* envelope 3 delay 0..100 */
