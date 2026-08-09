#!/usr/bin/env bash
# End-to-end tiling/seam validation for the Luminary Android renderer.
#
# Renders one native 2048x1200 frame headlessly from the SHIPPED assets (the
# exact render_core + ocean_sim the app compiles), then scans the sky and sea
# for unnatural straight seams and periodic tiling. Native output is used on
# purpose: a device screenshot adds letterbox bars, a status watermark and a
# non-integer panel-downscale moire, none of which are renderer bugs.
#
#   scripts/validate-render.sh            # build, render, validate
#   scripts/validate-render.sh --debug    # also write a seam-overlay PNG
#
# Exit code is non-zero if either region fails, so CI or a pre-deploy hook can
# gate on it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/android/luminary-viewer/app/src/main/cpp"
ASSETS="$ROOT/android/luminary-viewer/app/src/main/assets"
OUT="${TMPDIR:-/tmp}/luminary-validate"
mkdir -p "$OUT"

echo "[1/3] decode base photograph -> raw RGB"
python3 - "$ASSETS/nubble_runtime_base.jpg" "$OUT/base.rgb" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
assert im.size == (2048, 1200), im.size
open(sys.argv[2], "wb").write(im.tobytes())
PY

echo "[2/3] build host renderer + render one frame"
clang -O2 -DOCEAN_NX=384u -DOCEAN_NY=384u \
    -I "$CORE" -I "$ROOT/tools/ocean-sim" \
    "$ROOT/tools/host-render/render_frame.c" \
    "$CORE/luminary_render_core.c" \
    "$ROOT/tools/ocean-sim/ocean_sim.c" \
    -lm -lpthread -o "$OUT/render_frame"
"$OUT/render_frame" "$ASSETS" "$OUT/base.rgb" "$OUT/frame.ppm"

echo "[3/3] scan sky and sea for seams / tiling"
if [ "${1:-}" = "--debug" ]; then
    python3 "$ROOT/scripts/validate-tiling-seams.py" "$OUT/frame.ppm" \
        --water-mask "$ASSETS/nubble_runtime_water_mask.bin" \
        --land-mask "$ASSETS/nubble_runtime_land_mask.bin" \
        --debug "$OUT/seams-overlay.png"
else
    python3 "$ROOT/scripts/validate-tiling-seams.py" "$OUT/frame.ppm" \
        --water-mask "$ASSETS/nubble_runtime_water_mask.bin" \
        --land-mask "$ASSETS/nubble_runtime_land_mask.bin"
fi
