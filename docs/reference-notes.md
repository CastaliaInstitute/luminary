# Lightbox CAD reference notes

## Source references

- `references/Living_Landscape_Shadow_Box_Design_v0.1.pdf`
- `references/Photo 1.jpg`
- `references/Photo 2.jpg`
- `references/Pasted Image 1.jpg`
- `assets/living-landscape-silhouette.svg`
- `assets/living-landscape-foreground.svg`
- `assets/living-landscape-structures.svg`

## Dimensions captured

| Feature | Value | Confidence |
| --- | ---: | --- |
| Nominal shadow-box exterior | 7 × 5 in (177.8 × 127.0 mm) | Design brief |
| Nominal interior / silhouette panel | 6 × 4 in (152.4 × 101.6 mm) | Design brief |
| Waveshare display outer envelope | 126.90 × 70.70 mm | Product reference image |
| Display active area | 110.32 × 62.28 mm | Product reference image |
| Waveshare PCB envelope | 118.50 × 64.50 mm | Product reference image |
| Visible silhouette face | 0.8 mm | Design brief |
| Hidden rear ribs | ~2 mm deep | Design brief |
| Magnet reference | 3 × 1 mm neodymium | Design brief |
| Silhouette-to-display air gap | 2–3 mm | Design brief |

## Modeling assumptions

The OpenSCAD model is a fit-check starter, not a release-ready manufacturing model. The
following need physical verification before printing a final carrier or enclosure:

- actual frame inner opening, rabbet, glass thickness, and usable depth;
- display-to-frame registration and cable exit locations;
- PCB mounting-hole coordinates and connector keep-outs;
- magnet polarity, steel-square placement, and pocket tolerances;
- whether the 4 × 6 panel is centered or offset within the frame.

## Usage

Open `cad/lightbox.scad` in OpenSCAD and set `part` to `assembly`, `silhouette`, `structures`,
`carrier`, `display`, or `pcb`. Render with F6, then export the selected part as STL. The geometry is
centered on the origin so it can be aligned with a future SVG-derived landscape silhouette.

## Silhouette composition

The added SVG is a first-pass interpretation of the supplied coastal photograph, not a
pixel trace. It emphasizes a low island, two cottages, the lighthouse and lantern,
separated surf bands, and larger foreground rocks. The white cutouts are intentional
openings for the animated display to show through.

## Relief strategy

The silhouette is designed around a 0.2 mm nozzle: the base scene is 4 layers (0.8 mm),
and the foreground rocks/surf are raised another 8 layers (1.6 mm total). The lighthouse and
cottage insert is a separate white-print part at 6 layers (1.2 mm). This keeps every thickness
aligned to whole nozzle-width layer increments.

## Render handoff

The Blender scene generator is `blender/luminary_scene.py`. From the repository root:

```sh
/Applications/Blender.app/Contents/MacOS/Blender --background --python blender/luminary_scene.py
```

This writes the editable scene and the current concept render to `renders/`. The render omits
the real glass door so the display and relief remain legible; the physical assembly still
reserves the glass-door layer.

Current fit-check exports:

- `renders/luminary-silhouette.stl`
- `renders/luminary-structures.stl`
- `renders/luminary-carrier.stl`
- `renders/luminary-display.stl`
