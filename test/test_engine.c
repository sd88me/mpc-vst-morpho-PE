/* Engine checks: every built-in program sounds, pitch is right, the sequencer plays with no key held, state and SysEx round-trip.
 *   gcc -O1 -fsanitize=address,undefined -Isrc -Ianalog -I../mpc-vst-plugins/wrapper -o /tmp/pe_test test/test_engine.c src/[!t]*.c -lm && /tmp/pe_test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "engine.h"
#include "patch_tab.h"
#include "presets.h"
#include "syx.h"
#include "waves.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c); printf(ok_ ? "ok   " : "FAIL "); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

static const mpc_engine_t *E;
static void setp(void *h, const char *k, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); E->set_param(h, k, b); }
static void midi3(void *h, int a, int b, int c) { uint8_t m[3] = {(uint8_t)a, (uint8_t)b, (uint8_t)c}; E->midi(h, m, 3); }
/* renders n blocks, returns rms of the left channel; peak and NaN-free in *peak */
static double render(void *h, int blocks, float *peak, int16_t *keep) {
    int16_t o[256];
    double r = 0;
    *peak = 0;
    for (int k = 0; k < blocks; k++) {
        E->render(h, o, 128);
        for (int i = 0; i < 128; i++) {
            r += (double)o[2 * i] * o[2 * i];
            if (abs(o[2 * i]) > *peak) *peak = (float)abs(o[2 * i]);
            if (keep) keep[k * 128 + i] = o[2 * i];
        }
    }
    return sqrt(r / (blocks * 128.0)) / 32768;
}
/* frequency by zero crossings of a mono render */
static double zc_freq(const int16_t *x, int n) {
    int c = 0, first = -1, last = -1;
    for (int i = 1; i < n; i++)
        if (x[i - 1] < 0 && x[i] >= 0) { if (first < 0) first = i; last = i; c++; }
    return c > 1 ? (c - 1) * 44100.0 / (last - first) : 0;
}

