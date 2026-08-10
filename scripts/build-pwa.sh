#!/usr/bin/env bash
# Build the Luminary PWA into site/app/: compile the C core to WebAssembly and
# copy the scene assets. GitHub Pages serves site/ verbatim (no build step), so
# the wasm and assets are committed. Requires Emscripten (emcc).
#
#   scripts/build-pwa.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/android/luminary-viewer/app/src/main/cpp"
OCEAN="$ROOT/tools/ocean-sim"
APP="$ROOT/site/app"
ASSETS_SRC="$ROOT/android/luminary-viewer/app/src/main/assets"
mkdir -p "$APP/assets"

echo "[1/2] emcc -> wasm"
emcc -O3 -DOCEAN_NX=384u -DOCEAN_NY=384u \
    -I "$CORE" -I "$OCEAN" \
    "$ROOT/tools/wasm/luminary_wasm.c" \
    "$CORE/luminary_render_core.c" \
    "$OCEAN/ocean_sim.c" \
    -sMODULARIZE=1 -sEXPORT_NAME=LuminaryModule \
    -sENVIRONMENT=web \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 \
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,HEAPU32 \
    -sEXPORTED_FUNCTIONS=_malloc,_free,_lum_wasm_init,_lum_wasm_width,_lum_wasm_height,_lum_wasm_framebuffer,_lum_wasm_tick,_lum_wasm_render,_lum_wasm_set_conditions,_lum_wasm_set_clouds,_lum_wasm_cloud_bytes \
    -o "$APP/luminary_core.js"

echo "[2/2] copy scene assets"
for f in nubble_runtime_base.jpg nubble_runtime_water_mask.bin \
         nubble_runtime_land_mask.bin nubble_runtime_shore_distance.bin \
         nubble_runtime_ocean_map.bin nubble_runtime_ocean_depth.bin \
         nubble_runtime_cloud_low.bin nubble_runtime_cloud_mid.bin \
         nubble_runtime_cloud_high.bin; do
    cp "$ASSETS_SRC/$f" "$APP/assets/$f"
done

echo "done -> $APP"
ls -la "$APP" "$APP/assets" | awk '{print $5, $9}'
