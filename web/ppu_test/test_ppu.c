/* test_ppu.c — headless harness for the MZM web-port PPU (Agent 1, Milestone 1).
 *
 * NOT part of the game build (web/build.sh only globs src/**). It stands in for the
 * host backend: it defines the relocated GBA buffers that gba_hle.c normally owns,
 * paints a synthetic mode-0 scene into VRAM/PALRAM/registers, calls the real
 * gba_ppu_render_frame(), and dumps the 240x160 RGBA framebuffer to a raw file that
 * rgba2png.py turns into a PNG for eyeballing.
 *
 * Build + run (Emscripten SDK active), from the repo root:
 *   emcc -I include -I src/web -DREGION_US -DMODERN -DTARGET_WEB -Wno-everything -O0 \
 *        src/web/ppu.c web/ppu_test/test_ppu.c -sNODERAWFS=1 -o web/ppu_test/test_ppu.js
 *   node   web/ppu_test/test_ppu.js
 *   python3 web/ppu_test/rgba2png.py web/ppu_test/out_a.rgba web/ppu_test/out_a.png 240 160
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gba_hle.h"
#include "gba/memory.h"
#include "gba/display.h"

/* Relocated GBA address space (normally defined in gba_hle.c; the PPU reads these). */
unsigned char gGbaVram[0x18000];
unsigned char gGbaPalram[0x400];
unsigned char gGbaIo[0x400];
unsigned char gGbaOam[0x400];

#define W GBA_SCREEN_W
#define H GBA_SCREEN_H
static uint32_t fb[W * H];

/* IO offsets, mirroring ppu.c / gba/display.h. */
#define IO_DISPCNT    0x00
#define IO_BGCNT(n)   (0x08 + (n) * 2)
#define IO_BGHOFS(n)  (0x10 + (n) * 4)
#define IO_BGVOFS(n)  (0x12 + (n) * 4)

static uint16_t c555(int r, int g, int b) { return (uint16_t)((b << 10) | (g << 5) | r); }
static void     wio16(uint32_t off, uint16_t v) { *(uint16_t *)(gGbaIo + off) = v; }
static uint16_t *palram(void) { return (uint16_t *)gGbaPalram; }
static uint16_t *mapat(uint32_t byteoff) { return (uint16_t *)(gGbaVram + byteoff); }

/* Pack an 8x8 block of palette indices as a 4bpp tile (32 bytes) at char byte base. */
static void put_tile4(uint32_t charByteBase, int tile, const uint8_t px[64])
{
    uint8_t *d = gGbaVram + charByteBase + tile * 32;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x += 2)
            d[y * 4 + (x >> 1)] = (uint8_t)((px[y*8+x] & 0xF) | ((px[y*8+x+1] & 0xF) << 4));
}

/* Pack an 8x8 block of palette indices as an 8bpp tile (64 bytes). */
static void put_tile8(uint32_t charByteBase, int tile, const uint8_t px[64])
{
    uint8_t *d = gGbaVram + charByteBase + tile * 64;
    for (int i = 0; i < 64; i++) d[i] = px[i];
}

static void write_raw(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "test_ppu: cannot open %s\n", path); return; }
    fwrite(fb, 4, W * H, f);
    fclose(f);
    fprintf(stderr, "test_ppu: wrote %s (%d bytes)\n", path, W * H * 4);
}

/* Scene A: 4bpp mode-0 BG0 — border grid, an asymmetric arrow tile shown normal and
 * H/V-flipped, a vertical colour ramp, two palette banks side by side, sub-tile scroll
 * with wraparound, and transparent cells revealing the backdrop. */
