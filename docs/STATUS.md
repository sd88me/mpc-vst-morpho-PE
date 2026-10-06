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
- Plugin: 219 parameters (128 program + 64 steps + host controls + popup state), an eight-tab skin in the browser renderer
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
  key-tracking scales, the cutoff in Hz (calibrated analog hardware), unison detune (main CPU key assignment).
- External audio input, its peak and envelope follower and the Ext In trigger modes have nothing to work on in an instrument
  plugin: the Ext In trigger modes act like their keyboard counterparts and the input sources read zero.
- Sequencer MIDI-out destinations (notes, velocity, controllers) are ignored: MPC does not take MIDI from a VST.
- Not run on a device: skin, Q-Links, CPU and the MPC OS 2.x shape are unchecked.

## Skin previews without Docker (2026-10-06)
from `vst/`: `SHADOW_ART=../../mpc-vst-plugins/tools/html_art.py python3 ../../mpc-vst-plugins/tools/gen_vst.py vst.json`, then `tools/studio.py preview`.
It needs the `playwright` Python package matching the installed Chromium (in this environment: `pip install playwright==1.56.0`
for Chromium build 1194).

## Next steps
1. Device: build, bench, play, save/reload a project (mpc-vst-plugins docs/PORTING.md section 4).
2. Main CPU (dsPIC) code: unison detune, key tracking, the sequencer clock; needs a dsPIC decoder.
3. Run more of the DSP (digital oscillators 3/4, the delay and feedback paths) in `tools/fw/adsp219x_sim.py` as a reference for the digital half.
4. Recordings of a real instrument for the analog half.
5. The catalog route (docs/CATALOG.md of mpc-vst-plugins).
