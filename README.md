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

**Status: development build.** It builds, passes the offline host test and its own engine tests, and has been installed and
CPU-benched on a Force (it is not yet fully tested by ear on the device). See [docs/STATUS.md](docs/STATUS.md) for the current
state, the bench numbers and the open items.

## Features

- **Voices:** 1-8 selectable (default 4), each with the instrument's two analog-style oscillators (saw, triangle, saw-triangle,
  pulse width 0-99 that switches off at the extremes) and two digital waveshape oscillators, with hard sync, FM and ring
  modulation both ways between the digital pair, per-oscillator shape sequencing, noise and slop.
- **Filter and amplifier:** stereo (left/right) 2- or 4-pole resonant lowpass with envelope, velocity, key tracking, audio
  modulation and split; VCA with envelope and velocity; the seven output pan modes.
- **FX section:** tuned feedback with grunge, 4-pole highpass (pre/post is not modelled, see below), distortion with noise gate,
  three-tap delay with both feedback paths and tempo-synced times, output hack.
- **Modulation:** three envelopes (env 3 with delay; exponential or linear), four LFOs (synced rates, key sync), four modulation
  slots, the seven fixed controller routes, all 68 destinations.
- **Sequencer:** the 4 x 16 step sequencer with rests, resets, swing, clock modulation and every trigger mode.
- **Keyboard:** poly, mono and unison 1/2 with the six key priorities and per-oscillator glide (normal, fingered, keyboard off).
- **Program format:** the instrument's own 128 parameters and 64 sequencer steps, so its program, bank, edit-buffer and waveshape
  dumps load unchanged; banks of programs from `.syx` files; Prophet VS wave dumps and single-cycle WAV banks as wave sources.
- **Plugin:** 221 parameters, an eight-tab skin and Q-Link pages, MIDI CCs, project state, a **Quality** control (analog section at
  1x, 2x or 4x oversampling).

## Technology

- **Engine:** portable C (`src/engine.c`, about 1 300 lines, plus `curves.c`, `waves.c`, `syx.c`, `presets.c`), 44.1 kHz,
  control rate every 8 samples, audio at the host block size. No dynamic allocation on the audio thread; flush-to-zero set on
  ARM and x86.
