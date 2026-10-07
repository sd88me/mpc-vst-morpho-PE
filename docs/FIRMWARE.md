# What the Poly Evolver firmware files hold

Findings from the three public update files (Poly Evolver Keyboard main 2.2, voice 2.2, DSP 3.5), decoded offline on
2026-10-06 with `tools/fw/pe_fw.py`. Nothing here was checked against a running instrument. No firmware bytes, decoded
images or tables are committed: the tool rebuilds every number below from your own files.

## 1. The update format

`F0 01 20 01 <target> <payload> F7`. Target `0x68` is the main CPU, `0x69` the voice CPU and `0x78` the DSP.

The payload uses the manual's "packed MS bit" scheme (7 data bytes per 8 MIDI bytes, the first byte of each group holding
their top bits, bit 0 for the first). The scheme restarts every 1024 decoded bytes, though. Each 1171-byte slice (146 groups
of 8 plus a final group of 3) unpacks to 1024 bytes on its own. Unpacking the payload as one stream gives clean data for
the first kilobyte and apparent noise after it, which looks like encryption but is only a shifted frame. Every image ends
with one extra byte (probably a checksum; not checked).

| File | Target | Decoded | What it is |
|---|---|---|---|
| Main 2.2 | 0x68 | 135 169 bytes | dsPIC (16-bit core, 24-bit instruction words stored as 3 bytes). Reset vector `GOTO 0xFFBC`, interrupt vector table of 3-byte entries, two code regions, the rest erased flash (0xFF) |
| Voice 2.2 | 0x69 | 32 769 bytes | PIC18 (32 KB). `GOTO 0x18` at reset, C start-up code that copies initialised data, main loop at 0x5C6C. The first kilobyte is the application, not a boot loader: the decoder lives in the instrument |
| DSP 3.5 | 0x78 | 61 441 bytes | Analog Devices ADSP-219x boot stream (24-bit instructions, 16-bit data, interrupt vectors 32 words apart) |

The instrument's waveshapes (the 95 Prophet-VS ROM waves and the user waves) are in none of the three files. They live in
the instrument's own memory and come out only through the Waveshape Data dump (manual p. 60), which is how the plugin
takes them (README "Your own sounds and waves").

## 2. The DSP boot stream

32-bit little-endian words. A 24-bit word sits in the top 24 bits; a 16-bit data word in the top 16.

- Header: `0x2f`, `0`, `n`, then `n` (28) words for program memory 0x0000-0x001B.
- Then records of two words: `addr << 16 | flags` and `count << 16`. Flag bit 2 means zero-fill (no data follows); flag bit 0
  means 16-bit data. DSP 3.5 has 74 records after the header; the stream then ends (two 16-bit words at 0x97FE are its last data).
- Memory map: interrupt vectors at 0x0000-0x01FF (4 instructions each, 32 words apart), code from 0x0200 to 0x17C3,
  tables from 0x1800 to 0x2750 in the 24-bit block, the rest of the 24-bit block and the whole 16-bit block (0x8000-) zeroed.

`pe_fw.py dsp-map` lists the records.

## 3. The voice tables (DSP 3.5 addresses)

