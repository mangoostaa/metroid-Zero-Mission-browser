/* ppu.c -- software GBA PPU for the Metroid: Zero Mission web port (Agent 1).
 *
 * Rasterizes the current GBA graphics state into a 240x160 RGBA framebuffer.
 * The GBA address space is read through the relocated buffers that gba_hle.c
 * defines and gba/memory.h points its BASE macros at (gGbaVram, gGbaOam,
 * gGbaPalram, gGbaIo). No game C is touched; this is pure host-side raster.
 *
 * ---------------------------------------------------------------------------
 * 32-bit output packing (DOCUMENTED CONTRACT):
 *   Each out_rgba[] u32 holds, in little-endian MEMORY order, the bytes
 *   R, G, B, A -- exactly what an HTML canvas ImageData (Uint8ClampedArray,
 *   [R,G,B,A,...]) expects, so the buffer can be blitted with no swizzle.
 *   On little-endian that u32 is:  (0xFF<<24) | (B<<16) | (G<<8) | R.
 *   Alpha is always 0xFF (GBA framebuffer output is opaque).
 * ---------------------------------------------------------------------------
 *
 * Implemented: BG mode 0 text layers (4bpp/8bpp, H/V scroll, all 4 map sizes,
 * per-BG priority), affine BGs (modes 1/2, wrap/transparent overflow), bitmap
 * modes 3/4/5, OBJ sprites (1D & 2D tile mapping, H/V flip, affine + double
 * size, priority vs BG, semi-transparent, OBJ-window), windows (WIN0/WIN1/
 * OBJ/OUT incl. per-region color-effect enable), color special effects (alpha
 * blend + brightness increase/decrease), and BG/OBJ mosaic. Behavior
 * cross-checked against GBATEK.
 */
#include <stdint.h>
#include "gba_hle.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t  s16;
typedef int32_t  s32;

#define SCRW GBA_SCREEN_W   /* 240 */
#define SCRH GBA_SCREEN_H   /* 160 */

#define VRAM_LIMIT 0x18000u

/* ---- little-endian readers over the relocated buffers -------------------- */
static inline u16 rd16(const u8 *p, u32 o) { return (u16)(p[o] | (p[o + 1] << 8)); }
static inline u32 rd32(const u8 *p, u32 o) {
    return (u32)p[o] | ((u32)p[o + 1] << 8) | ((u32)p[o + 2] << 16) | ((u32)p[o + 3] << 24);
}
static inline u16 io16(u32 o) { return rd16(gGbaIo, o); }
/* VRAM byte read, guarded so char/map math that runs past the 0x18000 window
 * (legal on HW, which mirrors) can never index out of the host buffer. */
static inline u8  vram8(u32 a) { return (a < VRAM_LIMIT) ? gGbaVram[a] : 0; }
static inline u16 vram16(u32 a) { return (u16)(vram8(a) | (vram8(a + 1) << 8)); }

static inline u16 bgpal(int i)  { return rd16(gGbaPalram, (u32)((i & 0xFF) * 2)); }
static inline u16 objpal(int i) { return rd16(gGbaPalram, (u32)(0x200 + (i & 0xFF) * 2)); }

/* BGR555 (5 bits/channel) -> packed R,G,B,A bytes (little-endian u32). */
static inline u32 pack555(u16 c) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* ---- per-scanline working buffers ---------------------------------------- */
static u16 sBgColor[4][SCRW];
static u8  sBgOpaque[4][SCRW];
static u8  sBgPrio[4];
static u8  sBgEnabled[4];

static u16 sObjColor[SCRW];
static u8  sObjPrio[SCRW];    /* 0..3 ; 0xFF = no sprite pixel here */
static u8  sObjSemi[SCRW];    /* top sprite pixel is semi-transparent (mode 1) */
static u8  sObjWin[SCRW];     /* covered by an OBJ-window sprite (mode 2) */