- **Analog section** (oscillators 1/2, mixer, lowpass, VCA), run oversampled with Kaiser half-band decimators:
  - **Oscillator:** a ramp-core VCO in the manner of a CEM3340: band-limited saw and pulse (polyBLEP), triangle with polyBLAMP
    corners, a slight ramp bend, slow thermal drift, band-limited hard sync.
  - **Lowpass:** four OTA integrators in cascade in the manner of a CEM3320, each limiting its own input, zero-delay-feedback
    trapezoid stages with the tanh gain taken from the previous sample, resonance from stage 4 (4-pole) or stage 2 (2-pole), so
    only the 4-pole mode self-oscillates, as on the instrument.
  - **VCA:** an OTA that saturates softly. Written from the literature (Valimaki and Huovilainen; Zavalishin, *The Art of VA
    Filter Design*; mystran's nonlinear zero-delay notes); no GPL code. Compared offline with the OB-Xd 4-pole algorithm.
- **Digital section** (oscillators 3/4, highpass, feedback, distortion, delay, hack) at the base rate, as the instrument's DSP runs
  it, including the DSP's own behaviour where the firmware shows it: hard-clipping distortion, the noise gate, grunge as integer
  wrap-around, bit-mask output hack.
- **Skin:** a layout file (`tools/gen_layout.py`) rendered by a headless browser (`"art": "html"`), signal-flow drawings written by
  the generator, eight tabs, Q-Link pages per section.

## How it was derived, and how close it is

The instrument's three update files (main CPU, voice CPU, DSP) are public. They were decoded offline with tools in `tools/fw/`
(an update-format decoder, an ADSP-219x disassembler and interpreter, PIC18 and dsPIC disassemblers) and read against the manual.
**Nothing from them is stored here**: the engine holds formulas and short breakpoint lists fitted to what they contain, each
documented with its numbers in [docs/FIRMWARE.md](docs/FIRMWARE.md); the tools re-derive everything from your own files.

| Part | Source | Closeness |
|---|---|---|
| Program format, parameter ranges | Manual + the DSP's range table (all 128 agree) | Exact |
| Oscillator tuning, delay taps, LFO rates, highpass cutoffs | DSP tables | Exact (fitted formulas) |
| Envelopes: tick rate (12 kHz), linear attack, exponential decay/release, attack curve, linear shape | DSP code, run in an ADSP-219x interpreter | Exact to the tables' precision |
| Modulation: amount curves and depth of pitch, levels, FM/RM, pulse width, LFO, envelope rates, highpass, delay, pan, VCA, feedback | DSP code | Exact; filter-frequency depth is an estimate (56 semitones) |
| Distortion gain and hard clip, noise gate, grunge, output hack | DSP code, interpreter | Exact |
| Glide, Env 3 delay | Voice-CPU tables and timer | Exact in shape; times assume a 40 MHz clock (not in the file) |
| Unison detune (-1/+1/-3/+3 and -3/+3/-8/+8 cents) | Main-CPU table | Exact |
| Oscillator 1/2, lowpass and VCA sound | Circuit-style models from the literature | **Modelled, not measured** |
| Digital waves | The VS single-cycle recording you supply | Cycles 0-86 matched to the VS ROM waves by correlation (0.95-1.00); 7 slots not in the recording |

## What is missing

- **The analog character is a model.** The real oscillators, filter and VCA are calibrated analog circuits; their exact
  cutoff-to-Hz curve, resonance and self-oscillation level, drive, drift, left/right differences and VCA response need
  recordings of a real instrument. The OTA limiting levels were set so the filter is clean at normal levels and self-oscillates near 0.5 (compared with OB-Xd), not measured on the instrument.
- **The original's waves are not included** and could not be decoded from the Prophet VS ROM images; the Morphagene-collection WAV's
  cycles 0-86 were matched to the VS ROM waves (Arturia's wave ROM, kept local) and each goes to its slot; the recording has no cycle for
  waves 1, 3, 15, 25, 27, 55 and 95 (cycle 91 supplies wave 2), which keep stand-ins (docs/FIRMWARE.md sections 13-14).
- **Removed:** the external audio input. Its parameters stay in the program (so dumps load and re-save intact, marked "unused") but
  it has no controls, no sound and no peak / envelope-follower sources; the Ext In trigger modes act like their keyboard counterparts.
- **Not modelled:** distortion and highpass
  placed before the filter (settings 100-199), the sequencer's MIDI-out destinations (MPC takes no MIDI from a VST), and the
  oscillators' per-unit calibration.
- **Estimated or assumed:** filter-frequency modulation depth, the 40 MHz voice-CPU clock, the audio-mod, split and key-tracking
  scales (read by the voice CPU or DSP, not yet traced).
- **Device status:** installed and CPU-benched on a Force (about 17 % of a block for four voices at 1x oversampling, about 31 % at
  2x); a Q-Link sweep still shows rare spikes, and the on-device listening tests (all tabs, Q-Links, project save/reload, the
  SYSEX folder) are not done. Gen 2 hardware and MPC OS 2.x are untested.

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
    cues), resampled to 128 points, each cycle going to the slot it was matched to (cycles 0-86, plus 91; docs/FIRMWARE.md section 14). A recording includes the instrument's output stage, so it
    is close to the original data, not identical.

Without any files, eight built-in programs of this project's own play on the open waves.

## Using it

Eight tabs, grouped like the instrument's panel sections and in its signal order, with amber flow lines showing the path:
**PROGRAM** (program and bank, misc parameters, sequencer clock and run, sequence destinations), **OSC** (oscillators 1-4 and noise
feeding the filter), **FILTER** (low pass filter into the amplifier), **FX** (high pass, tuned feedback,
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
