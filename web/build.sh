#!/usr/bin/env bash
# Compile + link the MZM web port. Run with the Emscripten SDK active
# (`source /path/to/emsdk/emsdk_env.sh`); paths resolve to the repo root.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
BUILD="$ROOT/web/build"
mkdir -p "$BUILD"
: > "$BUILD/objs.rsp"; : > "$BUILD/compile.err"

# Prototype umbrella: every non-data header + the orphan prototypes. Force-included
# into the game TUs so the decomp's unprototyped calls get correct wasm signatures
# (otherwise each traps as a signature mismatch).
find include -name '*.h' -not -path 'include/data/*' | sed 's#^include/##' | sort \
  | awk '{print "#include \""$0"\""}' > web/prototypes.h
cat >> web/prototypes.h <<'EOF'
/* Orphan prototypes: called in the decomp with no header declaration anywhere. */
u32 GadoraHatchUpdate(u16 xPosition, u16 yPosition, u8 action);
void InGameTimerUpdate(void);
boolu32 ProcessPauseButtonPress(void);
u32 RidleyCheckGrabbing(u16 yPosition, u16 xPosition);
s32 CallGetNoteFrequency(s32, u32);
EOF

BASE="-I include -DREGION_US -DMODERN -DTARGET_WEB -Wno-everything -O0"
GAME="$BASE -include web/prototypes.h"   # decomp TUs: force-include the umbrella
ok=0; fail=0
while IFS= read -r f; do
  o="$BUILD/$(echo "$f" | sed 's#[/.]#_#g').o"
  case "$f" in
    src/web/*) cf="$BASE" ;;             # host backend is self-contained, no umbrella
    *)         cf="$GAME" ;;
  esac
  if emcc -c $cf "$f" -o "$o" 2>>"$BUILD/compile.err"; then
    # emcc does NOT translate MSYS /c/ paths inside a response file -> native path.
    printf '%s\n' "$(cygpath -m "$o" 2>/dev/null || echo "$o")" >> "$BUILD/objs.rsp"
    ok=$((ok + 1))
  else
    fail=$((fail + 1)); echo "CFAIL $f" >> "$BUILD/compile.err"
  fi
done < <(find src -name '*.c' | sort)
echo "compiled: $ok ok, $fail fail"

# Asyncify lets the game yield to the browser at its VBlank waits.
emcc @"$BUILD/objs.rsp" -sASYNCIFY -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=0 \
     -sEXPORTED_FUNCTIONS=_main,_gba_framebuffer,_gba_frame_count,_gba_js_set_keys \
     -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,ccall \
     -Wl,--error-limit=0 -o "$BUILD/mzm.js"
echo "link exit $? -> $BUILD/mzm.js + mzm.wasm"
