#!/usr/bin/env bash
set -euo pipefail

# Compile aligned AI-separated scene inputs into a normalized 1024x600 depth map
# and trace-ready masks. Usage: scripts/compile-bas-relief.sh <scene-name>
scene="${1:?usage: scripts/compile-bas-relief.sh <scene-name>}"
root="scenes/$scene"
out="$root/compiled"
mkdir -p "$out"

source_dimensions="$(identify -format '%w %h' "$root/source.png")"
if [[ "$source_dimensions" != "1024 600" ]]; then
  echo "error: $root/source.png is $source_dimensions; expected canonical 1024 600" >&2
  exit 2
fi

for layer in island breaker foreground; do
  input="$root/$layer.png"
  dimensions="$(identify -format '%w %h' "$input")"
  if [[ "$dimensions" != "1024 600" ]]; then
    echo "error: $input is $dimensions; expected canvas-aligned 1024 600" >&2
    exit 2
  fi
  # Never trim, crop, or recenter a supplied mask: its pixel coordinates are geometry.
  magick "$input" -colorspace Gray -threshold 50% -negate "$out/$layer-mask.png"
  # Potrace traces black pixels. The printable foreground is white in the
  # review mask, so invert only the tracing copy. This must not alter the
  # aligned review mask or move any image-space coordinates.
  magick "$out/$layer-mask.png" -negate -threshold 50% "$out/$layer.pbm"
  potrace --svg -t 3 --opttolerance 0.4 -o "$out/$layer.svg" "$out/$layer.pbm"
done

# Print depths: distant island 1.6 mm, breaker 3.2 mm, foreground 5 mm.
magick -size 1024x600 xc:black \
  \( "$out/island-mask.png" -fill 'gray(82)' -opaque white \) -compose Lighten -composite \
  \( "$out/breaker-mask.png" -fill 'gray(164)' -opaque white \) -compose Lighten -composite \
  \( "$out/foreground-mask.png" -fill white -opaque white \) -compose Lighten -composite \
  "$out/depth.png"

background_dimensions="$(identify -format '%w %h' "$root/background.png")"
if [[ "$background_dimensions" != "1024 600" ]]; then
  echo "error: $root/background.png is $background_dimensions; expected 1024 600" >&2
  exit 2
fi

magick "$root/background.png" "$out/depth.png" -compose Blend -define compose:args=65,35 \
  -composite "$out/alignment-preview.png"

identify "$root/background.png" "$out/depth.png" "$out/alignment-preview.png"
