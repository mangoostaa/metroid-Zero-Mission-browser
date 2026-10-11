/* loop.c (A3) — web frame loop.
 *
 * The game runs its own main loop and syncs each frame on VBlankIntrWait /
 * SYSCALL(2) Halt; gba_hle.c routes both to gba_frame_tick() here. Per tick we
 * raise the VBlank state, run the game's registered VBlank handler
 * (sIntrTable[VBlank] == CallbackCallVblank, which sets gVBlankRequestFlag and
 * calls gVBlankCallback), hand the frame to the PPU, and yield to the browser.
 */
#include <stdint.h>
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern void CallbackCallVblank(void);     /* src/callbacks.c */
extern unsigned char gGbaIo[];            /* src/web/gba_hle.c */
extern void gba_ppu_render_frame(uint32_t *out_rgba);

#define REG16(off) (*(volatile uint16_t *)(gGbaIo + (off)))
#define IO_DISPSTAT 0x004
#define IO_VCOUNT   0x006
#define DSTAT_IN_VBLANK 0x0001

static uint32_t s_frame;
static uint32_t s_fb[240 * 160];          /* framebuffer the PPU fills; shell presents it */

/* Weak fallbacks so the loop links before Agents 1/2 land their real versions. */
__attribute__((weak)) void gba_ppu_render_frame(uint32_t *o) { (void)o; }
__attribute__((weak)) void gba_apu_render(int16_t *o, int n) { (void)o; (void)n; }
__attribute__((weak)) void gba_set_keys(uint16_t k) { (void)k; }
__attribute__((weak)) void gba_run_frame(void) {}

void gba_frame_tick(void)
{
    REG16(IO_VCOUNT) = 160;               /* report the VBlank scanline */
    REG16(IO_DISPSTAT) |= DSTAT_IN_VBLANK;
    CallbackCallVblank();                 /* game VBlank handler -> sets gVBlankRequestFlag */
    REG16(IO_DISPSTAT) &= ~DSTAT_IN_VBLANK;

    gba_ppu_render_frame(s_fb);           /* weak until Agent 1; shell blits s_fb to canvas */

    ++s_frame;
#ifdef MZM_WEB_LOG
    if (s_frame <= 3 || (s_frame % 60) == 0) { printf("[loop] frame %u\n", s_frame); fflush(stdout); }
#endif
#ifdef __EMSCRIPTEN__
    emscripten_sleep(16);                  /* Asyncify: give the browser a turn */
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE uint32_t gba_frame_count(void) { return s_frame; }
/* Exposed to the JS shell: the 240x160 RGBA framebuffer the PPU fills (the shell
 * blits it to the canvas) and the input entry point. */
EMSCRIPTEN_KEEPALIVE uint32_t *gba_framebuffer(void) { return s_fb; }
EMSCRIPTEN_KEEPALIVE void gba_js_set_keys(uint16_t keyinput) { gba_set_keys(keyinput); }
#endif
