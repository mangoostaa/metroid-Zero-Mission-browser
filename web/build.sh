#!/usr/bin/env bash
# Portable compile + link of the MZM web port. Run with the Emscripten SDK active
# (`source /path/to/emsdk/emsdk_env.sh`) from anywhere; paths resolve to the repo root.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
BUILD="$ROOT/web/build"
mkdir -p "$BUILD"
: > "$BUILD/objs.rsp"; : > "$BUILD/compile.err"
CF="-I include -DREGION_US -DMODERN -DTARGET_WEB -Wno-everything -O0"
ok=0; fail=0
while IFS= read -r f; do
  o="$BUILD/$(echo "$f" | sed 's#[/.]#_#g').o"
  if emcc -c $CF "$f" -o "$o" 2>>"$BUILD/compile.err"; then
    # emcc does NOT translate MSYS /c/ paths inside a response file -> use a native path.
    printf '%s\n' "$(cygpath -m "$o" 2>/dev/null || echo "$o")" >> "$BUILD/objs.rsp"
    ok=$((ok + 1))
  else
    fail=$((fail + 1)); echo "CFAIL $f" >> "$BUILD/compile.err"
  fi
done < <(find src -name '*.c' | sort)
echo "compiled: $ok ok, $fail fail"
emcc @"$BUILD/objs.rsp" -sASYNCIFY -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=0 \
     -Wl,--error-limit=0 -o "$BUILD/mzm.js"
echo "link exit $? -> $BUILD/mzm.js + mzm.wasm"
