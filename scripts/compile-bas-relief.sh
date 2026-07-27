#!/usr/bin/env bash
set -euo pipefail

# Compile aligned AI-separated scene inputs into a normalized 1024x600 depth map
# and trace-ready masks. Usage: scripts/compile-bas-relief.sh <scene-name>
scene="${1:?usage: scripts/compile-bas-relief.sh <scene-name>}"
root="scenes/$scene"
out="$root/compiled"
mkdir -p "$out"

# The physical scene is 5.5 x 3.5 in (1.571:1), whereas the supplied P4
# image canvas is 1024 x 600 (1.707:1). Crop both the LCD background and all
# relief masks from the same centre 943 x 600 source region, then normalize
# the crop back to the 1024 x 600 working canvas. This preserves image-space
# registration when that canvas is mapped to the physical scene field.
scene_crop_w=943
scene_crop_x=40
scene_crop_geometry="${scene_crop_w}x600+${scene_crop_x}+0"

normalize_scene_canvas() {
  local input="$1"
  local output="$2"
  magick "$input" -crop "$scene_crop_geometry" +repage -resize 1024x600! "$output"
}

source_dimensions="$(identify -format '%w %h' "$root/source.png")"
if [[ "$source_dimensions" != "1024 600" ]]; then
  echo "error: $root/source.png is $source_dimensions; expected canonical 1024 600" >&2
  exit 2
fi
normalize_scene_canvas "$root/source.png" "$out/source-cropped.png"

# The source photograph supplies the rock micro-relief.  Keep the sampling
# A 700 x 444 canvas maps to 0.200 x 0.200 mm on the printed 5.5 x 3.5 in
# field. It preserves the intended nozzle-scale detail while the morphology
# pass below removes sub-nozzle flecks before vector tracing.
# Removing a soft blur retains local rock faces and cracks while suppressing
# the broad photographic lighting that belongs on the LCD background.
magick "$out/source-cropped.png" -colorspace Gray \
  \( +clone -blur 0x4 \) -compose Difference -composite \
  -auto-level -level 6%,88% "$out/rock-detail.png"

# The relief mesh needs broad, source-faithful faces as well as tiny edge
# detail.  This normalized tonal map becomes the 0.2 mm height field after
# masking per layer below; it does not invent terrain from a generic noise map.
magick "$out/source-cropped.png" -colorspace Gray -auto-level \
  -sigmoidal-contrast 6x52% "$out/rock-height-source.png"

for layer in island breaker foreground; do
  input="$root/$layer.png"
  segmentation_source="$root/$layer-source-segmentation.png"
  trace_source="$root/$layer-source-trace.svg"
  ai_separation="$root/$layer-ai-separation.jpg"
  if [[ -f "$segmentation_source" ]]; then
    # A source-registered semantic segmentation is authoritative. For Nubble
    # this is the continuous midground peninsula: it enters from the left
    # field edge and includes the breaker rock, while excluding water and foam.
    input="$segmentation_source"
  elif [[ -f "$ai_separation" ]]; then
    # A supplied AI separation may retain the source's detailed exterior
    # profile. Register it to the canonical source photograph via the
    # reviewed 80% scale / 59 px vertical crop, then fill internal white
    # architecture voids for the current opaque-black island layer.
    input="$out/$layer-ai-separation.png"
    magick "$ai_separation" -resize 1024x682! -crop 1024x600+0+59 +repage \
      -colorspace Gray -threshold 55% -negate -morphology Close Disk:12 -negate \
      "$input"
  elif [[ -f "$trace_source" ]]; then
    # A hand-reviewed SVG trace is authoritative when present. It preserves
    # source-image coordinates while avoiding generative re-imagining of a
    # physical silhouette. Rasterize it into the build directory only.
    input="$out/$layer-source-trace.png"
    magick -background white "$trace_source" -resize 1024x600! \
      -colorspace Gray -threshold 50% "$input"
  fi
  dimensions="$(identify -format '%w %h' "$input")"
  if [[ "$dimensions" != "1024 600" ]]; then
    echo "error: $input is $dimensions; expected canvas-aligned 1024 600" >&2
    exit 2
  fi
  normalized_input="$out/$layer-cropped.png"
  normalize_scene_canvas "$input" "$normalized_input"
  input="$normalized_input"
  # Never trim, crop, or recenter a supplied mask: its pixel coordinates are geometry.
  magick "$input" -colorspace Gray -threshold 50% -negate "$out/$layer-mask.png"
  # Potrace traces black pixels. The printable foreground is white in the
  # review mask, so invert only the tracing copy. This must not alter the
  # aligned review mask or move any image-space coordinates.
  magick "$out/$layer-mask.png" -negate -threshold 50% "$out/$layer.pbm"
  potrace --svg -t 3 --opttolerance 0.4 -o "$out/$layer.svg" "$out/$layer.pbm"
  # Black outside the mask is a true zero-height boundary for OpenSCAD's
  # surface() primitive. Each printed layer therefore captures only the
  # source rock texture belonging to that layer.
  magick "$out/rock-height-source.png" "$out/$layer-mask.png" -compose Multiply -composite \
    -resize 700x444! -colorspace Gray "$out/$layer-texture.png"
  # Convert three nested intensity bands into sparse vector contours. This
  # produces a watertight, slicer-friendly relief instead of asking OpenSCAD
  # to boolean a dense raster terrain mesh.
  for band in 45 65 82; do
    magick "$out/$layer-texture.png" -threshold "${band}%" -negate \
      -despeckle -morphology Open Disk:1 -morphology Close Disk:1 \
      "$out/$layer-texture-${band}.pbm"
    magick "$out/$layer-texture-${band}.pbm" -negate \
      "$out/$layer-texture-${band}.png"
    # Keep one nozzle-width of meaningful detail, but simplify the curve
    # language aggressively enough that the final STL remains practical.
    potrace --svg -t 12 --opttolerance 1.2 \
      -o "$out/$layer-texture-${band}.svg" "$out/$layer-texture-${band}.pbm"
  done
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
normalize_scene_canvas "$root/background.png" "$out/background-cropped.png"

magick "$out/background-cropped.png" "$out/depth.png" -compose Blend -define compose:args=65,35 \
  -composite "$out/alignment-preview.png"

# Validation is performed against the original photograph, never the generated
# background. Each layer keeps its exact 1024 x 600 source coordinates and is
# overlaid in a separate translucent color for a fast visual contour check.
make_reference_overlay() {
  local layer="$1"
  local color="$2"
  magick -size 1024x600 "xc:$color" "$out/$layer-mask.png" -alpha off \
    -compose CopyOpacity -composite \
    -channel A -evaluate multiply 0.48 +channel "$out/$layer-overlay.png"
}

make_reference_overlay island '#ff4d4d'
make_reference_overlay breaker '#ffd447'
make_reference_overlay foreground '#32d8ff'
magick "$out/source-cropped.png" \
  "$out/island-overlay.png" -compose Over -composite \
  "$out/breaker-overlay.png" -compose Over -composite \
  "$out/foreground-overlay.png" -compose Over -composite \
  "$out/reference-alignment.png"

identify "$out/background-cropped.png" "$out/depth.png" "$out/alignment-preview.png" \
  "$out/reference-alignment.png" "$out/rock-detail.png" \
  "$out/rock-height-source.png" \
  "$out/island-texture.png" "$out/breaker-texture.png" "$out/foreground-texture.png"
