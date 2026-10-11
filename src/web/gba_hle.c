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

/* --- DMA (replace the register-poking DMA_SET macro from gba/dma.h) --------------
 * On hardware the DMA_SET macro writes src/dst/count to the DMA registers and the
 * DMA controller performs the transfer. There is no controller here, so gba/dma.h
 * routes DMA_SET to this function under TARGET_WEB and we perform the copy/fill
 * directly. This is what actually flushes shadow OAM (gOamData) and palette/VRAM to
 * the relocated buffers each frame, so sprites and scroll updates become visible.
 *
 * `cnt` is C_32_2_16(control, count): high 16 = DMA control flags, low 16 = unit
 * count (0 means 0x10000 on channel 3, the max). We only run immediate transfers;
 * VBlank/HBlank/special-timed and FIFO DMA (audio) are left to their subsystems. */
#define DMA_WEB_ENABLE    (1u << 15)
#define DMA_WEB_32BIT     (1u << 10)
#define DMA_WEB_SRC_FIXED (1u << 8)
#define DMA_WEB_START_MSK (3u << 12)

/* True if [p, p+n) lies inside one of the relocated GBA regions. DMA on hardware
 * targets the GBA address space; in this port those regions are the gGba* buffers.
 * A transfer whose endpoint is NOT in that space is an artifact of the decomp
 * passing a bare GBA address (e.g. a ROM offset such as 0x1) that is a bogus host
 * pointer — on hardware it would hit ROM, here it would read/write garbage, so we
 * leave it alone. Such transfers were a silent no-op before this backend emulated
 * DMA, and the game booted and rendered fine, so skipping them is the safe choice. */
static int gba_region(const void *p, size_t n)
{
    const unsigned char *q = (const unsigned char *)p;
    const struct { const unsigned char *base; size_t len; } r[] = {
        { gGbaEwram, sizeof gGbaEwram }, { gGbaIwram,  sizeof gGbaIwram  },
        { gGbaIo,    sizeof gGbaIo    }, { gGbaPalram, sizeof gGbaPalram },
        { gGbaVram,  sizeof gGbaVram  }, { gGbaOam,    sizeof gGbaOam    },
        { gGbaSram,  sizeof gGbaSram  },
    };
    for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++)
        if (q >= r[i].base && q + n <= r[i].base + r[i].len)
            return 1;
    return 0;
}

void gba_dma_set(unsigned channel, const void *src, void *dst, unsigned cnt)
{
    unsigned count = cnt & 0xFFFF;
    unsigned ctrl  = cnt >> 16;

    if (!(ctrl & DMA_WEB_ENABLE))
        return;                       /* channel disabled */
    if (ctrl & DMA_WEB_START_MSK)
        return;                       /* scheduled/FIFO DMA — not an immediate copy */

    unsigned unit  = (ctrl & DMA_WEB_32BIT) ? 4u : 2u;
    unsigned units = count ? count : (channel == 3 ? 0x10000u : 0x4000u);
    size_t   bytes = (size_t)units * unit;
    int      fill  = (ctrl & DMA_WEB_SRC_FIXED) != 0;

    /* Emulate transfers landing in the mapped GBA address space (this bounds every
     * write to a real relocated buffer). A legit copy's SOURCE is often an ordinary
     * game buffer outside those regions (e.g. the shadow OAM gOamData), so we don't
     * require src to be mapped — only that it is a plausible host pointer, which
     * rejects the decomp's bare GBA/ROM offsets (0x1, 0x2, …) that are bogus here. */
    if (!gba_region(dst, bytes))
        return;
    if (!fill && (uintptr_t)src < 0x1000u)
        return;

    if (fill) {
        if (unit == 4) {
            uint32_t v = *(const uint32_t *)src, *d = dst;
            for (unsigned i = 0; i < units; i++) d[i] = v;
        } else {
            uint16_t v = *(const uint16_t *)src, *d = dst;
            for (unsigned i = 0; i < units; i++) d[i] = v;
        }
    } else {
        memcpy(dst, src, bytes);
    }
}

int32_t DivarmDiv(int32_t number, int32_t denom) { return denom ? number / denom : 0; }
int32_t DivarmMod(int32_t denom, int32_t number) { return denom ? number % denom : 0; }

uint16_t Sqrt(uint32_t n) { return (uint16_t)(uint32_t)sqrtf((float)n); }

/* GBA BIOS LZ77 decompressors now live in src/web/bios_decompress.c (A3). */

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

/* --- m4a / MP2k sound driver (asm/soundcode.s + audio_internal.s). Weak stubs
 *     matching the real signatures (audio/track_internal.h + soundcode.s) so the
 *     call sites don't trap and Agent 2's real driver overrides them on merge. --- */
struct TrackData; /* opaque */
__attribute__((weak)) uint8_t InitTrack(struct TrackData *t, const uint32_t *h) { (void)t; (void)h; return 0; }
__attribute__((weak)) void ResetTrack(struct TrackData *t) { (void)t; }
__attribute__((weak)) void StopMusicOrSound(struct TrackData *t) { (void)t; }
__attribute__((weak)) void UpdateAudio(void) {}
__attribute__((weak)) int32_t CallGetNoteFrequency(int32_t f, uint32_t p) { (void)p; return f; }

/* --- Symbols exposed once prototypes are correct (previously masked by trapping
 *     stubs). ARM sound driver + IRQ dispatcher: weak stubs (A2 / unused by the
 *     web loop, which calls the VBlank handler directly). --- */
struct SoundChannel;
__attribute__((weak)) void CallSoundCodeA(struct SoundChannel *ch, uint32_t *b, uint8_t c) { (void)ch; (void)b; (void)c; }
__attribute__((weak)) uint8_t *CallSoundCodeB(uint32_t *a, uint32_t *b, uint32_t *c, uint8_t d) { (void)a; (void)b; (void)c; (void)d; return 0; }
__attribute__((weak)) uint8_t *CallSoundCodeC(uint32_t *a, uint32_t *b, uint8_t c) { (void)a; (void)b; (void)c; return 0; }
__attribute__((weak)) void IntrMain(void) {}

/* Audio constants (gNumMusicPlayers, gUnk_Audio*) are provided as TARGET_WEB macros
 * in structs/audio.h (linker.ld address-as-value constants). */

/* More EWRAM arrays (linker.ld), sized to their regions. */
unsigned int gUnk_02035400[0x800 / 4];
unsigned int gSramDemoInputData[0x200 / 4];
unsigned int gSramDemoInputDuration[0x200 / 4];

/* Frame sync: the game spins in `do { SYSCALL(2); } while(!gVBlankRequestFlag)` and
 * calls VBlankIntrWait once per frame. Both advance one frame via the loop (loop.c),
 * which runs the game's VBlank handler (which sets the flag) and yields to the browser. */
extern void gba_frame_tick(void); /* src/web/loop.c */

void VBlankIntrWait(void) { gba_frame_tick(); }

void gba_bios_swi(int num)
{
    (void)num; /* SYS_Halt (2): advance one frame (VBlank), then yield */
    gba_frame_tick();
}

extern void agbmain(void);

int main(void)
{
    agbmain(); /* GBA entry; runs the game's own main loop (never returns) */
    return 0;
}
