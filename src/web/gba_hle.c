/* gba_hle.c — host backend for the web port (TARGET_WEB).
 *
 * Provides: the relocated GBA address-space buffers (referenced by gba/memory.h),
 * C implementations of the BIOS syscalls that asm/syscalls.s normally provides,
 * the inline-SWI dispatch (gba_bios_swi), the handful of linker-script symbols the
 * C code references, and the program entry (main -> agbmain).
 *
 * This is a FIRST-LINK scaffold: memory ops / div / sqrt are real; graphics
 * decompression and audio are stubbed (TODO: PPU=Agent1, audio=Agent2). The frame
 * loop yields to the browser via Asyncify at the game's VBlank wait points.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* --- Relocated GBA address space (gba/memory.h points its BASE macros here) ----- */
unsigned char gGbaEwram[256 * 1024];
unsigned char gGbaIwram[32 * 1024];
unsigned char gGbaIo[0x400];
unsigned char gGbaPalram[0x400];
unsigned char gGbaVram[0x18000];
unsigned char gGbaOam[0x400];
unsigned char gGbaRom[1];          /* ROM reads (nes_metroid / transfer) deferred */
unsigned char gGbaSram[64 * 1024];

/* --- Linker-script symbols the C code references (normally from linker.ld) ------ */
void *gIntrVector;                 /* IRQ handler slot */
void *gIntrCodePointer;
unsigned short gInterruptCheckFlag;
unsigned char __nes_metroid_load_address[1];

/* --- BIOS syscalls (replace asm/syscalls.s) ------------------------------------- */
#define CPUSET_COUNT_MASK 0x1FFFFF
#define CPUSET_FILL  (1 << 24)
#define CPUSET_32BIT (1 << 26)

void CpuSet(void *src, void *dst, uint32_t ctl)
{
    uint32_t n = ctl & CPUSET_COUNT_MASK;
    int fill = (ctl & CPUSET_FILL) != 0;
    if (ctl & CPUSET_32BIT) {
        uint32_t *s = src, *d = dst;
        for (uint32_t i = 0; i < n; i++) d[i] = s[fill ? 0 : i];
    } else {
        uint16_t *s = src, *d = dst;
        for (uint32_t i = 0; i < n; i++) d[i] = s[fill ? 0 : i];
    }
}

void CpuFastSet(void *src, void *dst, uint16_t ctl)
{
    uint32_t n = (uint32_t)ctl & CPUSET_COUNT_MASK; /* u16 can't carry the fill bit */
    uint32_t *s = src, *d = dst;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

int32_t DivarmDiv(int32_t number, int32_t denom) { return denom ? number / denom : 0; }
int32_t DivarmMod(int32_t denom, int32_t number) { return denom ? number % denom : 0; }

uint16_t Sqrt(uint32_t n) { return (uint16_t)(uint32_t)sqrtf((float)n); }

/* TODO(Agent1/me): real GBA LZ77/Huffman decompressors. Stubbed for first link. */
void LZ77UncompVram(const void *src, void *dst) { (void)src; (void)dst; }
void LZ77UncompWram(const void *src, void *dst) { (void)src; (void)dst; }

/* TODO(Agent2): audio. Stubbed for first link. */
uint32_t MidiKey2Freq(uint32_t *wd, uint8_t mk, uint8_t fp) { (void)wd; (void)mk; (void)fp; return 0; }
int Multiboot(void *mbp) { (void)mbp; return 1; }
void SoundBias0(void) {}
void SoundBias200(void) {}

/* --- EWRAM globals declared extern unconditionally and defined only in linker.ld
 *     (no CAST_TO_ARRAY fallback). Sizes = their linker.ld regions; u32 element
 *     type forces 4-byte alignment. (Faithful adjacency is a later refinement.) --- */
unsigned int gClipdataCollisionTypes[0x400 / 4];
unsigned int gClipdataCollisionTypes_Tilemap[0xC00 / 4];
unsigned int gClipdataBehaviorTypes[0x800 / 4];
unsigned int gClipdataBehaviorTypes_Tilemap[0x800 / 4];
unsigned int gTilemap[0x2000 / 4];
unsigned int gCommonTilemap[0x1000 / 4];
unsigned int gDecompBg0Map[0x3000 / 4];
unsigned int gDecompBg1Map[0x3000 / 4];
unsigned int gDecompBg2Map[0x3000 / 4];
unsigned int gDecompBg3Map[0x3000 / 4];
unsigned int gDecompClipdataMap[0x3000 / 4];
unsigned int gMakeSolidBlocks[0x80 / 4];
unsigned int gEventsTriggered[0x20 / 4];

/* --- linker.ld marker + sound data referenced but defined outside src/ --- */
unsigned char sTransferRom_After[1];
unsigned int sSoundDataEntries[0x400]; /* TODO(Agent2): real sound entries table */

/* --- m4a / MP2k sound driver (asm/soundcode.s + audio_internal.s). Stubbed. --- */
void InitTrack() {}
void ResetTrack() {}
void StopMusicOrSound() {}
void UpdateAudio() {}

/* Frame yield: the game spins in `do { SYSCALL(2); } while(!vblankflag)` and calls
 * VBlankIntrWait once per frame. Yield to the browser here (Asyncify). The actual
 * VBlank IRQ dispatch (sets the flag + runs per-frame work) is wired next. */
static void gba_yield(void)
{
#ifdef __EMSCRIPTEN__
    emscripten_sleep(0);
#endif
}

void VBlankIntrWait(void) { gba_yield(); }

void gba_bios_swi(int num)
{
    (void)num; /* SYS_Halt (2) and friends: just yield for now */
    gba_yield();
}

extern void agbmain(void);

int main(void)
{
    agbmain(); /* GBA entry; runs the game's own main loop (never returns) */
    return 0;
}