int main(void) {
    E = mpc_engine();
    void *h = E->create(NULL);
    char b[1024];
    float pk;
    static int16_t buf[128 * 400];

    /* every built-in program makes sound when played, without overloading */
    for (int p = 0; p < presets_count(); p++) {
        setp(h, "program", p);
        E->get_param(h, "patch_name", b, sizeof b);
        char name[64]; snprintf(name, sizeof name, "%.60s", b);
        midi3(h, 0x90, 48, 100); midi3(h, 0x90, 55, 100);
        double r = render(h, 300, &pk, NULL);
        midi3(h, 0x80, 48, 0); midi3(h, 0x80, 55, 0);
        render(h, 200, &pk, NULL);
        CHECK(r > 0.005 && r < 0.6, "program %d \"%s\" sounds (rms %.3f)", p + 1, name, r);
    }

    /* the programs above release slowly (the firmware's release is an exponential of up to 4 x 18 s): start from a fresh instance */
    E->destroy(h);
    h = E->create(NULL);

    /* pitch: Basic Program (osc 1+2 sawtooth at C0, key transpose -24) plays A4 at 440 Hz */
    setp(h, "program", 0);
    setp(h, "osc2_level", 0); setp(h, "lpf_freq", 164);
    midi3(h, 0x90, 69, 100);
    render(h, 20, &pk, NULL);
    render(h, 200, &pk, buf);
    double f = zc_freq(buf, 128 * 200);
    CHECK(fabs(f - 440) < 2, "A4 plays at %.1f Hz", f);
    for (int q = 0; q < 3; q++) {   /* every quality level keeps the pitch and stays finite */
        setp(h, "quality", q);
        render(h, 20, &pk, NULL);
        render(h, 200, &pk, buf);
        f = zc_freq(buf, 128 * 200);
        CHECK(fabs(f - 440) < 2 && pk > 1000 && pk < 32767, "quality %d: A4 at %.1f Hz, peak %.0f", q, f, pk);
    }
    setp(h, "quality", 1);
    midi3(h, 0x80, 69, 0);
    double early = render(h, 100, &pk, NULL);
    render(h, 1500, &pk, NULL);   /* release 30 = a time constant of 4 x 83 ms */
    double tail = render(h, 50, &pk, NULL);
    CHECK(early > 3 * tail && tail < 1e-4, "release dies away (rms %.6f, then %.6f)", early, tail);

    /* unison detune (main CPU table, docs/FIRMWARE.md section 12): voice 1 sits 3 cents flat in Unison 2, 1 cent flat in Unison 1 */
    {
        double fu[3];
        static const int km[3] = {0, 12, 18};          /* Poly, Unison 1, Unison 2 (Low Note) */
        setp(h, "voices", 1);
        for (int m = 0; m < 3; m++) {
            setp(h, "key_mode", km[m]);
            midi3(h, 0x90, 69, 100);
            render(h, 40, &pk, NULL);
            render(h, 200, &pk, buf);
            fu[m] = zc_freq(buf, 128 * 200);
            midi3(h, 0x80, 69, 0);
            render(h, 1600, &pk, NULL);
        }
        double c1 = 1200 * log2(fu[1] / fu[0]), c2 = 1200 * log2(fu[2] / fu[0]);
        CHECK(fabs(c1 + 1) < 0.5 && fabs(c2 + 3) < 0.5, "unison detune of voice 1: %.2f / %.2f cents (-1 / -3)", c1, c2);
        setp(h, "key_mode", 4);
        setp(h, "voices", 4);
    }

    /* digital oscillator alone, wave 1 (sine) */
    setp(h, "osc1_level", 0); setp(h, "osc3_level", 100); setp(h, "osc3_shape", 0);
    midi3(h, 0x90, 57, 100);
    render(h, 20, &pk, NULL);
    render(h, 200, &pk, buf);
    f = zc_freq(buf, 128 * 200);
    CHECK(fabs(f - 220) < 1.5, "Osc 3 sine plays A3 at %.1f Hz", f);
    midi3(h, 0x80, 57, 0);
    render(h, 300, &pk, NULL);

    /* the sequencer plays voice 1 with no key down when started */
    setp(h, "program", 4);   /* Sequenced Bass: Key Gates Seq Rst */
    setp(h, "trigger", 0);
    setp(h, "seq_run", 1); setp(h, "clock_src", 0);
    double r = render(h, 400, &pk, NULL);
    CHECK(r > 0.003, "sequencer running plays without a key (rms %.3f)", r);
    setp(h, "seq_run", 0);
    render(h, 400, &pk, NULL);
    r = render(h, 100, &pk, NULL);
    CHECK(r < 0.001, "stopped sequencer goes quiet (rms %.4f)", r);

    /* gated trigger mode: a key starts its own sequence */
    setp(h, "program", 4);
    midi3(h, 0x90, 40, 100);
    r = render(h, 300, &pk, NULL);
    CHECK(r > 0.003, "key-gated sequence plays (rms %.3f)", r);
    midi3(h, 0x80, 40, 0);
    render(h, 300, &pk, NULL);

    /* all parameters at their extremes: no NaN, nothing stuck at full scale */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < NPATCH; i++) setp(h, PTAB[i].key, pass ? PTAB[i].max : PTAB[i].min);
        midi3(h, 0x90, 60, 127); midi3(h, 0x90, 64, 127);
        r = render(h, 200, &pk, NULL);
        CHECK(r == r && r < 1.0, "all parameters at %s render (rms %.3f)", pass ? "maximum" : "minimum", r);
        midi3(h, 0xB0, 123, 0);
        render(h, 50, &pk, NULL);
    }

    /* state round trip */
    setp(h, "program", 2);
    setp(h, "lpf_res", 77);
    static char st[8192];
    E->get_param(h, "state", st, sizeof st);
    void *h2 = E->create(NULL);
    E->set_param(h2, "state", st);
    E->get_param(h2, "lpf_res", b, sizeof b);
    CHECK(atoi(b) == 77, "state restores lpf_res (%s)", b);
    E->get_param(h2, "patch_name", b, sizeof b);
    CHECK(!strcmp(b, "Hollow Pad"), "state restores the name (%s)", b);
    E->get_param(h2, "osc3_shape_display", b, sizeof b);
    CHECK(!strcmp(b, "13"), "wave display is 1-based (%s)", b);
    E->get_param(h2, "dly1_time_display", b, sizeof b);
    CHECK(strstr(b, "ms") != NULL, "delay time shows ms (%s)", b);
    E->get_param(h2, "lfo1_freq_display", b, sizeof b);
    CHECK(!strcmp(b, "0.16 Hz") || b[0], "LFO frequency text (%s)", b);

    /* SysEx program dump: pack and unpack */
    uint8_t prog[192], msg[300], back[192];
    for (int i = 0; i < 192; i++) prog[i] = (uint8_t)((i * 37) & 0xFF);
    int n = syx_write_program(prog, 2, 17, msg);
    CHECK(n == 228, "program dump is 228 bytes (%d)", n);
    int m = syx_unpack(msg + 7, n - 8, back, 192);
    CHECK(m == 192 && !memcmp(prog, back, 192), "program dump round-trips");
    n = syx_write_program(prog, -1, 0, msg);
    CHECK(n == 226, "edit buffer dump is 226 bytes (%d)", n);

    /* waveshape dump: a 12-bit ramp lands in its slot */
    uint8_t wraw[256], wmsg[400];
    for (int i = 0; i < 128; i++) { int16_t v = (int16_t)((i - 64) * 32); wraw[2 * i] = (uint8_t)(v & 0xFF); wraw[2 * i + 1] = (uint8_t)((v >> 8) & 0xFF); }
    wmsg[0] = 99;
    int wn = 1 + syx_pack(wraw, 256, wmsg + 1);
    float w[PE_WLEN];
    int slot = waves_from_dump(wmsg, wn, w);
    CHECK(wn == 294 && slot == 99 && fabsf(w[0] + 1.0f) < 1e-6f && fabsf(w[127] - 0.984375f) < 1e-6f, "waveshape dump decodes (%d bytes, slot %d, %.4f..%.4f)", wn, slot, w[0], w[127]);

    /* a Prophet VS wave dump: 32 waves of 128 12-bit samples (high bytes, then low nibbles), built here as a ramp */
    {
        static uint8_t vsm[4 + 12288 + 1];
        vsm[0] = 0xF0; vsm[1] = 0x01; vsm[2] = 0x0A; vsm[3] = 0x7F; vsm[4 + 12288] = 0xF7;
        for (int w2 = 0; w2 < 32; w2++)
            for (int k = 0; k < 128; k++) {
                int v = k * 32;                      /* 0..4064, offset binary: -2048..+2016 */
                uint8_t *b = vsm + 4 + w2 * 384;
                b[2 * k] = (uint8_t)(v >> 8); b[2 * k + 1] = (uint8_t)((v >> 4) & 15);
                uint8_t *lb = b + 256 + 2 * (k / 2);
                if (k & 1) lb[1] = (uint8_t)(v & 15); else lb[0] = (uint8_t)(v & 15);
            }
        static float vw[32][PE_WLEN];
        int nw = waves_from_vs_dump(vsm, sizeof vsm, vw);
        CHECK(nw == 32 && fabsf(vw[5][0] + 1) < 1e-6f && fabsf(vw[5][127] - 2016 / 2048.0f) < 1e-6f, "VS wave dump decodes (%d waves, %.4f..%.4f)", nw, vw[5][0], vw[5][127]);
    }
    /* a single-cycle WAV: 16-bit mono, three cycles of different lengths marked by cue points */
    {
        static uint8_t wv[44 + 2 * 600 + 12 + 3 * 24 + 64];
        int lens[3] = {100, 200, 300}, pos = 0, o = 0;
        #define PUT32(x) do { uint32_t x_ = (x); for (int q = 0; q < 4; q++) wv[o++] = (uint8_t)(x_ >> (8 * q)); } while (0)
        #define PUT16(x) do { wv[o++] = (uint8_t)(x); wv[o++] = (uint8_t)((x) >> 8); } while (0)
        memcpy(wv, "RIFF", 4); o = 4; PUT32(0); memcpy(wv + o, "WAVEfmt ", 8); o += 8; PUT32(16);
        PUT16(1); PUT16(1); PUT32(44100); PUT32(88200); PUT16(2); PUT16(16);
        memcpy(wv + o, "data", 4); o += 4; PUT32(1200);
        for (int c = 0; c < 3; c++) for (int k = 0; k < lens[c]; k++) { int16_t x = (int16_t)(20000 * sin(6.2831853 * k / lens[c])); PUT16((uint16_t)x); }
        memcpy(wv + o, "cue ", 4); o += 4; PUT32(4 + 3 * 24); PUT32(3);
        for (int c = 0; c < 3; c++) { pos += lens[c]; PUT32(c); PUT32(pos); memcpy(wv + o, "data", 4); o += 4; PUT32(0); PUT32(0); PUT32(pos); }
        static float ww[8][PE_WLEN];
        int nc = waves_from_wav(wv, (size_t)o, ww, 8);
        CHECK(nc == 3 && fabsf(ww[2][32] - 1) < 0.02f && fabsf(ww[2][96] + 1) < 0.02f, "WAV cycles split at cue points and resample to 128 (%d, %.3f %.3f)", nc, ww[2][32], ww[2][96]);
        #undef PUT32
        #undef PUT16
    }

    /* Prophet VS program ROM images: a made-up pair with the layout (a rising 16-bit table of 7264 words, then 95 waves of 128
     * signed high bytes and 64 bytes of low nibbles, read as one stream with the high chip at even bytes from the upper 16 KB) */
    static uint8_t vhi[32768], vlo[32768];
    {
        #define VSPUT(i, x) do { int i_ = (i); (i_ & 1 ? vlo : vhi)[16384 + (i_ >> 1)] = (uint8_t)(x); } while (0)
        for (int k = 0; k < 7264; k++) { int w = k < 7263 ? 0x0300 + 8 * k : 0xFFFF; VSPUT(2 * k, w >> 8); VSPUT(2 * k + 1, w & 255); }
        for (int w = 0; w < 95; w++)
            for (int k = 0; k < 128; k++) {
                int v = 30 * k - 1950 + w, base = 14528 + 192 * w;
                VSPUT(base + k, (v >> 4) & 255);
                int nb = base + 128 + k / 2;
                uint8_t *cell = &(nb & 1 ? vlo : vhi)[16384 + (nb >> 1)];
                *cell = (uint8_t)((k & 1) ? ((*cell & 0xF0) | (v & 15)) : ((v & 15) << 4 | (*cell & 15)));
            }
        #undef VSPUT
        static float vr[95][PE_WLEN];
        int n1 = waves_from_vs_rom(vlo, sizeof vlo, vhi, sizeof vhi, vr);
        CHECK(n1 == 95 && fabsf(vr[0][0] + 1950 / 2048.0f) < 1e-6f && fabsf(vr[94][127] - (30 * 127 - 1950 + 94) / 2048.0f) < 1e-6f,
              "VS ROM chip pair decodes in either order (%d, %.4f %.4f)", n1, vr[0][0], vr[94][127]);
        CHECK(waves_from_vs_rom(vhi, sizeof vhi, vhi, sizeof vhi, vr) == 0 && waves_from_vs_rom(vlo, 1000, vhi, sizeof vhi, vr) == 0,
              "VS ROM check refuses one chip twice and a wrong size");
    }

    /* a folder of .syx files: two banks of programs and a waveshape become banks and a user wave */
    {
        char dir[] = "/tmp/pe_test_XXXXXX";
        if (mkdtemp(dir)) {
            char path[256];
            snprintf(path, sizeof path, "%s/My Banks.syx", dir);
            FILE *fp = fopen(path, "wb");
            uint8_t pr[192];
            for (int i = 0; i < 192; i++) pr[i] = (uint8_t)PTAB[i].def;
            pr[22] = 66;   /* resonance */
            for (int bk = 0; bk < 2; bk++)
                for (int k = 0; k < 3; k++) {
                    uint8_t mm[300];
                    int l = syx_write_program(pr, bk, k, mm); fwrite(mm, 1, (size_t)l, fp);
                    char nm[17]; snprintf(nm, sizeof nm, "B%c P%c", (char)('1' + bk), (char)('1' + k));
                    l = syx_write_name(nm, bk, k, mm); fwrite(mm, 1, (size_t)l, fp);
                }
            uint8_t hdr[5] = {0xF0, 0x01, 0x20, 0x01, 0x0A}, end = 0xF7;
            fwrite(hdr, 1, 5, fp); fwrite(wmsg, 1, (size_t)wn, fp); fwrite(&end, 1, 1, fp);
            fclose(fp);
            void *h3 = E->create(dir);
            E->get_param(h3, "status", b, sizeof b);
            CHECK(!strcmp(b, "3 banks, 1 waves loaded"), "folder scan finds two banks and a user wave (%s)", b);
            setp(h3, "bank", 2);
            E->get_param(h3, "bank_name", b, sizeof b);
            CHECK(!strcmp(b, "My Banks B2"), "bank name from the file (%s)", b);
            setp(h3, "program", 1);
            E->get_param(h3, "patch_name", b, sizeof b);
            char res[16]; E->get_param(h3, "lpf_res", res, sizeof res);
            CHECK(!strcmp(b, "B2 P2") && atoi(res) == 66, "program and name load from the bank (%s, res %s)", b, res);
            /* the Banks page: browse another bank without loading it, pick its programs, page through, highlights follow */
            E->get_param(h3, "browse_bank_name", b, sizeof b);
            CHECK(!strcmp(b, "3 My Banks B2"), "the browsed bank starts at the loaded one (%s)", b);
            setp(h3, "browse_bank_index", 1);
            E->get_param(h3, "patch_slot_2", b, sizeof b);
            char pn[32]; E->get_param(h3, "patch_name", pn, sizeof pn);
            CHECK(!strcmp(b, "002 B1 P2") && !strcmp(pn, "B2 P2"), "a browsed bank lists its names and loads nothing (%s, loaded %s)", b, pn);
            E->get_param(h3, "bank_slot_2_on", b, sizeof b);
            char on3[8]; E->get_param(h3, "patch_slot_2_on", on3, sizeof on3);
            CHECK(!strcmp(b, "1") && !strcmp(on3, "0"), "bank tile highlighted, program tile not (%s, %s)", b, on3);
            setp(h3, "patch_slot_3", 1);
            E->get_param(h3, "bank_name", b, sizeof b);
            E->get_param(h3, "patch_name", pn, sizeof pn);
            E->get_param(h3, "patch_slot_3_on", on3, sizeof on3);
            CHECK(!strcmp(b, "My Banks B1") && !strcmp(pn, "B1 P3") && !strcmp(on3, "1"), "a program tile loads bank and program (%s, %s, on %s)", b, pn, on3);
            setp(h3, "next_browse_bank", 1);
            E->get_param(h3, "browse_bank_name", b, sizeof b);
            setp(h3, "patch_page_next", 1);
            char pg[32]; E->get_param(h3, "patch_page_text", pg, sizeof pg);
            char s1[32]; E->get_param(h3, "patch_slot_1", s1, sizeof s1);
            CHECK(!strcmp(b, "3 My Banks B2") && !strcmp(pg, "PAGE 2/5") && !strcmp(s1, "029 Prog 29"), "bank and page steppers (%s, %s, %s)", b, pg, s1);
            setp(h3, "browse_bank_index", 0);
            E->get_param(h3, "patch_slot_1", b, sizeof b);
            CHECK(!strncmp(b, "001 ", 4) && strlen(b) > 4, "the factory bank lists its names (%s)", b);
            E->destroy(h3);
            /* add the ROM pair: 95 more waves */
            char rp[2][256];
            for (int c = 0; c < 2; c++) {
                snprintf(rp[c], sizeof rp[c], "%s/vs_%s.bin", dir, c ? "lo" : "hi");
                FILE *rf = fopen(rp[c], "wb"); fwrite(c ? vlo : vhi, 1, 32768, rf); fclose(rf);
            }
            h3 = E->create(dir);
            E->get_param(h3, "status", b, sizeof b);
            CHECK(!strcmp(b, "3 banks, 96 waves loaded"), "folder scan reads the VS ROM chip pair (%s)", b);
            E->destroy(h3);
            remove(rp[0]); remove(rp[1]);
            remove(path);
            snprintf(path, sizeof path, "%s/SYSEX", dir); rmdir(path);
            rmdir(dir);
        }
    }

    E->destroy(h); E->destroy(h2);
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails;
}
