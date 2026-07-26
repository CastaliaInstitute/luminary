# Bas-relief scene pipeline

Luminary scenes are compiled from an image set, not manually redrawn. The output is a
source-faithful 2.5D relief in front of a full-screen P4 background.

## Inputs per scene

```text
scenes/<name>/source.png          # original reference, 1024 x 600 master canvas
scenes/<name>/background.png      # same canvas, land/foreground removed
scenes/<name>/island.png          # distant island/lighthouse opaque mask
scenes/<name>/breaker.png         # middle breaking-wave rock opaque mask
scenes/<name>/foreground.png      # nearest rocks opaque mask
```

Every input must share pixel coordinates and be exactly 1024 x 600. AI is used only to make `background.png`
and the source-aligned masks; the deterministic compiler produces `depth.png`. Their outputs are reviewed over the source before
compilation. Do not let a 3D generator invent coastline geometry.

## Deterministic compile

1. Validate every input is 1024 x 600 (the confirmed 7 in P4 panel resolution).
2. Threshold/trace opaque masks into SVG outlines.
3. Generate a review depth map: island 1.6 mm, breaker 3.2 mm, foreground 5.0 mm.
4. Preserve image-space coordinates exactly; do not trim, crop, or recenter a layer.
5. Generate separate STLs: dark relief layers and a white architecture insert.
6. Render against the 1024 x 600 P4 background and inspect at straight-on plus
   a 5–8 degree off-axis angle.

## Design limits

- Keep total relief depth at or below 8 mm.
- Distant land: 0.8–2 mm; foreground: 3–5 mm; reserve 5–8 mm for only the nearest rocks.
- The P4 stays at the rear of the 2 in box; parallax comes from relief depth, not moving
  or cropping the display.
- A lighthouse is a source-matched tapered bas-relief with a small clear lantern inset,
  never a generic freestanding object.

## Human checkpoint

Before STL export, overlay each mask and depth band at 50% opacity on `source.png`.
Approve shoreline, lighthouse, breaker, and foreground alignment. This one review step is
what keeps scenes recognizable across different photographs.
