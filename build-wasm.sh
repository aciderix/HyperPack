#!/bin/bash
#
# Build HyperPack WASM module
# Requires: Emscripten SDK (emcc in PATH)
#
# Output: hyperpack-web/public/hyperpack.js + hyperpack.wasm
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/src/hyperpack_wasm.c"
OUT_DIR="$SCRIPT_DIR/hyperpack-web/public"

mkdir -p "$OUT_DIR"

echo "[WASM] Building HyperPack WASM module..."

emcc "$SRC" \
    -O3 \
    -DHYPERPACK_WASM \
    -s MODULARIZE=1 \
    -s EXPORT_NAME='createHyperPack' \
    -s FORCE_FILESYSTEM=1 \
    -s "EXPORTED_FUNCTIONS=['_hp_compress','_hp_decompress','_hp_archive_compress','_hp_archive_decompress','_hp_archive_list','_hp_detect_format','_hp_num_strategies','_hp_strategy_name','_malloc','_free']" \
    -s "EXPORTED_RUNTIME_METHODS=['FS','cwrap','ccall','stringToNewUTF8','UTF8ToString']" \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=268435456 \
    -s MAXIMUM_MEMORY=2147483648 \
    -s STACK_SIZE=8388608 \
    -s NO_EXIT_RUNTIME=1 \
    -s INVOKE_RUN=0 \
    -lm \
    -s USE_ZLIB=1 \
    -o "$OUT_DIR/hyperpack.js"

echo "[WASM] Output:"
ls -lh "$OUT_DIR/hyperpack.js" "$OUT_DIR/hyperpack.wasm"
echo "[WASM] Done! Files ready in $OUT_DIR"
