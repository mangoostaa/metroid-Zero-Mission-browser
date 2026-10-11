/* gba_hle.h — host backend contract for the web port (TARGET_WEB).
 *
 * The game's C is unchanged; this backend stands in for GBA hardware + BIOS.
 * Implemented in gba_hle.c. The subsystems below are the fan-out boundary for the
 * three port agents (PPU / audio+input+saves / build+harness+frame-loop).
 */
#ifndef GBA_HLE_H
#define GBA_HLE_H
#include <stdint.h>

#define GBA_SCREEN_W 240
#define GBA_SCREEN_H 160

/* Relocated GBA address space (gba/memory.h BASE macros point here). */
extern unsigned char gGbaEwram[], gGbaIwram[], gGbaIo[], gGbaPalram[],
                     gGbaVram[], gGbaOam[], gGbaRom[], gGbaSram[];

/* Entry / BIOS (implemented): program start calls the game's agbmain(). */
extern void agbmain(void);
void gba_bios_swi(int num);          /* inline-SWI dispatch (SYS_Halt, ...) */

/* --- Fan-out interface (to be implemented by the agents) --------------------- */

/* Agent 1 — software PPU: render current VRAM/OAM/PAL/regs to a 240x160 RGBA buffer. */
void gba_ppu_render_frame(uint32_t *out_rgba /* GBA_SCREEN_W*GBA_SCREEN_H */);

/* Agent 2 — audio: pull `frames` interleaved-stereo s16 samples from the m4a mixer. */
void gba_apu_render(int16_t *out, int frames);
/* Agent 2 — input: active-low GBA KEYINPUT bits from keyboard/gamepad. */
void gba_set_keys(uint16_t keyinput);
/* Agent 2 — saves: SRAM/Flash backing, persisted to IndexedDB. */
void gba_save_flush(void);

/* Agent 3 — frame loop: run one frame (game logic to the next VBlank), present,
 * and dispatch the registered VBlank IRQ handler so gVBlankRequestFlag is set. */
void gba_run_frame(void);

#endif /* GBA_HLE_H */
