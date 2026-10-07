# Status (2026-10-06)

## Done (offline)
- Firmware decoded: update format, chip identification, DSP boot stream, voice tables (docs/FIRMWARE.md; `tools/fw/pe_fw.py`).
- Engine (`src/`): the full program format, 4 voices (1-8 selectable), poly / mono / unison 1 and 2 with the six key priorities,
  per-oscillator glide (normal, fingered, keyboard off), sync, slop, analog oscillators (saw, triangle, saw-tri, pulse 0-99 that
  turns off at the extremes), digital oscillators with FM and ring modulation both ways and shape sequencing, noise, 2/4-pole
  lowpass per channel with envelope, velocity, key tracking, audio mod and split, VCA, the seven output pan modes, tuned
  feedback with grunge, 4-pole highpass, distortion with noise gate, three-tap delay with both feedback paths and synced times,
  output hack, three envelopes (env 3 with delay), four LFOs (synced rates, key sync above 100), four mod slots and the
  seven fixed routes, the 4 x 16 sequencer with rests, resets, swing, clock modulation and the trigger modes, MIDI CCs.
- VS waves (2026-10-07): the VS program ROM chips are decoded (68000 byte interleave; docs/FIRMWARE.md section 14) and the plugin reads the
  user's own chip images for the 95 factory waves, bit-exact against Arturia's copy. A single-cycle recording alone fills 88 slots by correlation.
- Plugin: 278 parameters (128 program + 64 steps + host controls + popup state + the Banks page), a nine-tab skin in the browser renderer
  (`"art": "html"`, `vst/skin.css`, signal-flow drawings written by `tools/gen_layout.py`), banks from `.syx`,
  user waveshape dumps, Prophet VS wave dumps and single-cycle WAV banks, state chunk.
- Tests: `tools/test_port.sh` passes every check but one (below); `test/test_engine.c` passes (pitch to 0.1 Hz, every built-in
  program, sequencer, extremes, SysEx and state round trips, a folder of banks and a waveshape).
- Analog section (2026-10-06): CEM3340-style VCO, CEM3320-style OTA-cascade lowpass, soft VCA (src/engine.c `analog_osc`, `ota_lpf`). Checked offline:
  pole frequency = the set cutoff, 4-pole self-oscillates from resonance 4 (the engine's maximum is 4.5), 2-pole never does. Not compared
  with a real instrument.
- Hard sync is band-limited (polyBLEP of the jump, relative to the kernel the oscillator already applies at its own wrap): non-harmonic
  energy with sync on drops from about -33 dB to -45 dB at 1x and stays near -54 dB at 2x. An exponential VCA response was not added: the
  DSP's VCA control is linear (table 0x1AD7) and the chip's own curve needs measurements.
- Oversampling (2026-10-06): the analog section runs at 1x, 2x or 4x (host parameter Quality, default 2x), with Kaiser half-band
  decimators (about 70 dB down above the band). Saw/pulse aliasing at C6-F#7 falls from about -33 dB (1x) to about -49 dB (2x); 4x gains
  little more, and costs about 3.6x the engine's CPU against 1.8x for 2x (x86; the device is unmeasured, so check docs/BENCH.md before
  defaulting to 2x).
- Shared kernels (2026-10-07): the oscillator, OTA cascade, half-band decimators and tanh now live in `analog/mpc_analog.h` (this repo is the source of
  truth; Sturm keeps a synced copy, `analog/sync.sh --check ../mpc-vst-sturm/src`); output is bit-identical to before (docs/SHARED_ANALOG.md).
- Compared offline with a scalar port of the OB-Xd 4-pole as sst-filters publishes it (study only, nothing copied): the linear
  cascade and feedback solve are the same; OB-Xd is nearly linear until its self-oscillation (about 3-4.6 amplitude), here the OTAs
  limit (0.14 % THD at amplitude 0.8, 2.5 % at 3.2; self-oscillation about 0.5). Which is nearer the Evolver needs a recording.
- CPU: 2-6 % (before this change) of one x86 core for four held voices (rough; the device bench is still to do, docs/BENCH.md of mpc-vst-plugins).