static u8  sWinLayer[SCRW];   /* bits 0-4: BG0,BG1,BG2,BG3,OBJ visible here */
static u8  sWinEffect[SCRW];  /* 1 = color special effect allowed here */

/* OBJ dimensions [shape][size] -> {w,h} in pixels. */
static const u8 kObjDims[3][4][2] = {
    { {8, 8},  {16, 16}, {32, 32}, {64, 64} },  /* square     */
    { {16, 8}, {32, 8},  {32, 16}, {64, 32} },  /* horizontal */
    { {8, 16}, {8, 32},  {16, 32}, {32, 64} },  /* vertical   */
};

/* ---- text background scanline -------------------------------------------- */
static void render_text_bg(int bg, int y)
{
    u16 bgcnt = io16(0x08 + bg * 2);
    sBgPrio[bg] = bgcnt & 3;
    u32 charBase   = (u32)((bgcnt >> 2) & 3) * 0x4000;
    u32 screenBase = (u32)((bgcnt >> 8) & 0x1F) * 0x800;
    int bpp8   = (bgcnt >> 7) & 1;
    int mosaic = (bgcnt >> 6) & 1;
    int size   = (bgcnt >> 14) & 3;
    int mapW = (size & 1) ? 512 : 256;
    int mapH = (size & 2) ? 512 : 256;
    int hofs = io16(0x10 + bg * 4) & 0x1FF;
    int vofs = io16(0x12 + bg * 4) & 0x1FF;
    int mosH = 1, mosV = 1;
    if (mosaic) { u16 m = io16(0x4C); mosH = (m & 0xF) + 1; mosV = ((m >> 4) & 0xF) + 1; }

    for (int x = 0; x < SCRW; x++) {
        int px = x, py = y;
        if (mosaic) { px = x - (x % mosH); py = y - (y % mosV); }
        int sx = (px + hofs) & (mapW - 1);
        int sy = (py + vofs) & (mapH - 1);
        int tileX = sx >> 3, tileY = sy >> 3;
        int blockX = tileX >> 5, blockY = tileY >> 5;
        int inTX = tileX & 31, inTY = tileY & 31;
        int sb;
        switch (size) {
            case 1:  sb = blockX; break;
            case 2:  sb = blockY; break;
            case 3:  sb = blockY * 2 + blockX; break;
            default: sb = 0; break;
        }
        u16 e = vram16(screenBase + (u32)sb * 0x800 + (u32)(inTY * 32 + inTX) * 2);
        int tnum = e & 0x3FF, hf = (e >> 10) & 1, vf = (e >> 11) & 1, palbank = (e >> 12) & 0xF;
        int ix = sx & 7, iy = sy & 7;
        if (hf) ix = 7 - ix;
        if (vf) iy = 7 - iy;
        u16 color; int opaque;
        if (bpp8) {
            int idx = vram8(charBase + (u32)tnum * 64 + (u32)(iy * 8 + ix));
            opaque = idx != 0; color = bgpal(idx);
        } else {
            u8 byte = vram8(charBase + (u32)tnum * 32 + (u32)(iy * 4 + (ix >> 1)));
            int nib = (ix & 1) ? (byte >> 4) : (byte & 0xF);
            opaque = nib != 0; color = bgpal(palbank * 16 + nib);
        }
        sBgColor[bg][x] = color;
        sBgOpaque[bg][x] = (u8)opaque;
    }
}