| Address | Entries | What it is (by its shape and the manual) | How the engine uses it |
|---|---|---|---|
| 0x1800 | 128 | Maximum value of each program parameter | All 128 agree with the manual (`pe_fw.py check`); confirms External Input Mode has 4 modes (0-3), which the manual's header gives as 0-2 |
| 0x1880 | 64 | Maximum of each sequencer step: 102 on track 1 (Reset and Rest), 101 on tracks 2-4 (Reset) | Same in `src/patch_tab.h` |
| 0x18C0 | 128 | Oscillator phase increments: 23-bit phase at 48 kHz, note 0 = 8.177 Hz, exact equal temperament | `pe_note_hz()`: MIDI note numbers, osc value 0 = C-2 |
| 0x1CD6 | 111 | Envelope step sizes for 0-110 (full scale 2^23); full scale takes 3.3 to 111 848 ticks | `pe_env_seconds()`: log-interpolated breakpoints through the tick counts. The tick rate is not in the tables; 3 kHz is assumed (0.1 ms to 37 s) |
| 0x1E8E | 151 | LFO phase increments for 0-150: round decimal frequencies from 0.0333 Hz (30 s, as the manual says) to 7.7 Hz at 89, then semitones from 8.18 Hz at 90 to 261.6 Hz at 150 | `pe_lfo_hz()`: the same values as breakpoints and a formula |
| 0x201F | 151 | Delay tap length in samples (8 fractional bits) for 0-150: 0-21 samples one per step, then semitones (22 = 22.93 samples = C7, 94 = 1467.7 samples = C1, as in the manual), then round counts 1550 ... 8000, 9000 ... 48 000 (1 s) | `pe_delay_seconds()`: the same rule |
| 0x2369 | 100 x 2 x 5 | Highpass coefficients: two biquads per setting (Q 0.54 and 1.31, a 4-pole Butterworth), stored halved | `pe_hpf_hz()`: measured -3 dB points step one semitone per value, 99 = 21.55 kHz (setting 1 = 71 Hz; the lowest settings are coefficient-quantised) |
| 0x1A70, 0x1C04 | ~100 each | Linear 0-100 to 0-1.0 scalings | Implied by the engine's /100 scaling |
| 0x1C6F | ~40 | Reciprocals 1/n | Not needed |
| 0x2080, 0x20B8 | | Sample counts and reciprocals for tempo and sync arithmetic (48 000 at the top) | The engine computes sync times from the tempo |
| 0x1940, 0x19C0, 0x1D47, 0x1E90 (part), 0x2240, 0x2300 | | Further exponential and level curves: semitone-spaced periods (likely the tuned feedback), a curve to 0x7FFF (likely the filter or VCA control voltage), an exponential with a slowly changing ratio (100 entries) | Not yet identified: needs the code that reads them (section 4) |

## 4. What it would take to go further

- **Read the DSP code.** With the *ADSP-219x DSP Instruction Set Reference* (Analog Devices, 82-000390-07), a disassembler
  for the 24-bit words is a few hundred lines. It would identify the remaining tables, the envelope tick rate, the
  modulation depth per destination, the feedback and delay paths, the distortion curve and noise gate, and the output hack.
  Done 2026-10-06: `tools/fw/adsp219x_dis.py` (section 6).
- **Run the DSP as a reference.** An ADSP-219x interpreter running DSP 3.5 offline would do for the digital half what the
  Microwave firmware did for Clementine-XT: render oscillators 3/4, envelopes, LFOs, the highpass, feedback, delay and
  distortion for comparison. The voice CPU (PIC18) sends it parameters over a host port that would need emulating as
  well, or faking from the code. Running it on the device is out of reach: one 160 MIPS DSP per voice.
- **The analog half can't come from firmware.** Oscillators 1/2, the lowpass and the VCA are circuits driven by control
  voltages, so they need measurements of a real instrument (recordings of single oscillators, filter sweeps at several
  resonances, envelope timings).

## 5. Prophet VS wave formats (2026-10-06, offline, from user-supplied files that stay local)

- **VS wave dump** (`F0 01 0A 7F`, 12 288 nibbles, `F7`): read as nibble pairs (high first) it is 6144 bytes, 32 waves of 192
  bytes. Each wave is 128 bytes of the samples' top 8 bits (offset binary, so 0x80 is zero) followed by 64 bytes holding the
  128 low nibbles: 12-bit samples, as the manual says of the ROM waves. Which low nibble of a byte belongs to the even sample
  is not settled (no smoothness test separates them; the difference is under 1/256 of full scale).
- **VS program bank** (`F0 01 0A 64`, 16 400 nibbles): a VS sound bank. Not loadable: a VS program has nothing in common with
  an Evolver program.
- **A single-cycle WAV** of VS waves (48 kHz float, 104 cycles of about 366.6 samples marked by cue points) matched none of
  the 64 dumped RAM and ROM-cartridge waves, so it most likely holds the internal ROM waves; which cycle is which VS wave
  number is not known yet.

## 6. The DSP code (2026-10-06, `tools/fw/adsp219x_dis.py`)

The disassembler decodes the whole image (5 725 words, no unknown words) from the ISR's chapter 8. Structure found with it:

