#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "waves.h"
#include "syx.h"

static void add(float *w, int h, float a, float ph) {
    for (int i = 0; i < PE_WLEN; i++) w[i] += a * sinf(6.2831853f * h * i / PE_WLEN + ph);
}
static void norm(float *w) {
    float m = 0;
    for (int i = 0; i < PE_WLEN; i++) m = fmaxf(m, fabsf(w[i]));
    if (m > 0) for (int i = 0; i < PE_WLEN; i++) w[i] /= m;
}
static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

void waves_open(float w[PE_NWAVES][PE_WLEN]) {
    memset(w, 0, sizeof(float) * PE_NWAVES * PE_WLEN);
    for (int k = 0; k < 96; k++) {
        float *x = w[k];
        if (k == 0) add(x, 1, 1, 0);
        else if (k == 1) for (int h = 1; h <= 40; h++) add(x, h, 1.0f / h, 0);
        else if (k == 2) for (int h = 1; h <= 40; h += 2) add(x, h, 1.0f / h, 0);
        else if (k == 3) for (int h = 1; h <= 40; h += 2) add(x, h, 1.0f / (h * h), (h & 2) ? 3.14159265f : 0);
        else if (k < 24) {          /* one formant peak walking up the harmonics */
            float c = (float)(k - 2), bw = 1.5f + 0.15f * k;
            for (int h = 1; h <= 48; h++) add(x, h, expf(-(h - c) * (h - c) / (2 * bw * bw)) + 0.15f / h, 0);
        } else if (k < 48) {        /* power-law spectra from dark to bright, with alternating sign patterns */
            float p = 2.2f - (k - 24) * 0.08f;
            int pat = k % 4;
            for (int h = 1; h <= 48; h++) add(x, h, powf((float)h, -p) * ((pat == 1 && h % 2 == 0) ? 0.2f : 1), pat == 2 && (h & 1) ? 3.14159265f : 0);
        } else if (k < 72) {        /* pulse-like spectra at swept duty cycles */
            float d = 0.03f + (k - 48) * 0.019f;
            for (int h = 1; h <= 48; h++) add(x, h, sinf(3.14159265f * h * d) / h, 0);
        } else if (k < 94) {        /* seeded random spectra (fixed: the same set every time) */
            uint32_t s = 0x5EED0000u + (uint32_t)k;
            for (int h = 1; h <= 40; h++)
                add(x, h, (lcg(&s) % 1000) / 1000.0f / sqrtf((float)h), (lcg(&s) % 6283) / 1000.0f);
        } else if (k == 95) {       /* wave 96: a folded sine of our own (the original's slot 96 is unique to the instrument) */
            for (int i = 0; i < PE_WLEN; i++) x[i] = sinf(2.5f * sinf(6.2831853f * i / PE_WLEN));
        }
        /* k == 94 (wave 95) stays blank, as on the instrument */
        norm(x);
    }
    for (int k = 96; k < 128; k++) memcpy(w[k], w[k - 96], sizeof w[k]);
}

