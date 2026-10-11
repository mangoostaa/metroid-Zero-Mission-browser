/* test_save.c - headless round-trip test for SRAM persistence.
 *
 * Guarded by MZM_TEST (see test_audio.c). Standalone build (needs real FS):
 *   emcc -I include -DMODERN -DTARGET_WEB -DMZM_TEST -Wno-everything -O0 \
 *        -sNODERAWFS=1 src/web/save.c src/web/test_save.c -o web/build/test_save.js
 *   node web/build/test_save.js web/build/test_sram.sav
 */
#ifdef MZM_TEST

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GBA_SRAM_BYTES (64 * 1024)

/* Standalone backing for the SRAM region that save.c reads/writes. */
unsigned char gGbaSram[GBA_SRAM_BYTES];

int gba_save_flush_to_path(const char *path);
int gba_save_load_from_path(const char *path);

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  PASS: %s\n", msg); \
    else { printf("  FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "test_sram.sav";
    printf("[save] gGbaSram round-trip via %s\n", path);

    /* deterministic pattern across the whole buffer */
    for (int i = 0; i < GBA_SRAM_BYTES; i++)
        gGbaSram[i] = (unsigned char)((i * 31 + 7) & 0xFF);
    unsigned char expect[256];
    memcpy(expect, gGbaSram, sizeof(expect));
    unsigned char tail = gGbaSram[GBA_SRAM_BYTES - 1];

    CHECK(gba_save_flush_to_path(path) == 0, "flush wrote SRAM file");

    /* wipe and reload */
    memset(gGbaSram, 0, GBA_SRAM_BYTES);
    int n = gba_save_load_from_path(path);
    printf("  loaded %d bytes\n", n);
    CHECK(n == GBA_SRAM_BYTES, "loaded full 64 KB back");
    CHECK(memcmp(gGbaSram, expect, sizeof(expect)) == 0, "first 256 bytes match");
    CHECK(gGbaSram[GBA_SRAM_BYTES - 1] == tail, "last byte matches");

    /* a byte we change must survive a flush/reload */
    gGbaSram[0x1234] = 0xAB;
    gba_save_flush_to_path(path);
    memset(gGbaSram, 0, GBA_SRAM_BYTES);
    gba_save_load_from_path(path);
    CHECK(gGbaSram[0x1234] == 0xAB, "modified byte persisted");

    CHECK(gba_save_load_from_path("definitely_absent_file.sav") == -1, "absent file returns -1");

    printf(g_fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return g_fail;
}

#endif /* MZM_TEST */