- **Frame loop.** A serial-port interrupt (vector 0x00C0) shifts the 8-slot output frame out and sets a flag (DM 0xF78C) once
  per frame, at the 48 kHz sample rate (the delay table's 48 000 = 1 s). The main loop (0x0E02) waits for it, runs the per-sample
  audio (0x0200), then **one of four stages** (the address in DM 0xF7A7 steps 0x0E0A, 0x0E4E, 0x0E93, 0x0F40). Envelopes and
  LFOs therefore update once per four samples: **12 kHz**. Check: the LFO phase is a 32-bit word that resets on signed overflow
  (a 2^31 cycle) and adds the 0x1E8E entry times 4 per update: entry 0 gives 12 000 x 4 x 1491 / 2^31 = 0.03333 Hz, the
  manual's 30 s.
- **Envelope** (routine 0x071D, three calls: filter, amp, env 3; stage flags 32 gate, 2 attack, 4 decay, 8 sustain). All use a
  rate index = parameter - modulation (accumulator >> 9), limited to 0-110.
  - Attack adds the 0x1C67 entry x 16 to a 32-bit level each tick: a **linear ramp**. The entries are 2^25 / (3 n) with n =
    1, 2 ... 10, 12 ... 30 ... 100 ... 10 000 at 100 ... 44 739 at 110: **round millisecond counts** (full scale takes 12 n ticks
    = n ms), which is what fixes the tick rate. In the exponential shape (DM 0xF67A = 0) the level is a **curve of the ramp**,
    the 128-entry table at 0x1D45 (reverse-searched when a note retriggers): (1 - e^(-1.3 x)) / (1 - e^(-1.3)), x = (i + 1) / 128,
    within 0.6 % of full scale of every entry.
  - Exponential decay: every tick the level closes (0x1CD6 entry) / 2^23 of its distance to the sustain level (time constant
    2^23 / entry ticks: 0.28 ms at 0, 9.3 s at 110; 19 breakpoints, within 4 %). Release does the same to zero with the entry /
    2^25 (4x slower) and ends when the high word reaches 0. (First read as 2^24 / 2^26: the multiplier runs in fractional mode and
    doubles its product, which the simulator below showed.)
  - Linear shape (DM 0xF67A != 0): decay subtracts the attack-table slope (full scale in n ms), release a quarter of it (4 n ms).
- The old guesses (3 kHz, 1.3 overshoot, 4.6 time constants) are replaced in `src/curves.c` and `env_tick`. The Env 3 delay stage
  was not located: it still reads the attack table (a guess).
- **Modulation** is summed per destination into 16-bit accumulators (DM[0xF812 + destination number], swapped each frame; group
  destinations fan out at 0x16D5-0x17BA) and read back by the consumers (the stage code reads the previous frame's block at
  DM[0xF813 + destination]). Source depth: the table at 0x1DC5 maps an LFO amount 0-100 to 0, 50, 100, 150, 200, 300, 400, 600, 800, 1100,
  1500, 1900, 2300, 2800, 3300, 3700 (0-15), then 256 x amount (25 600 at 100); a -99..+99 amount (table at 0x1B3C) is 256 a step to
  +-72, then 512, reaching +-32 767 at 99. The multiplier runs in fractional mode (M_MODE is never set in the code except around
  one 32-bit multiply), so a full-scale source gives the depth itself in accumulator units. Units per destination step, from the code
  that reads each accumulator: pitch 512 a semitone (50 at amount 100), level / FM / RM parameters 326 (the parameter times 163,
  doubled: 78.5 steps), pulse width 331 with the accumulator doubled (155), LFO frequency and amount 256 (100), envelope rates 512 (50;
  a positive amount shortens the time), feedback frequency 512 (50). `DR` in `src/engine.c` holds these; filter frequency, delay,
  pan, VCA level, distortion and the sequencer destinations are not traced (the filter's goes through calibration tables the voice
  CPU fills in, so it can only be read together with a calibration).
- 0x19F0-0x1A71 are 2^(k/12) ratio tables used at boot (0x050E) to build the 64-entry control-voltage tables for the analog
  oscillators and filter from the voice CPU's calibration values: they cannot give a cutoff in Hz without that calibration.
- Output hack: the table at 0x2010 holds the masks 0xFFFF, 0xFFFE, 0xFFFC ... 0xC000 (hack 0-14); the stereo output words are ANDed with
  the mask of the setting (2 bits kept at 14): what the engine already did (`floor(x * 2^(15 - hack))`).
- Tuned feedback: 0x19C0 is its period table (read at 0x0248: 12 semitone entries, linear interpolation, octave folding by
  subtracting 0x0C00); 0x1940 feeds the oscillator increment code at 0x1200.

## 7. The interpreter (2026-10-06, `tools/fw/adsp219x_sim.py`)

A rig that runs one routine of the DSP image on a memory state you set up (no peripherals, interrupts or secondary registers;
fractional/integer multiplier mode, saturating ALU mode, circular buffers, delayed branches and loops are modelled). It
reproduces the envelope routine (0x071D) exactly: with state at DM 0xF801 (flags, level high word 0xF802, low word 0xF803),
parameters at 0xF634 (attack, decay, sustain, release) and DM 0xF67A (0 exponential, 1 linear), measured on 2026-10-06:
- linear attack index 0 / 10 / 50: 12 / 144 / 3600 ticks (1 / 12 / 300 ms at 12 kHz), linear decay index 50: 3600 ticks, linear
  release index 50: 14 398 ticks (4x);
- exponential attack index 50: 3572 ticks, level 0.22 / 0.66 of full scale at 1/8 and 1/2 of the time (the curve above gives 0.21 / 0.66);
- exponential decay to 1/e: 221 / 1311 / 4414 ticks at index 20 / 50 / 80, release 886 / 5243 ticks at 20 / 50: time constant 2^23 / entry
  ticks (corrected from 2^24, section 6), release 4x.

## 8. The voice CPU (2026-10-06, `tools/fw/pic18_dis.py`)

The PIC18 image disassembles with a short PIC18 decoder (160 `.word` entries of 16 000: the data tables). It is compiled C driven by
a timer: Timer 2 (T2CON 0x49: prescale 1:4, postscale 1:10, PR2 = 50, so one tick every 2040 instruction clocks) sets a flag; a counter
(0..59, DM 0x237) hands out the work, and the glide routine (0x2E1C) is called at counts 2, 7, 12 ... and 4, 9, 14 ... (twice every five
ticks, each call stepping all four oscillators). The clock frequency is not in the file; 40 MHz (10 MHz crystal x 4) is assumed, which
makes a tick 204 us and a glide step every 510 us.
- **Glide.** The target pitch is the note number in the high byte (8.8: 256 per semitone). Each step moves the current pitch by a byte
  R from the table at 0xF6E (index = glide setting, or setting - 99 for the fingered range; 255 at 0 and 1, 250, 240 ... 170 at 10, 205 at
  11, 120 at 21, 90 at 31, 50 at 51, then -1 a step to 1 at 100). So a glide of g takes 12 x 256 / R steps per octave: 6 ms at 1-2, 1.6 s
  at 99, and the table's non-monotonic spot at 10-11 is the firmware's own. `pe_glide_seconds` in `src/curves.c` uses the table's
  breakpoints and the 40 MHz assumption (a 20 MHz clock would double every time).
- Other tables: 0xB1A, 0xBE2 and 0xD4E (100-entry 16-bit tables, high byte first) are the voice CPU's other 0-100 curves, not yet
  attributed.

## 9. Distortion, noise gate, grunge (2026-10-06, interpreter measurements)

Parameter p of the program is at DM 0xF622 + p (osc 1 frequency 0xF622, filter frequency 0xF632, distortion 0xF689 ...).
- **Distortion** (setting 1-99 on the output; 100-199 is the same before the filter, not modelled): routine 0x06AD turns the setting into a
  gain index (the 99-entry table at 0x22FD plus the modulation accumulator / 8); 0x06C7 multiplies and **clips hard at full scale**. Gain =
  index / 16: 1 at setting 0, 2.3 at 3, 7.8 at 9, 32 at 20, 85 at 31, 197 at 44, 415 at 59, 868 at 78, 1659 (64 dB) at 99 (11 breakpoints
  within 4 %, `pe_dist_gain`). Setting 1 runs only the noise gate; the gate is keyed from the left channel before the distortion.
- **Noise gate** (0x06D5): open while the sample magnitude is at least 122/32768 (-48.6 dBFS), then held for 2048 samples (at 48 kHz) after the
  last such sample; then the gain falls to 0 at once, unless the peak seen since it opened is at most 409/32768, in which case it fades
  linearly over 8192 samples from (55 x peak + 1792) / 32768. Measured by feeding the routine samples (`adsp219x_sim.py`).
- **Grunge** (DM 0xF644 bit 0): the mixer's accumulation of oscillators 3/4, the tuned-feedback line and the delay feedback runs at 0x0376 with
  `SAT MR` after each multiply-accumulate, or with grunge at 0x0395 without it: the sum wraps round at full scale instead of clipping.
- Env 3 delay (parameter 112) is not read anywhere in the DSP image; it is in the voice CPU (section 10).

## 10. Voice CPU: Env 3 delay (2026-10-06)

The voice CPU's parameter RAM holds program parameter p at 0x15E + p (osc 1 glide, p 64, at 0x19E; key mode, p 71, at 0x1A5; Env 3 delay,
p 112, at 0x1CE). The delay (0x3B58) is a 16-bit count looked up in the 101-entry table at 0xB19 (0 ... 10 in steps of 1, 12 ... 30 by 2, 33 ...
60 by 3, ... 100 at 40, 150 at 50, 250 at 60, 550 at 90, 740 at 100) and counted down at 0x5FDC, once every second 60-tick frame (a counter at
0x263 reloaded with 2): 120 ticks = 24.5 ms a count at the assumed 40 MHz clock, so the delay runs from 24 ms (1) to 18 s (100). When it
reaches zero the voice CPU raises the envelope gate. `pe_env_delay_seconds` uses it (previously the attack table, a guess).