int waves_from_dump(const uint8_t *body, int n, float out[PE_WLEN]) {
    if (n < 2) return -1;
    int slot = body[0];
    uint8_t raw[260];
    if (syx_unpack(body + 1, n - 1, raw, sizeof raw) < 256 || slot > 127) return -1;
    int16_t v[PE_WLEN];
    int big = 0;
    for (int i = 0; i < PE_WLEN; i++) {
        v[i] = (int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
        if (v[i] > 2048 || v[i] < -2048) big = 1;
    }
    for (int i = 0; i < PE_WLEN; i++) out[i] = v[i] / (big ? 32768.0f : 2048.0f);
    return slot;
}

int waves_from_vs_dump(const uint8_t *m, size_t len, float out[32][PE_WLEN]) {
    if (len < 4 + 12288 + 1 || m[0] != 0xF0 || m[1] != 0x01 || m[2] != 0x0A || m[3] != 0x7F) return 0;
    const uint8_t *nb = m + 4;
    for (int i = 0; i < 12288; i++) if (nb[i] > 15) return 0;
    for (int w = 0; w < 32; w++) {
        const uint8_t *b = nb + w * 384;           /* 192 bytes as nibble pairs, high nibble first */
        for (int k = 0; k < PE_WLEN; k++) {
            int hi = (b[2 * k] << 4) | b[2 * k + 1];
            int lb = (b[256 + 2 * (k / 2)] << 4) | b[256 + 2 * (k / 2) + 1];
            int lo = (k & 1) ? (lb & 15) : (lb >> 4); /* the order of the two low nibbles is not known; it moves a sample by < 1/256 */
            out[w][k] = (((hi << 4) | lo) - 2048) / 2048.0f;
        }
    }
    return 32;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int cmpu(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

/* frame f of the first channel, as -1..1 */
static float wav_sample(const uint8_t *data, int fmt, int bits, int ch, uint32_t f) {
    const uint8_t *p = data + (size_t)f * ch * (bits / 8);
    if (fmt == 3) { float v; memcpy(&v, p, 4); return v; }
    if (bits == 16) return (int16_t)(p[0] | p[1] << 8) / 32768.0f;
    if (bits == 24) return (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 8388608.0f;
    return (float)(int32_t)rd32(p) / 2147483648.0f;
}

int waves_from_wav(const uint8_t *buf, size_t len, float (*out)[PE_WLEN], int max) {
    if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return 0;
    int fmt = 0, ch = 0, bits = 0;
    const uint8_t *data = NULL;
    uint32_t dlen = 0, ncue = 0, cue[257];
    for (size_t i = 12; i + 8 <= len;) {
        uint32_t sz = rd32(buf + i + 4);
        const uint8_t *c = buf + i + 8;
        if (sz > len - i - 8) sz = (uint32_t)(len - i - 8);
        if (!memcmp(buf + i, "fmt ", 4) && sz >= 16) { fmt = c[0] | c[1] << 8; ch = c[2] | c[3] << 8; bits = c[14] | c[15] << 8; if (fmt == 0xFFFE && sz >= 26) fmt = c[24] | c[25] << 8; }
        else if (!memcmp(buf + i, "data", 4)) { data = c; dlen = sz; }
        else if (!memcmp(buf + i, "cue ", 4) && sz >= 4) {
            uint32_t n = rd32(c);
            for (uint32_t k = 0; k < n && ncue < 256 && 4 + 24 * k + 24 <= sz; k++) cue[ncue++] = rd32(c + 4 + 24 * k + 20);
        }
        i += 8 + sz + (sz & 1);
    }
    if (!data || ch < 1 || !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) return 0;
    uint32_t frames = dlen / (uint32_t)(ch * bits / 8);
    uint32_t edges[258];
    int ne = 0;
    if (ncue) {
        qsort(cue, ncue, sizeof cue[0], cmpu);
        if (cue[0] > 0) edges[ne++] = 0;
        for (uint32_t k = 0; k < ncue; k++) if (cue[k] <= frames && (!ne || cue[k] > edges[ne - 1])) edges[ne++] = cue[k];
        if (edges[ne - 1] < frames && frames - edges[ne - 1] >= 16) edges[ne++] = frames;
    } else {
        if (frames % PE_WLEN) return 0;
        for (uint32_t k = 0; k <= frames / PE_WLEN && ne < 258; k++) edges[ne++] = k * PE_WLEN;
    }
    int n = 0;
    for (int k = 0; k + 1 < ne && n < max; k++) {
        uint32_t a = edges[k], L = edges[k + 1] - a;
        if (L < 16) continue;
        float m = 0, *w = out[n];
        for (int j = 0; j < PE_WLEN; j++) {
            float p = (float)j * L / PE_WLEN;
            uint32_t i0 = (uint32_t)p, i1 = i0 + 1 < L ? i0 + 1 : i0;
            float x0 = wav_sample(data, fmt, bits, ch, a + i0), x1 = wav_sample(data, fmt, bits, ch, a + i1);
            w[j] = x0 + (p - i0) * (x1 - x0);
        }
        float mean = 0;
        for (int j = 0; j < PE_WLEN; j++) mean += w[j];
        mean /= PE_WLEN;
        for (int j = 0; j < PE_WLEN; j++) { w[j] -= mean; m = fmaxf(m, fabsf(w[j])); }
        if (m > 0) for (int j = 0; j < PE_WLEN; j++) w[j] /= m;
        n++;
    }
    return n;
}