/* ---- affine background scanline (BG2 in mode 1/2, BG3 in mode 2) ---------- */
static void render_affine_bg(int bg, int y)
{
    u16 bgcnt = io16(0x08 + bg * 2);
    sBgPrio[bg] = bgcnt & 3;
    u32 charBase   = (u32)((bgcnt >> 2) & 3) * 0x4000;
    u32 screenBase = (u32)((bgcnt >> 8) & 0x1F) * 0x800;
    int wrap   = (bgcnt >> 13) & 1;
    int mosaic = (bgcnt >> 6) & 1;
    int size   = (bgcnt >> 14) & 3;
    int mapTiles = 16 << size;        /* 16,32,64,128 tiles square */
    int mapPix   = mapTiles * 8;
    u32 base = (bg == 2) ? 0x20 : 0x30;   /* PA..PD, then X(+8), Y(+12) */
    s32 pa = (s16)io16(base + 0), pb = (s16)io16(base + 2);
    s32 pc = (s16)io16(base + 4), pd = (s16)io16(base + 6);
    s32 refX = (s32)rd32(gGbaIo, base + 8);  refX = (refX << 4) >> 4;  /* sign-extend 28->32 */
    s32 refY = (s32)rd32(gGbaIo, base + 12); refY = (refY << 4) >> 4;
    int mosH = 1, mosV = 1;
    if (mosaic) { u16 m = io16(0x4C); mosH = (m & 0xF) + 1; mosV = ((m >> 4) & 0xF) + 1; }

    for (int x = 0; x < SCRW; x++) {
        int sx = x, sy = y;
        if (mosaic) { sx = x - (x % mosH); sy = y - (y % mosV); }
        s32 fx = refX + pa * sx + pb * sy;
        s32 fy = refY + pc * sx + pd * sy;
        int tX = fx >> 8, tY = fy >> 8;
        if (wrap) { tX &= (mapPix - 1); tY &= (mapPix - 1); }
        else if (tX < 0 || tX >= mapPix || tY < 0 || tY >= mapPix) { sBgOpaque[bg][x] = 0; continue; }
        int tnum = vram8(screenBase + (u32)((tY >> 3) * mapTiles + (tX >> 3)));
        int idx  = vram8(charBase + (u32)tnum * 64 + (u32)((tY & 7) * 8 + (tX & 7)));
        sBgOpaque[bg][x] = (u8)(idx != 0);
        sBgColor[bg][x]  = bgpal(idx);
    }
}

/* ---- bitmap modes 3/4/5 (BG2, affine-sampled) ---------------------------- */
static void render_bitmap_bg(int mode, int y)
{
    u16 bgcnt = io16(0x0C);
    sBgPrio[2] = bgcnt & 3;
    s32 pa = (s16)io16(0x20), pb = (s16)io16(0x22);
    s32 pc = (s16)io16(0x24), pd = (s16)io16(0x26);
    s32 refX = (s32)rd32(gGbaIo, 0x28); refX = (refX << 4) >> 4;
    s32 refY = (s32)rd32(gGbaIo, 0x2C); refY = (refY << 4) >> 4;
    int page = (io16(0x00) >> 4) & 1;
    int w = (mode == 5) ? 160 : 240;
    int h = (mode == 5) ? 128 : 160;
    u32 pbase = (mode == 4 || mode == 5) && page ? 0xA000 : 0x0000;

    for (int x = 0; x < SCRW; x++) {
        s32 fx = refX + pa * x + pb * y;
        s32 fy = refY + pc * x + pd * y;
        int tX = fx >> 8, tY = fy >> 8;
        if (tX < 0 || tX >= w || tY < 0 || tY >= h) { sBgOpaque[2][x] = 0; continue; }
        if (mode == 4) {
            int idx = vram8(pbase + (u32)(tY * w + tX));
            sBgOpaque[2][x] = (u8)(idx != 0);
            sBgColor[2][x]  = bgpal(idx);
        } else {
            sBgColor[2][x]  = vram16(pbase + (u32)(tY * w + tX) * 2);
            sBgOpaque[2][x] = 1;   /* 16bpp bitmap: every in-range pixel is opaque */
        }
    }
}

