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
| Hidden rear ribs | ~2 mm deep | Design brief |
| Magnet reference | 3 × 1 mm neodymium | Design brief |
| Silhouette-to-display depth | 35 mm starting target | Design allocation, adjustable |
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

The 2 in outer depth is now the controlling dimension. The model uses a 35 mm
silhouette-to-screen depth, preserving roughly 15 mm for the display carrier, P4 PCB,
rear plate, and clearance. This is a fit allocation, not proof of fit; confirm the tallest
P4-side component before printing.

## Usage

Open `cad/lightbox.scad` in OpenSCAD and set `part` to `assembly`, `silhouette`, `structures`,
`glass_hardware`, `backplate`, `carrier`, `display`, or `pcb`. Render with F6, then export the selected part as STL. The geometry is
centered on the origin so it can be aligned with a future SVG-derived landscape silhouette.

## Silhouette composition

The added SVG is a first-pass silhouette interpretation of the supplied coastal photograph,
not a pixel trace. It retains the low island and larger foreground rocks, while the separate
structures layer retains the lighthouse and cottages. Sky, water, and surf are intentionally
omitted so the display can provide those elements and their animation.

## Relief strategy

The silhouette is designed around a 0.2 mm nozzle: the base scene is 4 layers (0.8 mm),
and the foreground rocks are raised another 8 layers (1.6 mm total). The lighthouse and
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

## Rear plate architecture

The rear of the object is now a printed 5 × 7 plate. A shallow 4 × 6 registration land keeps
the display close to the frame back while providing a mounting datum for the P4/display
assembly. Four rear-facing pockets accept magnets that
mate with steel plates bonded to the inside back of the shadow-box frame. The default USB-C
access is provided by two small circular pass-throughs in the printed back: one on the side
for wall mounting and one centered along the bottom for tabletop placement. The connector
stays internal and the cable routes through the selected hole. Diameter and edge offsets are
parameterized for the actual cable or grommet.

The removable silhouette is retained separately by four 3 × 1 mm magnets in its corner
pockets. The 0.8 mm mounting frame and the 8 × 8 × 2 mm corner pods are hidden beneath
an 8 mm glass-mounting rabbet, leaving only the image-derived scene visible in the clear
window. Each magnet aligns to a 1/4 × 1/4 inch steel square bonded to the inside face of
the glass door. `glass_hardware` renders those steel squares as a fit-check overlay; verify
final adhesive thickness and corner offsets on the actual glass before printing.
