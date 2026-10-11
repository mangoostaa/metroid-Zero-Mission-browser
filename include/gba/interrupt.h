#ifndef GBA_INTERRUPT_H
#define GBA_INTERRUPT_H

#include "gba/memory.h"

#define REG_IME (REG_BASE + 0x208)
#define REG_IE (REG_BASE + 0x200)
#define REG_IF (REG_BASE + 0x202)
#ifdef TARGET_WEB
/* No scanline hardware: the game spin-waits on VCOUNT to reach/leave a line
 * (in_game.c room load, audio_wrappers, erase_sram). With a static VCOUNT those
 * loops hang. gba_vcount_addr() (src/web/gba_hle.c) returns a slot holding a
 * free-running 0..227 counter that advances on every read, so each wait ends. */
const volatile void *gba_vcount_addr(void);
#define REG_VCOUNT (gba_vcount_addr())
#else
#define REG_VCOUNT (REG_BASE + 0x006)
#endif

#define IF_VBLANK  (1 << 0)
#define IF_HBLANK  (1 << 1)
#define IF_VCOUNT  (1 << 2)
#define IF_TIMER0  (1 << 3)
#define IF_TIMER1  (1 << 4)
#define IF_TIMER2  (1 << 5)
#define IF_TIMER3  (1 << 6)
#define IF_SERIAL  (1 << 7)
#define IF_DMA0    (1 << 8)
#define IF_DMA1    (1 << 9)
#define IF_DMA2    (1 << 10)
#define IF_DMA3    (1 << 11)
#define IF_KEYPAD  (1 << 12)
#define IF_GAMEPAK (1 << 13)

#endif /* GBA_INTERRUPT_H */
