# Lightbox CAD reference notes

## Source references

- `references/Living_Landscape_Shadow_Box_Design_v0.1.pdf`
- `references/Photo 1.jpg`
- `references/Photo 2.jpg`
- `references/Pasted Image 1.jpg`
- `references/Distressed White Frame.jpg`
- `assets/living-landscape-silhouette.svg`
- `assets/living-landscape-foreground.svg`
- `assets/living-landscape-structures.svg`

## Dimensions captured

| Feature | Value | Confidence |
| --- | ---: | --- |
| Nominal shadow-box exterior | 7 × 5 × 2 in (177.8 × 127.0 × 50.8 mm) | Design brief + measured depth |
| Nominal interior / silhouette panel | 6 × 4 in (152.4 × 101.6 mm) | Design brief |
| Waveshare display outer envelope | 126.90 × 70.70 mm | Product reference image |
| Display active area | 110.32 × 62.28 mm | Product reference image |
| Waveshare PCB envelope | 118.50 × 64.50 mm | Product reference image |
| Silhouette mounting frame | 4 × 6 in, 0.8 mm face, hidden under 8 mm glass rabbet | Design brief + mechanical revision |
| Inner mounting-frame edge | 2 mm tapered feather | Mechanical revision |
| Hidden rear ribs | ~2 mm deep | Design brief |
| Magnet reference | 3 × 1 mm neodymium | Design brief |
| Silhouette-to-display depth | 20 mm target | Reduces off-axis parallax while retaining shadow depth |
| Clear glass / mat opening | 114.3 × 88.9 mm (4.5 × 3.5 in) | Actual visible front aperture |
| Printed rear plate | 177.8 × 127.0 mm (5 × 7 in) | New mechanical requirement |
| Shallow rear registration land | 152.4 × 101.6 mm (4 × 6 in) | New mechanical requirement |
| USB-C cable pass-throughs | 8 mm circular default, side + bottom-center | New mechanical requirement |
| Silhouette magnet reference | 3 × 1 mm magnet in 8 × 8 × 2 mm hidden corner pod, 4 corners | Design brief + mechanical revision |
| Glass steel squares | 1/4 × 1/4 in, 4 corners | New mechanical requirement |

## Modeling assumptions

The OpenSCAD model is a fit-check starter, not a release-ready manufacturing model. The
following need physical verification before printing a final carrier or enclosure:

- actual frame inner opening, rabbet, glass thickness, and usable depth;
- display-to-frame registration and cable exit locations;
- PCB mounting-hole coordinates and connector keep-outs;
- magnet polarity, steel-square placement, and pocket tolerances;
- whether the 4 × 6 panel is centered or offset within the frame.
- exact cable diameter, grommet choice, and cable bend radius;
- exact P4 PCB hole coordinates and connector keep-outs;
- steel plate thickness, position, and spacing relative to the silhouette magnet pockets.

The 2 in outer depth is now the controlling dimension. The model uses a 20 mm
silhouette-to-screen depth, preserving roughly 30 mm for the display carrier, P4 PCB,
rear plate, and clearance. This reduces the background's apparent shift when viewed at a
slight angle. This is a fit allocation, not proof of fit; confirm the tallest P4-side
component before printing.

## Usage

Open `cad/lightbox.scad` in OpenSCAD and set `part` to `assembly`, `silhouette`, `structures`,
`glass_hardware`, `backplate`, `carrier`, `display`, or `pcb`. Render with F6, then export the selected part as STL. The geometry is
centered on the origin so it can be aligned with a future SVG-derived landscape silhouette.

## Silhouette composition

`assets/lighthouse-silhouette-source-v3.png` is the latest AI-assisted, source-preserving
tracing reference. `assets/living-landscape-photo-trace.svg` is its Potrace-derived black
relief used by the CAD: compact island, separated foreground boulders, and modest lighthouse
scale rather than the earlier abstract trace. Sky, water, and surf are intentionally omitted
so the display can provide those elements and their animation.

## Relief strategy

The silhouette is designed around a 0.2 mm nozzle: the source-traced black scene is 4 layers
(0.8 mm) and bonds to the transparent carrier. The lighthouse and cottage insert is a
separate white-print part at 6 layers (1.2 mm). The lantern center is intentionally open,
allowing the display image to show through it.

## Render handoff

The Blender scene generator is `blender/luminary_scene.py`. From the repository root:

```sh
/Applications/Blender.app/Contents/MacOS/Blender --background --python blender/luminary_scene.py
```

This writes the editable scene and the current concept render to `renders/`. The render omits
the real glass door so the display and relief remain legible; the physical assembly still
reserves the glass-door layer.

Current fit-check exports:

- `renders/stl/luminary-backplate.stl` — printed 5 × 7 rear plate
- `renders/stl/luminary-display-carrier.stl` — P4 display carrier
- `renders/stl/luminary-silhouette-carrier.stl` — clear hidden ring, magnet pods, and support traces
- `renders/stl/luminary-silhouette.stl` — black source-derived island and foreground rocks
- `renders/stl/luminary-structures.stl` — white lighthouse and building insert
- `renders/stl/p4-reference-model.stl` — simplified, dimensioned P4 display/PCB fit-check model
- `renders/stl/p4-5_5x3_5-aspect-mask.stl` — two thin black LCD-mask rails, reducing the visible scene to 97.83 × 62.28 mm

All five were exported from `cad/lightbox.scad` with OpenSCAD and checked as non-empty
ASCII STL meshes. The silhouette carrier is deliberately a ring-and-trace design; it is
not a full clear 4 × 6 panel.
- `renders/luminary-display.stl`

## Rear plate architecture

The rear of the object is a **matte-black PLA printed 5 × 7 plate**, not a separate wood
backing. A shallow 4 × 6 registration land provides the mounting datum for the P4/display
assembly at the rear of the 2 in box. Four rear-facing pockets accept magnets that
mate with steel plates bonded to the inside back of the shadow-box frame. The default USB-C
access is provided by two small circular pass-throughs in the printed back: one on the side
for wall mounting and one centered along the bottom for tabletop placement. The connector
stays internal and the cable routes through the selected hole. Diameter and edge offsets are
parameterized for the actual cable or grommet.

The removable silhouette is retained separately by four 3 × 1 mm magnets in its corner
pockets. Its clear carrier is a 0.4 mm (two-layer, 0.2 mm nozzle) perimeter ring hidden
under the 8 mm glass-mounting rabbet, plus two 0.6 mm clear support traces behind isolated
foreground boulders—not a full 4 × 6 sheet. The source-traced island reaches the side
margins directly. Clear PLA is acceptable for these traces; clear PETG generally finishes
more transparent. Each magnet aligns to a 1/4 × 1/4 inch steel square bonded to the inside face of
the glass door. `glass_hardware` renders those steel squares as a fit-check overlay; verify
final adhesive thickness and corner offsets on the actual glass before printing.
