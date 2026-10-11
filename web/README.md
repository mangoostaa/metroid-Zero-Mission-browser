# Metroid: Zero Mission — Browser (WebAssembly) Port

A WebAssembly port of *Metroid: Zero Mission* (GBA), built from the
[metroidret/mzm](https://github.com/metroidret/mzm) decompilation and compiled with
Emscripten. **Not** an emulator and **not** cloud streaming — the game's own C code runs
natively as WebAssembly (the N64Recomp / pokeemerald-wasm approach, applied to GBA).

## Status

- **M0 — compile + link: DONE.** All 654 decompilation translation units + the host backend
  compile with `-DMODERN -DTARGET_WEB` and link to `mzm.wasm` (~6.9 MB) with **0 undefined
  symbols**.
- **Not booting visibly yet.** Next up: wire the VBlank frame loop, real GBA LZ77/Huffman
  decompressors, a software PPU → `<canvas>`, audio (m4a/MP2k → Web Audio), input, and
  SRAM saves → IndexedDB.

## How it works

The game's C is left unchanged. A host backend (`src/web/gba_hle.c`) relocates the GBA
address space (VRAM / OAM / PALRAM / IO / EWRAM / IWRAM) into host-allocated buffers — the
`gba/memory.h` BASE macros point there under `TARGET_WEB` — and provides C implementations of
the BIOS syscalls that `asm/*.s` normally supplies. The game yields to the browser at its
VBlank wait points via Emscripten Asyncify.

## Build (bring your own ROM)

This repository contains **no copyrighted game data**. Supply your own legally dumped
US v1.1 ROM (`sha1 5de8536afe1f0078ee6fe1089f890e8c7aa0a6e8`) as `mzm_us_baserom.gba`, then,
with the Emscripten SDK active:

```sh
python3 tools/extractor.py   # extracts assets from YOUR rom (output is gitignored)
bash web/build.sh            # compiles + links -> web/build/mzm.{js,wasm}
```

## Credits & legal

Game logic from the [metroidret/mzm](https://github.com/metroidret/mzm) decompilation (see
`LICENSE`). *Metroid* is © Nintendo. This is an unofficial, non-commercial port, not
affiliated with or endorsed by Nintendo. Bring your own ROM.