/* ---- sprite one-pixel fetch (returns -1 if transparent, else BGR555) ------ */
static int obj_texel(int tx, int ty, int w, int tileBase, int bpp8, int palBank, int map1d)
{
    int tileX = tx >> 3, tileY = ty >> 3;
    int tw = w >> 3;
    int step = bpp8 ? 2 : 1;   /* 8bpp tiles occupy two 32-byte slots */
    int tnum = map1d ? (tileBase + (tileY * tw + tileX) * step)
                     : (tileBase + tileY * 32 + tileX * step);
    tnum &= 0x3FF;
    u32 addr = 0x10000u + (u32)tnum * 32;   /* OBJ tiles live at VRAM + 0x10000 */
    int inX = tx & 7, inY = ty & 7;
    if (bpp8) {
        int idx = vram8(addr + (u32)(inY * 8 + inX));
        return idx ? objpal(idx) : -1;
    }
    u8 byte = vram8(addr + (u32)(inY * 4 + (inX >> 1)));
    int nib = (inX & 1) ? (byte >> 4) : (byte & 0xF);
    return nib ? objpal(palBank * 16 + nib) : -1;
}

/* ---- OBJ scanline: fills sObjColor/Prio/Semi and sObjWin ------------------ */
static void render_objects(int y, u16 dispcnt)
{
    int map1d = (dispcnt >> 6) & 1;
    u16 mos = io16(0x4C);
    int omH = ((mos >> 8) & 0xF) + 1, omV = ((mos >> 12) & 0xF) + 1;

    for (int s = 0; s < 128; s++) {
        u16 a0 = rd16(gGbaOam, (u32)s * 8 + 0);
        u16 a1 = rd16(gGbaOam, (u32)s * 8 + 2);
        u16 a2 = rd16(gGbaOam, (u32)s * 8 + 4);
        int affine = (a0 >> 8) & 1;
        if (!affine && ((a0 >> 9) & 1)) continue;        /* disable bit */
        int dbl    = affine && ((a0 >> 9) & 1);          /* double-size box */
        int objMode = (a0 >> 10) & 3;                    /* 0 normal,1 semi,2 window */
        int mosaic = (a0 >> 12) & 1;
        int bpp8   = (a0 >> 13) & 1;
        int shape  = (a0 >> 14) & 3;
        if (shape == 3) continue;
        int objY = a0 & 0xFF;
        int size = (a1 >> 14) & 3;
        int w = kObjDims[shape][size][0], h = kObjDims[shape][size][1];
        int boxW = dbl ? w * 2 : w, boxH = dbl ? h * 2 : h;
        int objX = a1 & 0x1FF;
        int row = (y - objY) & 0xFF;                     /* y wraps as 8-bit */
        if (row >= boxH) continue;
        int tileBase = a2 & 0x3FF, prio = (a2 >> 10) & 3, palBank = (a2 >> 12) & 0xF;

        s32 pa = 0x100, pb = 0, pc = 0, pd = 0x100;      /* identity (8.8) */
        int hf = 0, vf = 0;
        if (affine) {
            int m = (a1 >> 9) & 0x1F;
            pa = (s16)rd16(gGbaOam, (u32)(m * 4 + 0) * 8 + 6);
            pb = (s16)rd16(gGbaOam, (u32)(m * 4 + 1) * 8 + 6);
            pc = (s16)rd16(gGbaOam, (u32)(m * 4 + 2) * 8 + 6);
            pd = (s16)rd16(gGbaOam, (u32)(m * 4 + 3) * 8 + 6);
        } else {
            hf = (a1 >> 12) & 1; vf = (a1 >> 13) & 1;
        }

        for (int px = 0; px < boxW; px++) {
            int screenX = (objX + px) & 0x1FF;
            if (screenX >= SCRW) continue;               /* off-screen / wrap-left */
            int bx = px, by = row;
            if (mosaic) {                                /* snap to mosaic grid */
                bx = px - (screenX % omH);
                by = ((y - (y % omV)) - objY) & 0xFF;
                if (bx < 0) bx = 0;
            }
            int tx, ty;
            if (affine) {
                int dx = bx - boxW / 2, dy = by - boxH / 2;
                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || tx >= w || ty < 0 || ty >= h) continue;
            } else {
                tx = hf ? (w - 1 - bx) : bx;
                ty = vf ? (h - 1 - by) : by;
            }
            int col = obj_texel(tx, ty, w, tileBase, bpp8, palBank, map1d);
            if (col < 0) continue;                       /* transparent texel */
            if (objMode == 2) { sObjWin[screenX] = 1; continue; }   /* OBJ window */
            if (sObjPrio[screenX] == 0xFF || prio < sObjPrio[screenX]) {
                sObjColor[screenX] = (u16)col;
                sObjPrio[screenX]  = (u8)prio;
                sObjSemi[screenX]  = (u8)(objMode == 1);
            }
        }
    }
}