## 11. More modulation units (2026-10-06)

Read from the consumers, in accumulator units (an LFO at amount 100 = 25 600): highpass 512 a step (50; added); delay time 512 a table step
(50; **subtracted**, a positive amount shortens the delay); delay level, delay feedback 1, VCA level and VCA envelope amount 326 a step (the
parameter x 163, doubled: 78.5); delay feedback 2 and resonance 256 (table 0x1AD7: 100); pan: the accumulator is added to the left gain and
taken off the right gain of the (L, R) pair for the setting (1.0/0, 0.7/0.3, 0.6/0.4, 0.5/0.5 ... at 0x2002), 0.78 of full scale at 25 600,
about 4.7 positions. Filter frequency: base cutoff CV is 256 a semitone and the modulation enters at x 0.563 (0x4812/32768), so about 56
semitones; the final DAC code goes through calibration the voice CPU provides, so this is an estimate. The unison detune is not in the
voice CPU image (it is in the main CPU's key assignment) and was not traced.

## 12. The main CPU (2026-10-07, `tools/fw/dspic_dis.py`)

The main 2.2 image (dsPIC, 3 bytes per 24-bit word) disassembles with the dsPIC decoder written for the Tempest's voice CPU (mpc-vst-sturm):
code at 0x0100-0x26B6 and 0x5FB0-0xFFFE; the 4 713 words it shows as data are strings and tables kept as 16-bit words in program memory
and read through the PSV window (data address 0x8000 + program address; byte n of that stream is program address n).
- **RAM**: the edit buffer is at 0x10E6 (program parameter p at 0x10E6 + p, the 64 sequencer steps from 0x10E6 + 128); the globals at
  0x0CA8 in the main CPU's own order (Master Fine Tune is 0x0CA8 + 5); the voice mode is key mode / 6 at 0x0BBC (0 poly, 1 mono, 2
  unison 1, 3 unison 2).
