/* bios_decompress.c (A3) — real GBA BIOS decompression, replacing the gba_hle.c
 * stubs. LZ77 (BIOS SWI 0x11/0x12) unpacks most of the game's graphics into
 * VRAM/WRAM. Standard GBA LZ77 format: 4-byte header [type=0x10][size:24 LE],
 * then flag-byte groups of 8 tokens (MSB first): bit 0 = literal byte,
 * bit 1 = backref (len = (b1>>4)+3, disp = ((b1&0xF)<<8 | b2)+1). */
#include <stdint.h>

static void lz77_uncomp(const uint8_t *src, uint8_t *dst)
{
    uint32_t size = (uint32_t)src[1] | ((uint32_t)src[2] << 8) | ((uint32_t)src[3] << 16);
    src += 4;
    uint8_t *out = dst, *end = dst + size;
    while (out < end) {
        uint8_t flags = *src++;
        for (int i = 0; i < 8 && out < end; i++, flags <<= 1) {
            if (flags & 0x80) {
                uint8_t b1 = *src++, b2 = *src++;
                int len = (b1 >> 4) + 3;
                int disp = (((b1 & 0x0F) << 8) | b2) + 1;
                const uint8_t *cp = out - disp;
                while (len-- && out < end) *out++ = *cp++;
            } else {
                *out++ = *src++;
            }
        }
    }
}

void LZ77UncompVram(const void *src, void *dst) { lz77_uncomp(src, dst); }
void LZ77UncompWram(const void *src, void *dst) { lz77_uncomp(src, dst); }
