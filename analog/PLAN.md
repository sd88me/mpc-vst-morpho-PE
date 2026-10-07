# Status and plan
Done (2026-10-07):
1. Oscillator, OTA cascade, half-band decimators and tanh from Morpho-PE (null-tested: bit-identical render hash).
2. Sturm's DCO pair (exact sub-sample reset timing, hard sync, sub oscillator) moved in unchanged; Sturm's render hash is identical.
3. Sturm's low-pass replaced by `ma_ota2x` (2x, zero-delay feedback, half-band decimation): self-oscillation within 0.2 semitone of
   the set cutoff from 65 Hz to 15 kHz (the old filter was up to 2.5 semitones off at the top). The cost is about 35 % more per
   filter sample on x86 (116 vs 85 ns, mostly the `tanf` for the cutoff and the 12-tap half-band): measure on the Force.
Open:
- Decided: the engine stays in this repo (`analog/`), no separate repository; Sturm (pushed) keeps a synced copy of the header.
- Cheaper cutoff (a table or a rational tan) if the device cost matters.
- Morpho-PE could use `ma_ota2x`'s tuning compensation too (its 4-pole oscillates about 1 semitone flat at low cutoffs); it would change its output.