- **Unison detune** (routine 0x8B5C, called after a program or key-mode change): for each voice v the main CPU sends the voice CPU global
  11, Master Fine Tune (0-100, 50 = 0 cents; manual p. 62), as master fine tune + table[mode][v], clamped to 0-100. The table (PSV
  0x8844, 4 x 4 words): Poly and Mono 0 0 0 0, **Unison 1 -1 +1 -3 +3 cents, Unison 2 -3 +3 -8 +8 cents** (voices 1-4; the voice
  address table at 0x87FA is 1, 2, 4, 8). The engine had guessed an even spread of up to +-6 / +-15 cents; it now uses the table
  (voices 5-8 repeat it), checked by `test/test_engine.c`.
- **What is not here**: the main CPU reads only a few program parameters itself (trigger 54, tempo 66, clock divide 67, key mode 71 and
  the sequencer steps) and forwards the rest. LP key amount, audio mod and L/R split are never read from the edit buffer, so their
  scales are in the voice CPU or the DSP, not in this image; tempo and clock divide are copied to 0x0CB2/0x0CB3 and sent on (the sync
  arithmetic is the DSP's, section 3). The modulation routing is the DSP's (sections 6 and 11).


## 13. Which Prophet VS wave each cycle of the single-cycle WAV is (2026-10-07)

Anchors from the VS manual's wave list (VS waves 32-34, 45/46, 56, 58, 60, 75/76, 113-127) and a forum list of the Evolver's own names
(Evolver wave = VS wave - 31; wave 96 is Evolver-only; 93 is not named): the WAV cycles whose spectra match their descriptions exactly are
74 and 75 (bell partials, Evolver 82, 83), 76 (saw 3rd and 5th, 84), 77 (two sines an octave and a fifth apart, 85), 78 and 79 (two sines
two and four octaves apart, 86 and 87: harmonics 1+4 and 1+16 to 0.999), 80 and 81 (two saws, 88 and 89) and 82-84 (two squares, 90-92).
So cycle c is Evolver wave c + 8 for c = 74-84, and the engine puts those cycles in slots 82-92. The offset does not hold for the lower
cycles ("3rd and 5th, no fundamental" and "heavy 7th" do not fall where it predicts; the 7th-harmonic cycle is 55), no cycle is blank
(wave 95), and cycles 0 and 92 look alike, so the recording has an order or extras not yet understood. The Prophet VS ROM images (v1.1
and v1.2, a high and a low byte chip): each 32 KB chip is 16 KB of code (different between versions) and 16 KB identical in both (128 blocks
of 128 bytes; block 255 is random bytes, so it is the noise wave 127); the waves in them are not stored as plain consecutive 16-bit samples
(no permutation of the sample address makes them smooth) and could not be decoded.

## 14. The VS ROM chips against Arturia's wave ROM (2026-10-07, user-supplied files kept local)

`resources/roms/waverom.bin` in the Arturia Prophet-VS V installer (an Inno Setup archive; unpacked with innoextract, not run) is 24 320
bytes: 95 waves of 128 little-endian 16-bit words, a 12-bit sample in the top bits (the low nibble of each word is 0). Against the
single-cycle WAV (cycles band-limited to 20 harmonics, best circular correlation, polarity free): cycles 0-86 are ROM waves in
increasing order (0-based index j = cycle + 3 up to cycle 10, then cycle + 4 to 19, then growing as the WAV leaves out ROM waves 0, 2, 14,
24, 26, 54 and 94), mostly 0.95-1.00, a few bright ones 0.76-0.92; cycles 74-84 land on j = 81-91, i.e. Evolver wave j + 1 = 82-92, as
section 13 found. Cycles 87-103 are not in order: some repeat ROM waves (91 is j = 1; 92-95 are 3, 5, 6, 7) and some match nothing well.

The chip images (the v1.1 and v1.2 MSB/LSB files): the upper 16 KB of every chip is identical in both versions. In the MSB chip's upper
half, blocks 0-55 (128 bytes each) are a smooth table, a constant per block rising from 2 to 154 (a curve, not waves); blocks 56-127 are
72 blocks that look like waveforms; the LSB chip's upper half is near random (7.85 bits of entropy per byte, consistent with packed low
nibbles). Those 72 blocks match none of Arturia's 95 waves: not as byte multisets (also with the data inverted or offset by 0x80), not
as a count of ones per data line (invariant to any address and data-line permutation, inverted or not), not as normalised circular
correlation of the top bytes (best 0.3-0.9, nothing unique). So Arturia's ROM is not a plain copy of these top bytes (resampled or
re-quantised, or the chip packs the samples in a form not found yet). The layout remains undecoded; the WAV mapping above does not need it.

Loader (src/engine.c, `scan_dir`): cycle k goes to wave slot k + 3 (k <= 10), k + 4 (11-19), 25 (k = 20), k + 6 (21-47), k + 7 (48-86),
all 0-based; cycle 91 goes to slot 1. The formula agrees with the best match of all 87 cycles (score at least 0.95 at 20 harmonics).
Slots with no cycle (0-based 0, 2, 14, 24, 26, 54 and 94) keep their stand-ins. The file is the Morphagene collection (its metadata:
REAPER, 2019), so it is a capture of the waves, not the ROM data itself.
