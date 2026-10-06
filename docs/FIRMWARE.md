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
  - Exponential decay: every tick the level closes (0x1CD6 entry) / 2^24 of its distance to the sustain level (time constant
    2^24 / entry ticks: 0.56 ms at 0, 18.6 s at 110; 19 breakpoints, within 4 %). Release does the same to zero with the entry /
    2^26 (4x slower) and ends when the high word reaches 0.
  - Linear shape (DM 0xF67A != 0): decay subtracts the attack-table slope (full scale in n ms), release a quarter of it (4 n ms).
- The old guesses (3 kHz, 1.3 overshoot, 4.6 time constants) are replaced in `src/curves.c` and `env_tick`. The Env 3 delay stage
  was not located: it still reads the attack table (a guess).
- **Modulation** is summed per destination into 16-bit accumulators (DM[0xF812 + destination], swapped each frame; group
  destinations fan out at 0x16D5-0x17BA) and read back by the consumers with a per-destination unit. Source depth: the table at
  0x1DC5 maps an amount 0-100 to 0, 50, 100, 150, 200, 300, 400, 600, 800, 1100, 1500, 1900, 2300, 2800, 3300, 3700 (0-15), then
  256 x amount (4096 at 16 ... 25 600 at 100); amounts 101-200 (LFO/negative) repeat it. The routing code multiplies it by the source
  (assumed fractional multiply: amount 100 = 25 600 units). Units per step seen so far: LFO frequency 256 (so 100 steps at amount
  100), envelope rates 512 (50 steps; a positive amount shortens the time), filter cutoff 163 (157 steps). `DR` in `src/engine.c`
  uses these three; the other destinations are not traced yet.
- 0x19F0-0x1A71 are 2^(k/12) ratio tables used at boot (0x050E) to build the 64-entry control-voltage tables for the analog
  oscillators and filter from the voice CPU's calibration values: they cannot give a cutoff in Hz without that calibration.
- Tuned feedback: 0x19C0 is its period table (read at 0x0248: 12 semitone entries, linear interpolation, octave folding by
  subtracting 0x0C00); 0x1940 feeds the oscillator increment code at 0x1200.
