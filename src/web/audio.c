/* audio.c - software APU for the Metroid: Zero Mission web port (TARGET_WEB).
 *
 * Implements gba_apu_render(): a self-contained software mixer that renders
 * interleaved-stereo s16 for Web Audio from GBA sound state. It models the GBA
 * APU as the game drives it:
 *   - 4 PSG channels (square 1, square 2, wave, noise) read from the GBA sound
 *     registers in the relocated I/O space (gGbaIo, 0x60..0x9F).
 *   - 2 DirectSound (FIFO) PCM channels (A/B), fed through a decoupled push API
 *     (gba_apu_push_directsound) and resampled to the output rate.
 * PSG waveforms are synthesized analytically at the output rate; DirectSound is
 * resampled with a fractional accumulator. The GBA mixes near 13379 Hz (MZM's
 * default SOUND_MODE_FREQ) up to 32768 Hz; we render straight at the Web Audio
 * rate (44100/48000) so no fixed intermediate rate is assumed.
 *
 * This file is standalone-compilable (links only against the extern gGbaIo[]
 * buffer, which the headless test defines) and does NOT reference the game's
 * audio structs, so it builds for the milestone without the full tree.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "gba_hle.h" /* frozen contract: extern gGbaIo[]; gba_apu_render proto */
#include "audio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- GBA sound register offsets within gGbaIo ----------------------------- */
#define IO_SND1CNT_L 0x60
#define IO_SND1CNT_H 0x62
#define IO_SND1CNT_X 0x64
#define IO_SND2CNT_L 0x68
#define IO_SND2CNT_H 0x6C
#define IO_SND3CNT_L 0x70
#define IO_SND3CNT_H 0x72
#define IO_SND3CNT_X 0x74
#define IO_SND4CNT_L 0x78
#define IO_SND4CNT_H 0x7C
#define IO_SNDCNT_L  0x80
#define IO_SNDCNT_H  0x82
#define IO_SNDCNT_X  0x84
#define IO_WAVE_RAM  0x90

static uint16_t io16(int off)
{
    return (uint16_t)((unsigned)gGbaIo[off] | ((unsigned)gGbaIo[off + 1] << 8));
}

/* --- Mixer state ---------------------------------------------------------- */
#define DS_CAP 16384

typedef struct {
    float buf[DS_CAP];
    int head;      /* read index */
    int count;     /* samples available */
    double pos;    /* fractional read position within the ring (0..count) */
    int src_rate;  /* source sample rate of pushed data */
} DsChannel;

static struct {
    int   out_rate;
    int   inited;
    double master_vol;

    /* PSG phase state (persist across render calls) */
    double sq1_phase, sq2_phase, wave_idx;
    uint16_t lfsr;
    double noise_phase;

    /* test tone */
    int    tone_on, tone_square;
    double tone_freq, tone_amp, tone_phase;

    DsChannel ds[2];
} g;

void gba_apu_init(int sample_rate)
{
    if (sample_rate <= 0)
        sample_rate = 48000;
    memset(&g, 0, sizeof(g));
    g.out_rate = sample_rate;
    g.master_vol = 0.85;
    g.lfsr = 0x7FFF;
    g.inited = 1;
}

void gba_apu_set_master_volume(double v)
{
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    g.master_vol = v;
}

void gba_apu_set_test_tone(int enable, double freq_hz, double amplitude, int square)
{
    if (!g.inited) gba_apu_init(0);
    g.tone_on = enable;
    g.tone_freq = freq_hz;
    g.tone_amp = amplitude;
    g.tone_square = square;
    g.tone_phase = 0.0;
}

void gba_apu_push_directsound(int ch, const int8_t *samples, int count, int src_rate)
{
    if (!g.inited) gba_apu_init(0);
    if (ch < 0 || ch > 1 || samples == NULL || count <= 0)
        return;
    DsChannel *d = &g.ds[ch];
    d->src_rate = src_rate > 0 ? src_rate : 13379;
    for (int i = 0; i < count; i++) {
        if (d->count >= DS_CAP) {
            /* ring full: drop oldest to make room */
            d->head = (d->head + 1) % DS_CAP;
            d->count--;
            if (d->pos > 0.0) d->pos -= 1.0;
        }
        int w = (d->head + d->count) % DS_CAP;
        d->buf[w] = (float)samples[i] / 128.0f;
        d->count++;
    }
}

