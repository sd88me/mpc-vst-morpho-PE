/* The digital oscillators' 128 waveshapes of 128 samples. The plugin builds an open set of its own (additive spectra); the
 * instrument's ROM waves are not in its firmware files and are never shipped, but a user's own Waveshape Data dumps
 * (F0 01 20 01 0A <n> <293 packed bytes> F7, e.g. from "Request Waveshape Dump") replace slots as they are loaded. */
#pragma once
#include <stdint.h>
#define PE_WLEN 128
#define PE_NWAVES 128
void waves_open(float w[PE_NWAVES][PE_WLEN]);
/* Decodes a waveshape dump body (after the 0x0A command byte); returns the slot 0..127 or -1. 12-bit ROM-style data
 * (|x| <= 2048) is scaled as 12 bit, anything larger as 16 bit. */
int waves_from_dump(const uint8_t *body, int n, float out[PE_WLEN]);
/* A Prophet VS wave dump (F0 01 0A 7F, then 12288 nibbles, F7): 32 waves of 192 bytes, each 128 high bytes (offset binary)
 * followed by 64 bytes of low nibbles, so 12-bit samples. msg is the whole message from F0; returns waves decoded (32) or 0. */
int waves_from_vs_dump(const uint8_t *msg, size_t len, float out[32][PE_WLEN]);
/* Single-cycle waves from a WAV file (16/24/32-bit PCM or 32-bit float, first channel). Cycles are the spans between cue
 * points; without cues the file must be whole multiples of 128 samples. Each cycle is resampled to 128 points and
 * normalised to a peak of 1. Returns the number of cycles written (at most max). */
int waves_from_wav(const uint8_t *buf, size_t len, float (*out)[PE_WLEN], int max);
