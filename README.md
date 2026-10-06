# Morpho-PE

A four-voice synthesizer for Akai MPC OS standalone devices (Force, MPC Live / One / X / Key), built as a native VST2 instrument with
its own screen skin and Q-Link pages. It plays the way the DSI Poly Evolver's voice does: two analog-style oscillators and two
digital waveshape oscillators per voice, a stereo 2/4-pole lowpass and VCA, the DSP side's highpass, tuned feedback, distortion,
three-tap delay and output hack, three envelopes, four LFOs, the modulation slots and fixed controller routes, and the 4 x 16 step
sequencer with its trigger modes. Its program is the instrument's own 128 parameters and 64 sequencer steps, so Poly Evolver and
Evolver program dumps load as they are.

*The name: the Blue Morpho is an iridescent blue butterfly, and to morph is to change form, as the instrument's sounds do.
Morpho-PE is an independent project, not affiliated with or endorsed by Dave Smith Instruments or Sequential. The PE in the
name is a nod to the instrument it is modelled on.*

**Status: development build, offline only.** It builds and passes the offline host test and its own engine tests; it has not
been run on a device yet. See [docs/STATUS.md](docs/STATUS.md).

## How it relates to the original

This is **not an emulation of the instrument's firmware**. It is a new engine in portable C built on the instrument's own data
model and on what its firmware and manual say:

- **The program format is the original's.** 128 parameters in the SysEx order of the manual (p. 66-74) with their ranges and
  meanings, plus the 64 sequencer steps. Program Data, Edit Buffer and Program Name dumps load; the plugin state stores the
  same 192 bytes.
- **Curves come from the DSP firmware.** The DSP 3.5 update holds the voice's lookup tables. Decoded offline
  ([docs/FIRMWARE.md](docs/FIRMWARE.md)), they give the exact oscillator tuning, the LFO rates (0.033 Hz to 261.6 Hz), the
  delay tap lengths, the envelope time curve and the 4-pole Butterworth highpass cutoffs, and they confirm all 128 parameter
  ranges. The engine's curves are formulas fitted to them; no firmware data is in this repository or the plugin.
- **The analog half is modelled, not measured.** The oscillators, lowpass and VCA are circuits on the instrument, so their sound
  here is a circuit-style model, not a calibrated copy: a ramp-core VCO (polyBLEP saw and pulse, polyBLAMP triangle, slight ramp
  bend, slow drift), a lowpass of four OTA integrators that each saturate their own input (zero-delay feedback, resonance from
  stage 4, or stage 2 in 2-pole mode) and a soft-saturating VCA. Written from the published papers (Valimaki and Huovilainen,
  Zavalishin, mystran's nonlinear zero-delay notes); no GPL code is used.

## Your own sounds and waves

The plugin makes a folder called `SYSEX` inside its own folder on first load. Put `.syx` files there (or next to the plugin):

- **Programs:** single program dumps, bank dumps or "all banks" dumps from a Poly Evolver or Evolver. Each bank in a file becomes
  a bank on the PROGRAM page, named after the file.
- **Waveshapes:** the digital oscillators start with an open set of 128 waves of this project's own (wave 95 is blank and 97-128
  repeat 1-32, as on the instrument). The original's waves are not in its firmware files and are not shipped. Three ways to
  bring your own, all read from `SYSEX` (files load in name order; a later file overwrites the same waves):
  - **Waveshape Data dumps** from a Poly Evolver or Evolver (manual p. 56 and 60): each dumped wave replaces its own slot.
  - **Prophet VS wave dumps** (`F0 01 0A 7F`, a VS's 32 RAM waves): they become waves 97-128, the slots the Evolver keeps
    for user waves (the VS's own user waves were 0-31).
  - **Single-cycle WAV banks** (16/24/32-bit or float): one cycle per span between cue points (or every 128 samples without
    cues), resampled to 128 points, filling waves 1-95 in order. A recording includes the instrument's output stage, so it
    is close to the original data, not identical.

Without any files, eight built-in programs of this project's own play on the open waves.

## Using it

Eight tabs, grouped like the instrument's panel sections and in its signal order, with amber flow lines showing the path:
**PROGRAM** (program and bank, misc parameters, sequencer clock and run, sequence destinations), **OSC** (oscillators 1-4, noise
and external input feeding the filter), **FILTER** (low pass filter into the amplifier), **FX** (high pass, tuned feedback,
distortion, delay, output hack, voice volume), **MOD** (envelope 3 and the four LFOs), **MODS** (the four modulators and the fixed
controller routes), **SEQ 1-2** and **SEQ 3-4** (the step sequencer, one Q-Link page per track).

The look takes its cues from the instrument without copying it: an ultramarine plate, brighter rounded blue sections, black
pointer knobs, red LEDs and a grey LCD. The wordmark and every drawing are this project's own.

**Sequencer.** RUN starts the sequencer on voice 1 (Run, or Transport to follow MPC's play button); CLOCK picks the program's
BPM or MPC's tempo. The gated trigger modes (Key Gates Seq and the others the manual marks AUTO) start a sequence on each key,
on every voice, as on the instrument.

**MIDI.** Notes, pitch bend, mod wheel (CC 1), breath (2), foot (4), volume (7), expression (11), brightness (74), sustain (64),
channel and poly pressure, program change, and the instrument's parameter CCs (manual p. 50-51).

## Building

Needs a checkout of [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) next to this repo (`../mpc-vst-plugins`),
Python 3, and Docker for the armhf build.

```
tools/make_layout.sh                                            # params.json, src/patch_tab.h, the skin layout and flow drawings
../mpc-vst-plugins/tools/test_port.sh vst/vst.json              # offline host test (ASan)
../mpc-vst-plugins/tools/build_port.sh vst/vst.json             # armhf .so + skin (Docker; the skin uses the browser renderer)
```

Engine tests: see the first lines of `test/test_engine.c`.

## Licence

MIT (see LICENSE), the same as mpc-vst-plugins.