/* Read one resampled sample (output-rate) from a DirectSound channel, advancing
 * its fractional read position. Returns 0 on underrun. */
static float ds_next(DsChannel *d)
{
    if (d->count <= 0 || d->src_rate <= 0)
        return 0.0f;

    int i0 = (int)d->pos;
    if (i0 >= d->count) {
        /* consumed everything available */
        d->head = (d->head + d->count) % DS_CAP;
        d->count = 0;
        d->pos = 0.0;
        return 0.0f;
    }

    float a = d->buf[(d->head + i0) % DS_CAP];
    float b = (i0 + 1 < d->count) ? d->buf[(d->head + i0 + 1) % DS_CAP] : a;
    float frac = (float)(d->pos - (double)i0);
    float out = a + (b - a) * frac;

    d->pos += (double)d->src_rate / (double)g.out_rate;

    /* retire fully-consumed whole samples to keep indices small */
    int consumed = (int)d->pos;
    if (consumed > 0) {
        if (consumed > d->count) consumed = d->count;
        d->head = (d->head + consumed) % DS_CAP;
        d->count -= consumed;
        d->pos -= (double)consumed;
    }
    return out;
}

/* --- PSG channel synthesis (sampled at the output rate) ------------------- */

static const double kDuty[4] = { 0.125, 0.25, 0.5, 0.75 };

/* square: returns value in [-1,1] scaled by volume, 0 if inaudible. duty_env is
 * the CNTx register holding duty (bits 6-7) + envelope init volume (bits 12-15);
 * freq_ctl holds the 11-bit rate (bits 0-10). */
static double psg_square(uint16_t duty_env, uint16_t freq_ctl, double *phase)
{
    int vol = (duty_env >> 12) & 0xF;
    int rate = freq_ctl & 0x7FF;
    int denom = 2048 - rate;
    if (vol == 0 || denom <= 0)
        return 0.0;
    double freq = 131072.0 / (double)denom;
    double duty = kDuty[(duty_env >> 6) & 3];

    double ph = *phase + freq / (double)g.out_rate;
    ph -= floor(ph);
    *phase = ph;

    double s = (ph < duty) ? 1.0 : -1.0;
    return s * ((double)vol / 15.0);
}

static double psg_wave(void)
{
    uint16_t cl = io16(IO_SND3CNT_L);
    if (!(cl & 0x80)) /* DAC / sound off */
        return 0.0;
    uint16_t ch = io16(IO_SND3CNT_H);
    uint16_t cx = io16(IO_SND3CNT_X);
    int rate = cx & 0x7FF;
    int denom = 2048 - rate;
    if (denom <= 0)
        return 0.0;

    /* volume: bits 13-14 (0=mute,1=100%,2=50%,3=25%), bit15 forces 75% */
    double vmul;
    if (ch & 0x8000) vmul = 0.75;
    else switch ((ch >> 13) & 3) {
        case 0: return 0.0;
        case 1: vmul = 1.0; break;
        case 2: vmul = 0.5; break;
        default: vmul = 0.25; break;
    }

    double step = (2097152.0 / (double)denom) / (double)g.out_rate; /* samples/out-sample */
    double idx = g.wave_idx + step;
    idx -= 32.0 * floor(idx / 32.0);
    g.wave_idx = idx;

    int i = (int)idx & 31;
    uint8_t byte = gGbaIo[IO_WAVE_RAM + (i >> 1)];
    int nib = (i & 1) ? (byte & 0xF) : (byte >> 4);
    double s = ((double)nib / 7.5) - 1.0; /* 0..15 -> -1..+1 */
    return s * vmul;
}

static double psg_noise(void)
{
    uint16_t cl = io16(IO_SND4CNT_L);
    uint16_t ch = io16(IO_SND4CNT_H);
    int vol = (cl >> 12) & 0xF;
    if (vol == 0)
        return 0.0;

    int r = ch & 7;
    int s = (ch >> 4) & 0xF;
    int width7 = (ch >> 3) & 1;
    double div = (r == 0) ? 0.5 : (double)r;
    double clock = 524288.0 / div / pow(2.0, (double)(s + 1));

    g.noise_phase += clock / (double)g.out_rate;
    int steps = (int)g.noise_phase;
    if (steps > 64) steps = 64; /* clamp absurd rates */
    g.noise_phase -= floor(g.noise_phase);
    for (int k = 0; k < steps; k++) {
        int bit = (g.lfsr ^ (g.lfsr >> 1)) & 1;
        g.lfsr >>= 1;
        if (width7)
            g.lfsr = (uint16_t)((g.lfsr & ~0x40) | (bit << 6));
        else
            g.lfsr = (uint16_t)((g.lfsr & ~0x4000) | (bit << 14));
    }
    double out = (g.lfsr & 1) ? -1.0 : 1.0;
    return out * ((double)vol / 15.0);
}

