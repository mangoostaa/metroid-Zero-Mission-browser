/* test_audio.c - headless milestone + mixer tests for the web-port APU.
 *
 * Guarded by MZM_TEST so the full-tree build (web/build.sh globs src for .c
 * files with no -DMZM_TEST) compiles this to an empty TU: no main(), no clash
 * with gba_hle.c. The standalone milestone build compiles ONLY audio.c + this
 * file with -DMZM_TEST, so there is no gba_hle.c to collide with.
 *
 * Build/run (milestone):
 *   emcc -I include -DMODERN -DTARGET_WEB -DMZM_TEST -Wno-everything -O0 \
 *        -sNODERAWFS=1 src/web/audio.c src/web/test_audio.c -o web/build/test_audio.js
 *   node web/build/test_audio.js web/build/test_tone.wav
 */
#ifdef MZM_TEST

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "audio.h"

/* Standalone backing for the GBA I/O space that audio.c reads (gGbaIo[0x60..]). */
unsigned char gGbaIo[0x400];

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

/* --- canonical 16-bit stereo PCM WAV writer ------------------------------- */
static void w16(FILE *f, uint16_t v){ fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void w32(FILE *f, uint32_t v){ fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f); }

static int write_wav(const char *path, const int16_t *pcm, int frames, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t dataBytes = (uint32_t)frames * 2u * 2u; /* stereo * s16 */
    fwrite("RIFF", 1, 4, f); w32(f, 36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(f, 16); w16(f, 1); w16(f, 2);
    w32(f, (uint32_t)rate); w32(f, (uint32_t)rate * 2u * 2u); w16(f, 4); w16(f, 16);
    fwrite("data", 1, 4, f); w32(f, dataBytes);
    fwrite(pcm, 1, dataBytes, f);
    return fclose(f) == 0 ? 0 : -1;
}

/* measure peak amplitude (0..1) and dominant frequency (Hz) of the left channel
 * via zero-crossing counting */
static void analyze(const int16_t *pcm, int frames, int rate, double *peak, double *freq)
{
    int maxabs = 0, crossings = 0, prev = 0;
    for (int i = 0; i < frames; i++) {
        int s = pcm[2 * i];
        int a = s < 0 ? -s : s;
        if (a > maxabs) maxabs = a;
        int sign = (s > 0) ? 1 : (s < 0 ? -1 : 0);
        if (sign != 0) {
            if (prev != 0 && sign != prev) crossings++;
            prev = sign;
        }
    }
    *peak = (double)maxabs / 32767.0;
    *freq = ((double)crossings / 2.0) / ((double)frames / (double)rate);
}

int main(int argc, char **argv)
{
    const char *wavPath = (argc > 1) ? argv[1] : "test_tone.wav";
    const int rate = 44100;
    const int frames = rate; /* 1 second */
    int16_t *pcm = (int16_t *)malloc((size_t)frames * 2 * sizeof(int16_t));
    double peak, freq;

    /* ---- MILESTONE: 440 Hz test tone -> WAV ---- */
    printf("[1] test tone -> WAV\n");
    gba_apu_init(rate);
    gba_apu_set_master_volume(1.0);
    gba_apu_set_test_tone(1, 440.0, 0.5, 1 /* square */);
    gba_apu_render(pcm, frames);
    CHECK(write_wav(wavPath, pcm, frames, rate) == 0, "wrote WAV file");

    /* verify by reading the file header + size back */
    {
        FILE *f = fopen(wavPath, "rb");
        CHECK(f != NULL, "WAV reopens");
        if (f) {
            unsigned char h[44];
            size_t rd = fread(h, 1, 44, f);
            fseek(f, 0, SEEK_END);
            long total = ftell(f);
            fclose(f);
            uint32_t hdrRate = h[24] | (h[25]<<8) | (h[26]<<16) | ((uint32_t)h[27]<<24);
            uint16_t chans = h[22] | (h[23]<<8);
            uint16_t bits = h[34] | (h[35]<<8);
            long expect = 44 + (long)frames * 4;
            CHECK(rd == 44 && memcmp(h, "RIFF", 4) == 0 && memcmp(h+8, "WAVE", 4) == 0, "WAV magic RIFF/WAVE");
            CHECK(chans == 2 && bits == 16 && hdrRate == (uint32_t)rate, "WAV fmt = 44100/16/stereo");
            CHECK(total == expect, "WAV byte length matches frame count");
        }
    }
    analyze(pcm, frames, rate, &peak, &freq);
    printf("  tone peak=%.3f freq=%.1fHz\n", peak, freq);
    CHECK(peak > 0.45 && peak < 0.55, "tone amplitude ~0.5 full-scale");
    CHECK(fabs(freq - 440.0) < 10.0, "tone frequency ~440 Hz");

    /* ---- PSG square channel driven via the GBA sound registers ---- */
    printf("[2] PSG square (channel 2) via registers\n");
    gba_apu_init(rate);
    gba_apu_set_master_volume(1.0);
    memset(gGbaIo, 0, sizeof(gGbaIo));
    {
        int freqRate = 2048 - (int)(131072.0 / 440.0); /* ~1750 -> ~440 Hz */
        #define SETIO16(off, v) do { gGbaIo[(off)] = (unsigned char)((v) & 0xFF); \
                                     gGbaIo[(off)+1] = (unsigned char)(((v) >> 8) & 0xFF); } while (0)
        SETIO16(0x68, 0xF000 | (2 << 6));     /* SND2CNT_L: vol=15, duty=50% */
        SETIO16(0x6C, freqRate & 0x7FF);      /* SND2CNT_H: 11-bit rate */
        SETIO16(0x84, 0x0080);                /* SOUNDCNT_X: master enable */
        SETIO16(0x80, 0x0077 | 0x0200 | 0x2000); /* SOUNDCNT_L: masters + ch2 L/R */
        SETIO16(0x82, 0x0002);                /* SOUNDCNT_H: PSG ratio 100% */
    }
    gba_apu_render(pcm, frames);
    analyze(pcm, frames, rate, &peak, &freq);
    printf("  psg peak=%.3f freq=%.1fHz\n", peak, freq);
    CHECK(peak > 0.05, "PSG square is audible (non-silent)");
    CHECK(fabs(freq - 440.0) < 25.0, "PSG square frequency ~440 Hz");

    /* ---- DirectSound (FIFO) PCM push + resample ---- */
    printf("[3] DirectSound A: push + resample (8000 -> 44100)\n");
    gba_apu_init(rate);
    gba_apu_set_master_volume(1.0);
    memset(gGbaIo, 0, sizeof(gGbaIo));
    gGbaIo[0x82] = 0x04 | 0x00; gGbaIo[0x83] = 0x03; /* SOUNDCNT_H: A vol 100%, A L+R */
    {
        /* constant level: deterministic amplitude check */
        int8_t block[2000];
        for (int i = 0; i < 2000; i++) block[i] = 64; /* 0.5 full-scale */
        gba_apu_push_directsound(0, block, 2000, 8000);
        gba_apu_render(pcm, 1000);
        int mid = pcm[2 * 500];
        printf("  ds const sample=%d (expect ~8192)\n", mid);
        CHECK(mid > 7000 && mid < 9400, "DirectSound constant amplitude ~0.25 FS");
    }
    gba_apu_init(rate);
    gba_apu_set_master_volume(1.0);
    memset(gGbaIo, 0, sizeof(gGbaIo));
    gGbaIo[0x82] = 0x04; gGbaIo[0x83] = 0x03;
    {
        /* 1000 Hz square at 8000 Hz src: period 8 samples (4 hi / 4 lo) */
        int8_t sq[8000];
        for (int i = 0; i < 8000; i++) sq[i] = ((i / 4) & 1) ? -64 : 64;
        gba_apu_push_directsound(0, sq, 8000, 8000);
        gba_apu_render(pcm, frames);
        analyze(pcm, frames, rate, &peak, &freq);
        printf("  ds peak=%.3f freq=%.1fHz\n", peak, freq);
        CHECK(peak > 0.05, "DirectSound square is audible");
        CHECK(fabs(freq - 1000.0) < 60.0, "DirectSound resampled frequency ~1000 Hz");
    }

    free(pcm);
    printf(g_fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return g_fail;
}

#endif /* MZM_TEST */