## Known limits
- `test_port.sh`: "six data wheel clicks step six" fails on Osc1 Freq (0-120): a wheel click is 1.2 steps and the wrapper
  rounds it to 2. Every parameter with a range of 101-149 behaves so. A wrapper fix is proposed separately; `nudge_pct` is
  not used because it makes sweeps run fast on these ranges.
- Measured in the DSP and voice-CPU code (docs/FIRMWARE.md sections 6-11): envelope tick rate, shapes and times, the amount curves and the
  modulation units of pitch, levels, FM/RM, pulse width, LFO frequency/amount, envelope rates, feedback, highpass, delay, pan and VCA;
  distortion gain and hard clip, the noise gate, grunge (the mixer sum wraps instead of saturating), the output hack's bit masks, glide, and
  the Env 3 delay. The voice-CPU times assume a 40 MHz clock (not in the file).
- Still guessed: filter-frequency modulation depth (an estimate, 56 semitones), split, the sequencer destinations, audio-mod and filter
  key-tracking scales, the cutoff in Hz (calibrated analog hardware). Unison detune is the main CPU's table (2026-10-07, docs/FIRMWARE.md
  section 12): -1/+1/-3/+3 and -3/+3/-8/+8 cents.
- The external audio input is removed from the plugin (2026-10-07): no skin controls (Ext In level, input mode, input hack, the peak and
  envelope-follower routes) and no sound; its parameters stay in the program as "unused" so dumps load and save unchanged, the Ext In
  trigger modes act like their keyboard counterparts and the input sources read zero.
- Sequencer MIDI-out destinations (notes, velocity, controllers) are ignored: MPC does not take MIDI from a VST.
- Device (Force, 2026-10-07): deployed to /sdcard/Synths/sd88me - VST - Morpho-PE (settings backup MPC.settings.bak-sync-20261006-195200), MPC
  restarted, plugin listed. Current build (md5 6f907e66f658e37eef12e72731c203c2, Eco 1x default, `-DPE_DEFAULT_OS=1`) on `tools/bench.sh`
  (4 voices by default): idle 4.0 %, 1-16 held notes 15-17 % mean, p99 19 %, max 20 % (WARN class: p99 above 15 %); q-link sweep mean 38.5 %,
  p99 68 %, max 75 %; release tail mean 64 %, p99 67 %; verdict FAIL (the sweep and tail). The sweep sets random parameters, including Quality
  and Voices, and the tail then runs whatever the sweep left: on-device timing of the engine alone is 16 / 28 / 50 % of a block for 4 voices at
  1x / 2x / 4x and 32 / 57 / 101 % for 8 voices, so the tail (64 %) is 8 voices at 2x, not a denormal slowdown. The earlier 131-135 % sweep
  p99 was 8 voices at Ultra 4x: since 2026-10-07 more than 4 voices run at 2x even when Ultra is chosen (p99 135 % -> 68 %). Not yet done:
  insert/play/Q-Link/Banks-tab tests; the engine is about 5.7 times the x86 cost, so the way to PASS (p99 15 %, max 50 %) is to make the voice
  cheaper, not to change the sweep.
- Not run on a device: skin, Q-Links, CPU and the MPC OS 2.x shape are unchecked.

## Skin previews without Docker (2026-10-06)
from `vst/`: `SHADOW_ART=../../mpc-vst-plugins/tools/html_art.py python3 ../../mpc-vst-plugins/tools/gen_vst.py vst.json`, then `tools/studio.py preview`.
It needs the `playwright` Python package matching the installed Chromium (in this environment: `pip install playwright==1.56.0`
for Chromium build 1194).

## Next steps
1. Device: build, bench, play, save/reload a project (mpc-vst-plugins docs/PORTING.md section 4).
2. LP key tracking, audio mod and split scales: not in the main CPU (section 12); look in the voice CPU's 0-100 tables (0xB1A, 0xBE2,
   0xD4E) and the DSP's filter CV path.
3. Run more of the DSP (digital oscillators 3/4, the delay and feedback paths) in `tools/fw/adsp219x_sim.py` as a reference for the digital half.
4. Recordings of a real instrument for the analog half.
5. The catalog route (docs/CATALOG.md of mpc-vst-plugins).