/* --- Main render ---------------------------------------------------------- */

void gba_apu_render(int16_t *out, int frames)
{
    if (!out || frames <= 0)
        return;
    if (!g.inited)
        gba_apu_init(0);

    /* Test tone: bypass the channel mixer (bring-up / milestone path). */
    if (g.tone_on) {
        double inc = g.tone_freq / (double)g.out_rate;
        for (int i = 0; i < frames; i++) {
            double ph = g.tone_phase;
            double s = g.tone_square ? ((ph < 0.5) ? 1.0 : -1.0)
                                     : sin(2.0 * M_PI * ph);
            int v = (int)lrint(s * g.tone_amp * 32767.0);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[2 * i + 0] = (int16_t)v;
            out[2 * i + 1] = (int16_t)v;
            ph += inc;
            ph -= floor(ph);
            g.tone_phase = ph;
        }
        return;
    }

    uint16_t sndx = io16(IO_SNDCNT_X);
    int master_on = (sndx & 0x80) != 0;
    uint16_t cl = io16(IO_SNDCNT_L);
    uint16_t chh = io16(IO_SNDCNT_H);

    /* PSG left/right per-channel enables (SOUNDCNT_L bits 8-11 right, 12-15 left) */
    int rEn = (cl >> 8) & 0xF;
    int lEn = (cl >> 12) & 0xF;
    double lMaster = (double)((cl >> 4) & 7) / 7.0;
    double rMaster = (double)(cl & 7) / 7.0;

    /* PSG : DMA volume ratio (SOUNDCNT_H bits 0-1: 0=25%,1=50%,2=100%) */
    double psgRatio;
    switch (chh & 3) { case 0: psgRatio = 0.25; break; case 1: psgRatio = 0.5; break; default: psgRatio = 1.0; }

    /* DirectSound routing / volume (SOUNDCNT_H) */
    double dsAvol = (chh & 0x4) ? 1.0 : 0.5;
    double dsBvol = (chh & 0x8) ? 1.0 : 0.5;
    int aR = (chh & 0x100) != 0, aL = (chh & 0x200) != 0;
    int bR = (chh & 0x1000) != 0, bL = (chh & 0x2000) != 0;

    for (int i = 0; i < frames; i++) {
        double pl = 0.0, pr = 0.0;

        if (master_on) {
            double s1 = psg_square(io16(IO_SND1CNT_H), io16(IO_SND1CNT_X), &g.sq1_phase);
            double s2 = psg_square(io16(IO_SND2CNT_L), io16(IO_SND2CNT_H), &g.sq2_phase);
            double w  = psg_wave();
            double n  = psg_noise();
            double ch_v[4] = { s1, s2, w, n };
            for (int c = 0; c < 4; c++) {
                if (lEn & (1 << c)) pl += ch_v[c];
                if (rEn & (1 << c)) pr += ch_v[c];
            }
            /* 4 channels, 4-bit each -> normalize, apply PSG ratio + masters */
            pl *= 0.25 * psgRatio * lMaster;
            pr *= 0.25 * psgRatio * rMaster;
        }

        /* DirectSound A/B */
        float a = ds_next(&g.ds[0]);
        float b = ds_next(&g.ds[1]);
        double dl = (aL ? a * dsAvol : 0.0) + (bL ? b * dsBvol : 0.0);
        double dr = (aR ? a * dsAvol : 0.0) + (bR ? b * dsBvol : 0.0);

        double L = (pl + dl) * g.master_vol * 0.5;
        double R = (pr + dr) * g.master_vol * 0.5;

        int li = (int)lrint(L * 32767.0);
        int ri = (int)lrint(R * 32767.0);
        if (li > 32767) li = 32767; if (li < -32768) li = -32768;
        if (ri > 32767) ri = 32767; if (ri < -32768) ri = -32768;
        out[2 * i + 0] = (int16_t)li;
        out[2 * i + 1] = (int16_t)ri;
    }
}

