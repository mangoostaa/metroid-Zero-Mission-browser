/* audio.h - internal API for the web-port software APU (src/web only).
 *
 * NOT part of the frozen host contract (gba_hle.h). gba_apu_render() itself is
 * declared in gba_hle.h; the extra entry points here are the integration seam
 * used by the frame loop / Web Audio glue and by the headless tests.
 */
#ifndef WEB_AUDIO_H
#define WEB_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the output (Web Audio) sample rate in Hz, e.g. 44100 or 48000. Safe to
 * call more than once; also lazily defaults to 48000 on first render. */
void gba_apu_init(int sample_rate);

/* Frozen contract (also declared in gba_hle.h): pull `frames` interleaved
 * stereo s16 samples from the mixer into `out` (out holds frames*2 int16_t). */
void gba_apu_render(int16_t *out, int frames);

/* --- Test / bring-up hooks ------------------------------------------------ */

/* Enable a self-contained test tone that bypasses the channel mixer. When on,
 * gba_apu_render emits a pure tone (square != 0 => square wave, else sine). */
void gba_apu_set_test_tone(int enable, double freq_hz, double amplitude, int square);

/* --- DirectSound (FIFO) PCM source --------------------------------------- */

/* Push mono signed-8-bit PCM for DirectSound channel ch (0 = A, 1 = B), sampled
 * at src_rate Hz. The mixer resamples it to the output rate. This is the
 * decoupled seam: the full build feeds it from the game's mixed PCM buffer
 * (gMusicInfo.soundRawData); the tests feed synthetic data. */
void gba_apu_push_directsound(int ch, const int8_t *samples, int count, int src_rate);

/* Master output volume (0.0 .. 1.0), applied after mixing. Default 0.85. */
void gba_apu_set_master_volume(double v);

#ifdef __cplusplus
}
#endif

#endif /* WEB_AUDIO_H */
