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

/* ===== numeric-assert infrastructure (milestones 2+) ====================== */
#define OBJV 0x10000u   /* OBJ tile region: gGbaVram + 0x10000 */

static int g_fail = 0;
static uint32_t pack555(unsigned c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}
static void chk(int x, int y, unsigned expect555, const char *name) {
    uint32_t got = fb[y * W + x], exp = pack555(expect555);
    if (got != exp) { printf("  FAIL %-28s (%3d,%3d) got %08X exp %08X\n", name, x, y, got, exp); g_fail++; }
    else            { printf("  ok   %-28s (%3d,%3d) %08X\n", name, x, y, got); }
}

/* reference color-effect math (mirrors ppu.c) for computing expected values */
static unsigned ablend(unsigned a, unsigned b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31; if (g > 31) g = 31; if (bl > 31) bl = 31;
    return (unsigned)(r | (g << 5) | (bl << 10));
}
static unsigned brighten(unsigned a, int evy) {
    int r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
    r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
    return (unsigned)(r | (g << 5) | (b << 10));
}
static unsigned darken(unsigned a, int evy) {
    int r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
    r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
    return (unsigned)(r | (g << 5) | (b << 10));
}

static uint16_t *objpalram(void) { return (uint16_t *)(gGbaPalram + 0x200); }
static void obj_tile4_solid(int t, int nib) {
    unsigned char b = (unsigned char)((nib << 4) | nib);
    for (int i = 0; i < 32; i++) gGbaVram[OBJV + (unsigned)t * 32 + i] = b;
}
static void obj_tile8_solid(int t, int idx) {   /* t in 32-byte units; 8bpp tile = 64 bytes */
    for (int i = 0; i < 64; i++) gGbaVram[OBJV + (unsigned)t * 32 + i] = (unsigned char)idx;
}
static void oam_set(int i, uint16_t a0, uint16_t a1, uint16_t a2) {
    uint16_t *o = (uint16_t *)(gGbaOam + i * 8);
    o[0] = a0; o[1] = a1; o[2] = a2;
}
static void oam_aff(int m, int pa, int pb, int pc, int pd) {   /* matrix m <- entries 4m..4m+3 */
    ((uint16_t *)(gGbaOam + (4 * m + 0) * 8))[3] = (uint16_t)pa;
    ((uint16_t *)(gGbaOam + (4 * m + 1) * 8))[3] = (uint16_t)pb;
    ((uint16_t *)(gGbaOam + (4 * m + 2) * 8))[3] = (uint16_t)pc;
    ((uint16_t *)(gGbaOam + (4 * m + 3) * 8))[3] = (uint16_t)pd;
}
static void reset_mem(void) {
    memset(gGbaVram, 0, sizeof gGbaVram);
    memset(gGbaPalram, 0, sizeof gGbaPalram);
    memset(gGbaOam, 0, sizeof gGbaOam);
    memset(gGbaIo, 0, sizeof gGbaIo);
    /* Disable every sprite by default (bit9 of attr0), as real games do; scenes
     * re-enable the few they use. Otherwise a zeroed entry is an 8x8 sprite at
     * (0,0) pointing at tile 0. */
    for (int i = 0; i < 128; i++) ((uint16_t *)(gGbaOam + i * 8))[0] = 0x0200;
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

/* ===== shared: BG0 solid red (prio1), BG1 solid green (prio0, front) ======= */
static void two_bg_solid(void)
{
    uint16_t *p = palram();
    p[0] = c555(2,2,2); p[1] = c555(31,0,0); p[17] = c555(0,31,0);
    for (int i = 0; i < 32; i++) gGbaVram[i] = 0x11;                 /* tile0 = solid idx1 */
    uint16_t *m0 = mapat(8 * 0x800); for (int i = 0; i < 32*32; i++) m0[i] = 0;        /* bank0 */
    uint16_t *m1 = mapat(9 * 0x800); for (int i = 0; i < 32*32; i++) m1[i] = (1 << 12); /* bank1 */
    wio16(IO_BGCNT(0), (uint16_t)CREATE_BGCNT(0, 8, 1, 0));
    wio16(IO_BGCNT(1), (uint16_t)CREATE_BGCNT(0, 9, 0, 0));
}

/* ===== Scene windows: WIN0 rect masking + WINOUT + H-edge clamp ============ */
static void scene_win_mask(void)
{
    unsigned RED = c555(31,0,0), GRN = c555(0,31,0);
    printf("- windows: WIN0 masking / WINOUT / edge clamp:\n");

    reset_mem(); two_bg_solid();
    wio16(0x40, (60 << 8) | 120);                 /* WIN0H x1=60 x2=120 */
    wio16(0x44, (40 << 8) | 100);                 /* WIN0V y1=40 y2=100 */
    wio16(0x48, 0x0001);                          /* WININ : WIN0 -> BG0 only */
    wio16(0x4A, 0x0003);                          /* WINOUT: BG0 + BG1 */
    wio16(IO_DISPCNT, (uint16_t)(DCNT_BG0 | DCNT_BG1 | DCNT_WIN0));
    gba_ppu_render_frame(fb);
    chk(90,70,  RED, "inside WIN0: BG1 masked");
    chk(10,10,  GRN, "outside: BG1 shows");
    chk(90,30,  GRN, "above WIN0 (y<40)");
    chk(130,70, GRN, "right of WIN0 (x>=120)");

    wio16(0x40, (200 << 8) | 250);                /* x2=250 > 240 -> clamp to 240 */
    gba_ppu_render_frame(fb);
    chk(235,70, RED, "x2>240 clamp: inside near edge");
    chk(195,70, GRN, "x2>240 clamp: outside");

    wio16(0x40, (120 << 8) | 40);                 /* x2<x1 -> clamp right to 240 */
    gba_ppu_render_frame(fb);
    chk(200,70, RED, "x2<x1 clamp: [120,240) inside");
    chk(60,70,  GRN, "x2<x1 clamp: outside");
}

/* ===== Scene windows: per-region color-effect-enable (bit5) ================ */
static void scene_win_effect(void)
{
    unsigned GRN = c555(0,31,0), BLACK = c555(0,0,0);
    printf("- windows: color-effect-enable bit5:\n");

    reset_mem(); two_bg_solid();
    wio16(0x50, (1 << 1) | (3 << 6));             /* BLDCNT: BG1 first target, darken */
    wio16(0x54, 16);                              /* BLDY evy=16 (full darken) */
    wio16(0x40, (60 << 8) | 120);
    wio16(0x44, (40 << 8) | 100);
    wio16(0x48, 0x0003 | (1 << 5));               /* WININ : BG0+BG1 + effect ON */
    wio16(0x4A, 0x0003);                          /* WINOUT: BG0+BG1, effect OFF */
    wio16(IO_DISPCNT, (uint16_t)(DCNT_BG0 | DCNT_BG1 | DCNT_WIN0));
    gba_ppu_render_frame(fb);
    chk(90,70, BLACK, "inside: darken enabled -> black");
    chk(10,10, GRN,   "outside: darken disabled -> green");
}

/* ===== Scene blending: alpha between two BGs =============================== */
static void scene_blend_bg(void)
{
    printf("- blending: alpha between two BGs:\n");
    reset_mem(); two_bg_solid();
    palram()[17] = c555(0,0,31);                  /* BG1 -> blue */
    unsigned A = c555(31,0,0), B = c555(0,0,31);
    wio16(0x50, (1 << 1) | (1 << 8) | (1 << 6));  /* BG1 first, BG0 second, alpha */
    wio16(0x52, (8 << 8) | 8);                    /* eva=8 evb=8 */
    wio16(IO_DISPCNT, (uint16_t)(DCNT_BG0 | DCNT_BG1));
    gba_ppu_render_frame(fb);
    chk(50,50, ablend(B, A, 8, 8), "alpha BG1 over BG0");
}

/* ===== Scene blending: BLDY brighten & darken ============================== */
static void scene_blend_bldy(void)
{
    printf("- blending: BLDY brighten & darken:\n");
    reset_mem();
    palram()[0] = c555(2,2,2); palram()[1] = c555(16,16,16);
    for (int i = 0; i < 32; i++) gGbaVram[i] = 0x11;
    uint16_t *m = mapat(8 * 0x800); for (int i = 0; i < 32*32; i++) m[i] = 0;
    wio16(IO_BGCNT(0), (uint16_t)CREATE_BGCNT(0, 8, 0, 0));
    unsigned G = c555(16,16,16);
    wio16(IO_DISPCNT, (uint16_t)DCNT_BG0);
    wio16(0x54, 8);                               /* evy=8 */
    wio16(0x50, (1 << 0) | (2 << 6));             /* BG0 first, brighten */
    gba_ppu_render_frame(fb);
    chk(50,50, brighten(G, 8), "BLDY brighten BG0");
    wio16(0x50, (1 << 0) | (3 << 6));             /* BG0 first, darken */
    gba_ppu_render_frame(fb);
    chk(50,50, darken(G, 8), "BLDY darken BG0");
}

/* ===== Scene blending: semi-transparent OBJ (mode 1) over a BG ============= */
static void scene_blend_obj(void)
{
    printf("- blending: semi-transparent OBJ over BG:\n");
    reset_mem();
    palram()[0] = c555(2,2,2); palram()[1] = c555(31,0,0);   /* BG0 red */
    objpalram()[1] = c555(0,0,31);                           /* OBJ blue */
    for (int i = 0; i < 32; i++) gGbaVram[i] = 0x11;
    uint16_t *m = mapat(8 * 0x800); for (int i = 0; i < 32*32; i++) m[i] = 0;
    wio16(IO_BGCNT(0), (uint16_t)CREATE_BGCNT(0, 8, 1, 0));   /* BG0 prio1 */
    for (int t = 0; t < 4; t++) obj_tile4_solid(t, 1);       /* blue 16x16 sprite */
    oam_set(0, 40 | (1 << 10) | (0 << 14), 40 | (1 << 14), 0); /* semi (mode1) prio0 @ (40,40) */
    wio16(0x52, (8 << 8) | 8);
    unsigned A = c555(31,0,0), B = c555(0,0,31);

    wio16(0x50, (1 << 8));                        /* BG0 is a 2nd target -> semi obj blends */
    wio16(IO_DISPCNT, (uint16_t)(DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D));
    gba_ppu_render_frame(fb);
    chk(48,48, ablend(B, A, 8, 8), "semi OBJ blends with BG");
    chk(10,10, A, "BG where no sprite");

    wio16(0x50, 0);                               /* no 2nd target -> semi obj shown plain */
    gba_ppu_render_frame(fb);
    chk(48,48, B, "semi OBJ, no 2nd target -> plain");
}

/* ===== Scene OBJ-map: 16x16 sprites, 4bpp & 8bpp, 1D vs 2D mapping, flips === */
static void scene_obj_map(int map1d)
{
    reset_mem();
    uint16_t *bp = palram(); bp[0] = c555(4, 4, 4);              /* backdrop */
    uint16_t *op = objpalram();
    op[1] = c555(31,0,0); op[2] = c555(0,31,0); op[3] = c555(0,0,31); op[4] = c555(31,31,0);
    op[10]= c555(31,0,0); op[11]= c555(0,31,0); op[12]= c555(0,0,31); op[13]= c555(31,31,0);

    /* 4bpp quadrant tiles for a 16x16 sprite at base 0 (per-mapping layout). */
    int BL = map1d ? 2 : 32, BR = map1d ? 3 : 33;
    obj_tile4_solid(0, 1); obj_tile4_solid(1, 2); obj_tile4_solid(BL, 3); obj_tile4_solid(BR, 4);
    /* 8bpp quadrant tiles at base 64 (step 2 per column; row step 4 in 1D / 32 in 2D). */
    int b8 = 64, R8 = b8 + 2, BL8 = map1d ? b8 + 4 : b8 + 32, BR8 = map1d ? b8 + 6 : b8 + 34;
    obj_tile8_solid(b8, 10); obj_tile8_solid(R8, 11); obj_tile8_solid(BL8, 12); obj_tile8_solid(BR8, 13);

    oam_set(0, 40 | (0 << 14),              40 | (1 << 14), 0);            /* 4bpp @ (40,40) */
    oam_set(1, 80 | (0 << 14) | (1 << 13),  40 | (1 << 14), 64);          /* 8bpp @ (40,80) */
    oam_set(2, 40 | (0 << 14),              80 | (1 << 14) | (1 << 12), 0); /* Hflip @ (80,40) */
    oam_set(3, 80 | (0 << 14),              80 | (1 << 14) | (1 << 13), 0); /* Vflip @ (80,80) */
    wio16(IO_DISPCNT, (uint16_t)(DCNT_OBJ | (map1d ? DCNT_OBJ_1D : 0)));

    gba_ppu_render_frame(fb);
    unsigned RED = c555(31,0,0), GRN = c555(0,31,0), BLU = c555(0,0,31), YEL = c555(31,31,0);
    printf("- OBJ %s mapping:\n", map1d ? "1D" : "2D");
    chk(44,44,RED,"4bpp quad TL"); chk(52,44,GRN,"4bpp quad TR");
    chk(44,52,BLU,"4bpp quad BL"); chk(52,52,YEL,"4bpp quad BR");
    chk(44,84,RED,"8bpp quad TL"); chk(52,84,GRN,"8bpp quad TR");
    chk(44,92,BLU,"8bpp quad BL"); chk(52,92,YEL,"8bpp quad BR");
    chk(84,44,GRN,"Hflip TL<-TR"); chk(92,44,RED,"Hflip TR<-TL");
    chk(84,84,BLU,"Vflip TL<-BL"); chk(84,92,RED,"Vflip BL<-TL");
}

/* ===== Scene OBJ-misc: OBJ-vs-BG priority ties + affine double-size ======== */
static void scene_obj_misc(void)
{
    reset_mem();
    uint16_t *bp = palram(); bp[0] = c555(4,4,4); bp[1] = c555(28,28,28); /* backdrop + BG0 gray */
    uint16_t *op = objpalram(); op[1] = c555(31,0,0); op[2] = c555(0,31,0);

    for (int i = 0; i < 32; i++) gGbaVram[i] = 0x11;                   /* BG tile0 = solid idx1 */
    uint16_t *map = mapat(8 * 0x800); for (int i = 0; i < 32*32; i++) map[i] = 0;
    wio16(IO_BGCNT(0), (uint16_t)CREATE_BGCNT(0, 8, 1, 0));            /* BG0 prio 1 */

    for (int t = 0; t < 4; t++) obj_tile4_solid(t, 1);                /* sprite red tiles 0..3 */
    for (int t = 4; t < 8; t++) obj_tile4_solid(t, 2);                /* sprite green tiles 4..7 */

    oam_set(0, 0x0200, 0, 0); oam_set(1, 0x0200, 0, 0);               /* entries 0-3: disabled, */
    oam_set(2, 0x0200, 0, 0); oam_set(3, 0x0200, 0, 0);               /* hold matrix-0 params   */
    oam_aff(0, 0x100, 0, 0, 0x100);                                   /* identity */

    oam_set(4, 60 | (0<<14), 100 | (1<<14), 0 | (1<<10));             /* A: prio1 == BG0 */
    oam_set(5, 60 | (0<<14), 140 | (1<<14), 4 | (2<<10));             /* B: prio2  > BG0 */
    oam_set(6, 100 | (1<<8) | (1<<9) | (0<<14), 40 | (0<<9) | (1<<14), 0); /* C: affine+double, mtx0 */
    wio16(IO_DISPCNT, (uint16_t)(DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D));

    gba_ppu_render_frame(fb);
    write_raw("web/ppu_test/out_obj.rgba");
    unsigned GRAY = c555(28,28,28), RED = c555(31,0,0);
    printf("- OBJ priority + affine:\n");
    chk(108,68, RED,  "OBJ wins prio tie vs BG");
    chk(148,68, GRAY, "BG wins (OBJ prio# higher)");
    chk(56,116, RED,  "affine double-size center");
    chk(42,102, GRAY, "affine double box corner");
    chk(44,116, GRAY, "affine outside-texture");
}

int main(void)
{
    scene_a();
    gba_ppu_render_frame(fb);
    write_raw("web/ppu_test/out_a.rgba");

    scene_b();
    gba_ppu_render_frame(fb);
    write_raw("web/ppu_test/out_b.rgba");

    printf("== PPU numeric asserts ==\n");
    scene_obj_map(1);
    scene_obj_map(0);
    scene_obj_misc();
    scene_win_mask();
    scene_win_effect();
    scene_blend_bg();
    scene_blend_bldy();
    scene_blend_obj();

    printf("\n%s  (%d checks failed)\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS", g_fail);
    return g_fail ? 1 : 0;
}