static void scene_a(void)
{
    memset(gGbaVram, 0, sizeof gGbaVram);
    memset(gGbaPalram, 0, sizeof gGbaPalram);
    memset(gGbaIo, 0, sizeof gGbaIo);
    uint16_t *pal = palram();

    pal[0]  = c555(3, 6, 10);                                  /* backdrop: muted blue */
    pal[1]  = c555(31, 0, 0);  pal[2] = c555(0, 31, 0);  pal[3] = c555(0, 0, 31);
    pal[4]  = c555(31, 31, 31); pal[5] = c555(31, 31, 0);
    pal[17] = c555(0, 31, 31); pal[18] = c555(31, 0, 31); pal[19] = c555(16, 16, 31);
    pal[20] = c555(31, 20, 0);  pal[21] = c555(10, 31, 10);

    uint8_t t[64];
    /* tile 0 stays all-zero = transparent. */
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)      /* tile 1: bordered fill */
        t[y*8+x] = (x==0||x==7||y==0||y==7) ? 1 : 4;
    put_tile4(0, 1, t);
    { static const uint8_t a[64] = {                            /* tile 2: arrow (asym) */
        0,0,2,0,0,0,0,0,  0,0,2,2,0,0,0,0,  2,2,2,2,2,0,0,0,  0,0,2,2,2,2,0,0,
        0,0,2,2,0,0,0,0,  0,0,2,0,0,0,0,0,  0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0 };
      put_tile4(0, 2, a); }
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)      /* tile 3: vertical ramp */
        t[y*8+x] = (uint8_t)(1 + (y * 5) / 8);
    put_tile4(0, 3, t);

    uint16_t *map = mapat(8 * 0x800);                           /* screen base block 8 */
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            uint16_t bank = (uint16_t)((tx < 16 ? 0 : 1) << 12);
            uint16_t e;
            if (tx == 0 || tx == 31 || ty == 0 || ty == 31) e = 1;      /* border */
            else if (((tx ^ ty) & 1) == 0)                   e = 3;      /* ramp */
            else {
                e = 2;                                                   /* arrow */
                if (tx & 2) e |= (1 << 10);                              /* H-flip some */
                if (ty & 2) e |= (1 << 11);                              /* V-flip some */
            }
            map[ty * 32 + tx] = (uint16_t)(e | bank);
        }
    }

    wio16(IO_DISPCNT, (uint16_t)(0 /* mode 0 */ | DCNT_BG0));
    wio16(IO_BGCNT(0), (uint16_t)CREATE_BGCNT(0, 8, 0, 0));     /* char0 screen8 prio0 256x256 */
    wio16(IO_BGHOFS(0), 4);
    wio16(IO_BGVOFS(0), 4);
}

/* Scene B: 8bpp mode-0 BG0 — a single 256-colour tile sweeping indices 1..64 across a
 * blue->white ramp, tiled across the screen. Exercises the 8bpp sampling path. */
static void scene_b(void)
{
    memset(gGbaVram, 0, sizeof gGbaVram);
    memset(gGbaPalram, 0, sizeof gGbaPalram);
    memset(gGbaIo, 0, sizeof gGbaIo);
    uint16_t *pal = palram();

    pal[0] = c555(2, 2, 6);
    for (int i = 1; i < 64; i++) { int v = (i * 31) / 63; pal[i] = c555(v, v, 31); }
    pal[64] = c555(31, 31, 31);

    uint8_t px[64];
    for (int i = 0; i < 64; i++) px[i] = (uint8_t)(1 + i);      /* indices 1..64 */
    put_tile8(0, 1, px);

    uint16_t *map = mapat(8 * 0x800);
    for (int i = 0; i < 32 * 32; i++) map[i] = 1;

    wio16(IO_DISPCNT, (uint16_t)(0 | DCNT_BG0));
    wio16(IO_BGCNT(0), (uint16_t)(CREATE_BGCNT(0, 8, 0, 0) | (1 << 7)));  /* 256-colour */
}

int main(void)
{
    scene_a();
    gba_ppu_render_frame(fb);
    write_raw("web/ppu_test/out_a.rgba");

    scene_b();
    gba_ppu_render_frame(fb);
    write_raw("web/ppu_test/out_b.rgba");
    return 0;
}
