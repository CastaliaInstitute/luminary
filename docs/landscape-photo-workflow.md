# Landscape photo → Luminary workflow

Use this workflow for every new scene. Its rule is simple: the photograph is the
composition authority. AI may remove, reconstruct, and clarify material, but must not
redesign the landform.

## 1. Prepare one source photograph

Start with the highest-resolution image available. Save it as:

```text
assets/source/<scene>-original.jpg
```

Choose a landscape crop that has a clear horizon and a foreground/middle-ground separation.
Do not crop buildings, lighthouse tops, rock edges, or the highest wave until the final
display aspect ratio is known.

Record these three visual planes before processing:

| Plane | Becomes |
| --- | --- |
| Sky and open water | LCD background |
| Island / shore / architecture | Black relief plus optional white building insert |
| Nearest rocks | Separate raised foreground silhouette |

## 2. Make the LCD background with AI fill

Use image generation with the source photo as a reference. Generate a **land-free** image at
the LCD aspect ratio (the P4 active area is 110.32 × 62.28 mm, effectively 16:9). Preserve
the photograph's horizon height, sky color, clouds, water direction, and lighting.

Prompt template:

> Edit this supplied landscape photograph into a photorealistic 16:9 LCD background.
> Preserve the exact sky, horizon height, atmosphere, and water color/direction. Completely
> remove all land, buildings, shoreline, rocks, surf foreground, and people; naturally fill
> those areas with uninterrupted water. Leave the lower third readable behind a physical
> foreground silhouette. No text, border, or illustration styling.

Save the resulting file as:

```text
assets/display-<scene>-1280x720.png
```

Resize/crop to exactly 1280 × 720 without moving the horizon unless the CAD review demands
it. The display image should be the only source of sky and water.

## 3. Make AI tracing references for the island and foreground

Generate a second reference from the original photo for the island and architecture only.
This is **not** the final artwork; it is an aid for making a faithful vector silhouette.

Prompt template:

> Create a clean, source-faithful architectural tracing reference from this exact landscape
> photograph. Do not redesign or beautify it. Preserve the photographed landform, relative
> building sizes, horizon placement, and foreground rock contours. Remove sky, water, waves,
> and foam. Make the distant island and land solid black; make buildings pure white; keep lighthouse lantern
> glazing open/transparent. Use a pure white background, crisp untextured forms, no gradients,
> labels, or border.

Save it as:

```text
assets/<scene>-silhouette-reference.png
```

Reject a reference if it invents buildings, turns a low island into a mountain, merges open
water into land, or changes the relative lighthouse/house scale.

Generate a third reference for foreground rocks only. Exclude the distant island and preserve
the original water gaps between boulders. If the source has a wave striking a middle rock,
make that rock a separate fourth shallow layer. This prevents foreground rocks or the
breaker-rock target from being lost when AI creates the island pass.

## 4. Trace printable silhouettes, not a bitmap

Build SVGs from the reference using simple closed paths:

- `living-landscape-photo-trace.svg`: black island reference
- `living-landscape-foreground.svg`: separate black foreground-rock reference
- `living-landscape-structures.svg`: separate white structures

Keep the three rules below.

1. The black landform must have a visibly irregular top and bottom contour; avoid a long,
   uniform horizontal band.
2. The lighthouse lantern center must be an actual hole in the white STL, not a dark printed
   rectangle.
3. Isolated foreground rocks need narrow clear support traces to the rabbet ring. Do **not**
   solve this with a full clear 4 × 6 sheet.

At a 0.2 mm nozzle, use a 0.8 mm black base (four layers) and a 1.2 mm white insert (six
layers) unless a scene specifically needs a thicker foreground layer.

## 5. Fit the background to the physical viewing angle

Render the actual exported STLs, not proxy geometry. Set the LCD 20 mm behind the silhouette
as the normal Luminary starting point; this retains depth while reducing horizon parallax at
a slight viewing angle.

Check these before approval:

- Horizon remains visually continuous behind the island.
- Water is visible through every intended opening.
- The display does not appear to slide sideways relative to the silhouette at the chosen
  product-render angle.
- The white lighthouse/buildings read as distinct physical inserts.

If the background still shifts, adjust the display image's crop/horizon first; only then move
the display plane in 2–3 mm increments.

## 6. Export and verify the printable set

Export from `cad/lightbox.scad`:

```text
luminary-backplate.stl
luminary-display-carrier.stl
luminary-silhouette-carrier.stl
luminary-silhouette.stl
luminary-structures.stl
```

Every export must be non-empty and closed before it is used in Blender. The final render must
import those files directly, with the generated LCD background applied at the active display
area.