/* ---- window masks for one scanline --------------------------------------- */
static void compute_windows(int y, u16 dispcnt)
{
    int w0en = (dispcnt >> 13) & 1, w1en = (dispcnt >> 14) & 1, wobjen = (dispcnt >> 15) & 1;
    if (!w0en && !w1en && !wobjen) {
        for (int x = 0; x < SCRW; x++) { sWinLayer[x] = 0x1F; sWinEffect[x] = 1; }
        return;
    }
    u16 winin = io16(0x48), winout = io16(0x4A);
    int in0 = winin & 0x3F, in1 = (winin >> 8) & 0x3F;
    int outc = winout & 0x3F, inobj = (winout >> 8) & 0x3F;

    u16 w0h = io16(0x40), w0v = io16(0x44), w1h = io16(0x42), w1v = io16(0x46);
    int x0a = (w0h >> 8) & 0xFF, x0b = w0h & 0xFF; if (x0b > SCRW || x0a > x0b) x0b = SCRW;
    int y0a = (w0v >> 8) & 0xFF, y0b = w0v & 0xFF; if (y0b > SCRH || y0a > y0b) y0b = SCRH;
    int x1a = (w1h >> 8) & 0xFF, x1b = w1h & 0xFF; if (x1b > SCRW || x1a > x1b) x1b = SCRW;
    int y1a = (w1v >> 8) & 0xFF, y1b = w1v & 0xFF; if (y1b > SCRH || y1a > y1b) y1b = SCRH;
    int in0v = w0en && (y >= y0a && y < y0b);
    int in1v = w1en && (y >= y1a && y < y1b);

    for (int x = 0; x < SCRW; x++) {
        int ctrl;
        if (in0v && x >= x0a && x < x0b)             ctrl = in0;
        else if (in1v && x >= x1a && x < x1b)        ctrl = in1;
        else if (wobjen && sObjWin[x])               ctrl = inobj;
        else                                         ctrl = outc;
        sWinLayer[x]  = (u8)(ctrl & 0x1F);
        sWinEffect[x] = (u8)((ctrl >> 5) & 1);
    }
}

/* ---- color special-effect helpers (operate in 5-bit channel space) ------- */
static u16 blend_alpha(u16 a, u16 b, int eva, int evb)
{
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31; if (g > 31) g = 31; if (bl > 31) bl = 31;
    return (u16)(r | (g << 5) | (bl << 10));
}
static u16 blend_brighten(u16 a, int evy)
{
    int r = (a & 31), g = ((a >> 5) & 31), b = ((a >> 10) & 31);
    r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
    return (u16)(r | (g << 5) | (b << 10));
}
static u16 blend_darken(u16 a, int evy)
{
    int r = (a & 31), g = ((a >> 5) & 31), b = ((a >> 10) & 31);
    r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
    return (u16)(r | (g << 5) | (b << 10));
}

