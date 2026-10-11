#ifndef GBA_MEMORY_H
#define GBA_MEMORY_H

#ifdef TARGET_WEB
/* Port: the GBA address space is relocated into host-allocated buffers (defined in
 * src/web/gba_hle.c). The BASE macros point at those buffers, so every VRAM/OAM/PAL/
 * REG/EWRAM/IWRAM access in the game routes through them with no code changes. */
extern unsigned char gGbaEwram[], gGbaIwram[], gGbaIo[], gGbaPalram[],
                     gGbaVram[], gGbaOam[], gGbaRom[], gGbaSram[];
#define EWRAM_BASE  ((void *)gGbaEwram)
#define IWRAM_BASE  ((void *)gGbaIwram)
#define REG_BASE    ((void *)gGbaIo)
#define PALRAM_BASE ((void *)gGbaPalram)
#define PALRAM_OBJ  ((void *)(gGbaPalram + 0x200))
#define VRAM_BASE   ((void *)gGbaVram)
#define VRAM_OBJ    ((void *)(gGbaVram + 0x10000))
#define OAM_BASE    ((void *)gGbaOam)
#define ROM_BASE    ((void *)gGbaRom)
#define SRAM_BASE   ((void *)gGbaSram)
#else
#define EWRAM_BASE (void *)0x02000000
#define IWRAM_BASE (void *)0x03000000
#define REG_BASE (void *)0x04000000
#define PALRAM_BASE (void *)0x05000000
#define PALRAM_OBJ (void *)0x05000200
#define VRAM_BASE (void *)0x06000000
#define VRAM_OBJ (void *)0x06010000
#define OAM_BASE (void *)0x07000000
#define ROM_BASE (void *)0x08000000
#define SRAM_BASE (void *)0x0e000000
#endif

#define EWRAM_SIZE (256 * 0x400)
#define IWRAM_SIZE (32 * 0x400)
#define PAL_ROW (16)
#define PAL_ROW_SIZE (PAL_ROW * 2)
#define COLORS_IN_PAL (16 * PAL_ROW)
#define PAL_SIZE (COLORS_IN_PAL * 2)
#define PALRAM_SIZE (PAL_SIZE * 2)
#define VRAM_SIZE (96 * 0x400)
#define VRAM_BG_SIZE (0x10000)
#define VRAM_OBJ_SIZE (0x8000)
#define OAM_SIZE 0x400
#define ROM_SIZE (32 * 0x100000)
#define SRAM_SIZE (32 * 0x400)

#endif /* GBA_MEMORY_H */
