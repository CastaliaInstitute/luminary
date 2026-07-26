# Luminary production build

## Print set

Print these parts at 0.20 mm layer height unless a slicer-specific strength
test indicates otherwise:

- `renders/stl/luminary-7in-p4-backplate.stl` — matte-black PLA rear plate.
- `renders/stl/luminary-7in-p4-fit-check.stl` — 1 mm low-material test frame;
  verify the actual P4 perimeter and corner holes before printing the backplate.
- `renders/stl/luminary-front-door-reference.stl` — watertight 7 x 5 x 1 in
  woodworking reference for the hinged front door; do not print this wood part.
- `renders/stl/nubble-basrelief-island.stl` — black PLA, 1.6 mm relief.
- `renders/stl/nubble-basrelief-breaker.stl` — dark/charcoal PLA, 1.6 mm relief.
- `renders/stl/nubble-basrelief-foreground.stl` — dark/charcoal PLA, 1.8 mm
  relief above its 3.2 mm placement level.

The landscape shadow box has a 7 x 5 x 2 in outer enclosure and a hinged,
1 in deep front door. Its distressed-white wood door rails are 0.75 in wide
and frame the 5.5 x 3.5 in glass opening; there is no separate mat material.
Two small aged-metal butt hinges mount along the lower horizontal door/frame
seam, each with a horizontal barrel and four visible screw heads.
The P4 sits at the rear, behind the full scene background. Total printed scene
depth is 5.0 mm; the remaining box depth is open air for shadow and parallax.

## Rear plate and P4

The plate is 177.8 x 127 x 3 mm with a 166 x 101.6 mm P4 registration land,
four rear-facing 6.2 x 2.1 mm magnet pockets, and both side and bottom-centre
8 mm cable exits. Use M2.5 machine screws and washers through the four 6 x
5.2 mm tolerance slots. The slots intentionally allow the mount to match the
actual board-hole pattern, which must be checked against the physical P4 before
final assembly.

Use four 6 x 2 mm disc magnets (or revise the parameters in
`cad/luminary-7in-backplate.scad` for the magnets on hand) and epoxy matching
steel squares to the back of the wood frame. Confirm magnet polarity before
gluing.

## Verification

The render in `renders/nubble-concept.png` imports the same bas-relief STLs
listed above, not substitute Blender geometry. The scene source, land-free LCD
background, and all relief masks use one 1024 x 600 canvas in
`scenes/nubble-aligned/`.

Blender mesh audit: the P4 backplate, door reference, island, breaker, and
foreground STLs each have zero non-manifold and zero boundary edges. The P4
registration land clears the 164.28 x 99.17 mm display envelope by 0.86 mm at
each side and 1.215 mm at the top and bottom.