/* ---- composite one scanline into the framebuffer ------------------------- */
static void composite(int y, u32 *line)
{
    u16 backdrop = bgpal(0);
    u16 bldcnt = io16(0x50);
    int effect = (bldcnt >> 6) & 3;
    u16 bldalpha = io16(0x52), bldy = io16(0x54);
    int eva = bldalpha & 0x1F; if (eva > 16) eva = 16;
    int evb = (bldalpha >> 8) & 0x1F; if (evb > 16) evb = 16;
    int evy = bldy & 0x1F; if (evy > 16) evy = 16;

    for (int x = 0; x < SCRW; x++) {
        int topL = -1, secL = -1, topSemi = 0;
        u16 topC = 0, secC = 0;
        /* Resolve the two front-most opaque layers. Within a priority level the
         * OBJ layer sits above the BGs, and BG0 above BG1 above BG2 above BG3. */
        for (int pri = 0; pri < 4 && secL < 0; pri++) {
            if (sObjPrio[x] == pri && (sWinLayer[x] & 0x10)) {
                if (topL < 0) { topL = 4; topC = sObjColor[x]; topSemi = sObjSemi[x]; }
                else          { secL = 4; secC = sObjColor[x]; }
            }
            for (int bg = 0; bg < 4 && secL < 0; bg++) {
                if (sBgEnabled[bg] && sBgPrio[bg] == pri && sBgOpaque[bg][x]
                    && (sWinLayer[x] & (1 << bg))) {
                    if (topL < 0) { topL = bg; topC = sBgColor[bg][x]; }
                    else          { secL = bg; secC = sBgColor[bg][x]; }
                }
            }
        }
        if (topL < 0) { topL = 5; topC = backdrop; }   /* backdrop = BG palette 0 */
        if (secL < 0) { secL = 5; secC = backdrop; }

        u16 result = topC;
        int fx = sWinEffect[x];
        int secondIsTarget = (bldcnt >> (8 + secL)) & 1;
        if (topSemi) {
            /* Semi-transparent OBJ is always an alpha first target. */
            if (fx && secondIsTarget) result = blend_alpha(topC, secC, eva, evb);
        } else if (fx) {
            int firstIsTarget = (bldcnt >> topL) & 1;
            if (effect == 1) {
                if (firstIsTarget && secondIsTarget) result = blend_alpha(topC, secC, eva, evb);
            } else if (effect == 2) {
                if (firstIsTarget) result = blend_brighten(topC, evy);
            } else if (effect == 3) {
                if (firstIsTarget) result = blend_darken(topC, evy);
            }
        }
        line[x] = pack555(result);
    }
}

/* ---- public entry point -------------------------------------------------- */
void gba_ppu_render_frame(uint32_t *out_rgba)
{
    u16 dispcnt = io16(0x00);
    int mode = dispcnt & 7;

    for (int y = 0; y < SCRH; y++) {
        u32 *line = out_rgba + (u32)y * SCRW;

        if (dispcnt & (1 << 7)) {                 /* forced blank -> white */
            for (int x = 0; x < SCRW; x++) line[x] = pack555(0x7FFF);
            continue;
        }

        for (int x = 0; x < SCRW; x++) { sObjPrio[x] = 0xFF; sObjSemi[x] = 0; sObjWin[x] = 0; }
        sBgEnabled[0] = sBgEnabled[1] = sBgEnabled[2] = sBgEnabled[3] = 0;

        if (mode == 0) {
            for (int bg = 0; bg < 4; bg++)
                if (dispcnt & (1 << (8 + bg))) { sBgEnabled[bg] = 1; render_text_bg(bg, y); }
        } else if (mode == 1) {
            if (dispcnt & (1 << 8))  { sBgEnabled[0] = 1; render_text_bg(0, y); }
            if (dispcnt & (1 << 9))  { sBgEnabled[1] = 1; render_text_bg(1, y); }
            if (dispcnt & (1 << 10)) { sBgEnabled[2] = 1; render_affine_bg(2, y); }
        } else if (mode == 2) {
            if (dispcnt & (1 << 10)) { sBgEnabled[2] = 1; render_affine_bg(2, y); }
            if (dispcnt & (1 << 11)) { sBgEnabled[3] = 1; render_affine_bg(3, y); }
        } else { /* 3,4,5 bitmap */
            if (dispcnt & (1 << 10)) { sBgEnabled[2] = 1; render_bitmap_bg(mode, y); }
        }

        if (dispcnt & (1 << 12)) render_objects(y, dispcnt);
        compute_windows(y, dispcnt);
        composite(y, line);
    }
}
