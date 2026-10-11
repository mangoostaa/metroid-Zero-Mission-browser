/* save.c - SRAM/Flash persistence for the web port (TARGET_WEB).
 *
 * The game backs its battery save with gGbaSram[] (the SRAM_BASE region). MZM is
 * a 32 KB SRAM title (include/structs/save_file.h: sizeof(struct Sram) <=
 * SRAM_SIZE == 32 KB), but gGbaSram is a 64 KB buffer; we round-trip the whole
 * buffer so nothing is lost.
 *
 * gba_save_flush() persists it. In the browser it writes into an IDBFS mount and
 * calls FS.syncfs so the bytes land in IndexedDB (compiled only when
 * GBA_SAVE_USE_IDBFS is defined for the real browser build). Everywhere else
 * (native, or Emscripten with -sNODERAWFS) it writes a plain file via stdio,
 * which is what the headless round-trip test exercises.
 */
#include <stdint.h>
#include <stdio.h>

#include "gba_hle.h" /* frozen contract: extern unsigned char gGbaSram[] */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* Whole backing buffer (see gba_hle.c: gGbaSram[64*1024]). */
#define GBA_SRAM_BYTES (64 * 1024)

#ifndef GBA_SAVE_PATH
#ifdef GBA_SAVE_USE_IDBFS
#define GBA_SAVE_PATH "/mzm_save/sram.sav"
#else
#define GBA_SAVE_PATH "mzm_sram.sav"
#endif
#endif

/* Write the SRAM buffer to `path`. Returns 0 on success, -1 otherwise. */
int gba_save_flush_to_path(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(gGbaSram, 1, GBA_SRAM_BYTES, f);
    int ok = (fclose(f) == 0) && (n == GBA_SRAM_BYTES);
    return ok ? 0 : -1;
}

/* Load the SRAM buffer from `path`. Returns bytes read, or -1 if absent. */
int gba_save_load_from_path(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(gGbaSram, 1, GBA_SRAM_BYTES, f);
    fclose(f);
    return (int)n;
}

#if defined(__EMSCRIPTEN__) && defined(GBA_SAVE_USE_IDBFS)
/* Mount IDBFS once and pull any previously-persisted save into the FS. The
 * initial async load populates /mzm_save before the first flush overwrites it;
 * callers that need the loaded bytes should re-read after the mount settles. */
EM_JS(void, mzm_idbfs_mount, (void), {
    if (Module.__mzmIdbMounted) return;
    try {
        try { FS.mkdir('/mzm_save'); } catch (e) {}
        FS.mount(IDBFS, {}, '/mzm_save');
        Module.__mzmIdbMounted = 1;
        FS.syncfs(true, function (err) {});
    } catch (e) {}
});
EM_JS(void, mzm_idbfs_sync, (void), {
    try { FS.syncfs(false, function (err) {}); } catch (e) {}
});
#endif

void gba_save_flush(void)
{
#if defined(__EMSCRIPTEN__) && defined(GBA_SAVE_USE_IDBFS)
    mzm_idbfs_mount();
    gba_save_flush_to_path(GBA_SAVE_PATH);
    mzm_idbfs_sync();
#else
    gba_save_flush_to_path(GBA_SAVE_PATH);
#endif
}

/* Load persisted SRAM at boot (browser: after the IDBFS mount has synced). */
void gba_save_load(void)
{
#if defined(__EMSCRIPTEN__) && defined(GBA_SAVE_USE_IDBFS)
    mzm_idbfs_mount();
#endif
    gba_save_load_from_path(GBA_SAVE_PATH);
}
