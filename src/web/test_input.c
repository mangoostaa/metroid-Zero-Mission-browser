/* test_input.c - headless test for gba_set_keys (active-low REG_KEY_INPUT).
 *
 * Guarded by MZM_TEST (see test_audio.c for the rationale). Standalone build:
 *   emcc -I include -DMODERN -DTARGET_WEB -DMZM_TEST -Wno-everything -O0 \
 *        src/web/input.c src/web/test_input.c -o web/build/test_input.js
 *   node web/build/test_input.js
 */
#ifdef MZM_TEST

#include <stdint.h>
#include <stdio.h>

#include "gba/keys.h" /* KEY_* masks (pulls gba/memory.h -> needs TARGET_WEB externs) */

/* Standalone backing for the I/O space (input.c writes gGbaIo + 0x130). */
unsigned char gGbaIo[0x400];
/* gba/memory.h (via keys.h) declares the full relocated address space; define
 * the rest so the standalone link resolves even though input.c uses only I/O. */
unsigned char gGbaEwram[1], gGbaIwram[1], gGbaPalram[1], gGbaVram[1], gGbaOam[1], gGbaRom[1], gGbaSram[1];

void gba_set_keys(uint16_t keyinput);

static int g_fail = 0;
static uint16_t key_reg(void)
{
    return (uint16_t)(gGbaIo[0x130] | (gGbaIo[0x131] << 8));
}
static void expect(const char *name, uint16_t got, uint16_t want)
{
    if (got == want) { printf("  PASS: %s = 0x%04X\n", name, got); }
    else { printf("  FAIL: %s = 0x%04X (want 0x%04X)\n", name, got, want); g_fail = 1; }
}

int main(void)
{
    printf("[input] gba_set_keys stores active-low LE u16 at gGbaIo+0x130\n");

    gba_set_keys(KEY_MASK);                 /* 0x03FF: all released */
    expect("all released", key_reg(), 0x03FF);
    /* byte-order check: low byte then high byte */
    if (gGbaIo[0x130] == 0xFF && gGbaIo[0x131] == 0x03)
        printf("  PASS: little-endian byte order (FF,03)\n");
    else { printf("  FAIL: byte order %02X,%02X\n", gGbaIo[0x130], gGbaIo[0x131]); g_fail = 1; }

    gba_set_keys(KEY_MASK & ~KEY_A);        /* A pressed -> bit0 clear */
    expect("A pressed", key_reg(), 0x03FE);

    gba_set_keys(KEY_MASK & ~KEY_L);        /* L pressed -> bit9 clear */
    expect("L pressed", key_reg(), 0x03FF & ~(1 << 9));

    gba_set_keys(KEY_MASK & ~(KEY_A | KEY_RIGHT | KEY_START)); /* combo */
    expect("A+RIGHT+START", key_reg(), (uint16_t)(0x03FF & ~(KEY_A | KEY_RIGHT | KEY_START)));

    gba_set_keys(0x0000);                   /* every bit low = all pressed */
    expect("all pressed", key_reg(), 0x0000);

    printf(g_fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return g_fail;
}

#endif /* MZM_TEST */
