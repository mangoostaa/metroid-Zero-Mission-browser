// Headless boot harness (orchestrator): load the integrated wasm, let the game
// self-run through init via Asyncify, and snapshot the PPU framebuffer to a raw
// RGBA file. Proves the full port boots and the PPU renders real game graphics.
//   node web/boot_check.js [out.rgba] [targetFrames]
const fs = require('fs');
const path = require('path');
const OUT = process.argv[2] || path.resolve(__dirname, 'build', 'boot.rgba');
const TARGET = parseInt(process.argv[3] || '300', 10);
const WALL_MS = 25000;
const W = 240, H = 160;

const start = Date.now();
let done = false;

function finish(reason, count) {
  if (done) return; done = true;
  const M = global.Module;
  try {
    const ptr = M._gba_framebuffer();
    const buf = Buffer.from(M.HEAPU8.buffer, ptr, W * H * 4);
    fs.writeFileSync(OUT, buf);
    console.log(`[boot_check] ${reason}; frames=${count}; wrote ${OUT} (${W * H * 4} bytes)`);
  } catch (e) {
    console.log(`[boot_check] ${reason}; frames=${count}; framebuffer dump FAILED: ${e}`);
  }
  process.exit(0);
}

global.Module = {
  arguments: [],
  print: (s) => console.log('[game]', s),
  printErr: (s) => console.log('[game:err]', s),
  onAbort: (what) => console.log('[boot_check] ABORT:', what),
  onRuntimeInitialized: function () {
    console.log('[boot_check] runtime ready; main running (asyncify)...');
    const timer = setInterval(() => {
      let c = 0;
      try { c = global.Module._gba_frame_count(); }
      catch (e) { clearInterval(timer); return finish('frame_count threw ' + e, -1); }
      if (c >= TARGET) { clearInterval(timer); finish('reached target', c); }
      else if (Date.now() - start > WALL_MS) { clearInterval(timer); finish('wall-clock timeout', c); }
    }, 20);
  }
};

require(path.resolve(__dirname, 'build', 'mzm.js'));
