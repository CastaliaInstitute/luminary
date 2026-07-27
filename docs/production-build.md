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
- `renders/stl/nubble-basrelief-island-texture.stl` — source-derived 0.2 mm
  rock-texture companion; import with the island base STL as one print.
- `renders/stl/nubble-basrelief-breaker.stl` — dark/charcoal PLA, 1.6 mm relief.
- `renders/stl/nubble-basrelief-breaker-texture.stl` — 0.2 mm texture
  companion for the breaker base STL.
- `renders/stl/nubble-basrelief-foreground.stl` — dark/charcoal PLA, 1.8 mm
  relief above its 3.2 mm placement level.
- `renders/stl/nubble-basrelief-foreground-texture.stl` — 0.2 mm texture
  companion for the foreground base STL.
- `renders/stl/nubble-display-contact-frame.stl` — thin 4 x 6 in matte-black
  PLA display-contact frame; it has no front magnets.
- `renders/stl/nubble-clear-supports.stl` — clear-PETG bridge traces and posts
  for isolated shallow layers; this is not a full transparent backing sheet.

The landscape shadow box has a 7 x 5 x 2 in outer enclosure and a hinged,
1 in deep front door. Its distressed-white wood door rails are 0.75 in wide
and frame the 5.5 x 3.5 in glass opening. A 1/8 in deep matching wood
sight-mat sits immediately behind the glass, reducing the visible LCD aperture
to 5.25 x 3.25 in so the P4 bezel and image edge remain hidden; it is wood,
not a separate cardboard/brown mat material.
Two small aged-metal butt hinges mount along the lower horizontal door/frame
seam, each with a horizontal barrel and four visible screw heads.
The P4 sits at the rear, behind the full scene background. Total printed scene
depth is 5.0 mm plus up to 1.35 mm of shallow source-derived rock texture; the
remaining box depth is open air for shadow and parallax.

The Nubble island and the 4 x 6 in contact frame sit directly against the P4
face. A 1.0 mm deep, 0.30 mm clearance rabbet in the rear of the door captures
the 0.8 mm frame flush with the lid inset; the closed door and its wood
sight-mat provide retention. Do not add front magnet pockets. This creates a
black interior reveal that hides the LCD edge and lets distant, zero-parallax
content—weather, changing sea, or seasonal background—live on the display.
Keep physically raised foreground elements separate; a display cannot place
dynamic pixels on opaque black PLA.

Mount the midground peninsula 2.5 mm forward of the LCD contact plane and the foreground
5.0 mm forward. Do not use the unused 2 in box cavity as a 35 mm parallax
gap: source registration visibly fails at normal three-quarter viewing angles.

Print `nubble-clear-supports.stl` in clear PETG at 0.12 mm layers. Its two
breaker posts and two foreground posts locate the shallow relief stack; the
0.45 mm traces begin beneath the black contact-frame margin and remain hidden
by the closed door/sight-mat.

## Rear plate and P4

The plate is 177.8 x 127 x 3 mm with a 166 x 101.6 mm P4 registration land,
four rear-facing 6.2 x 2.1 mm magnet pockets, and both side and bottom-centre
8 mm circular cable exits, each retained by a 1 mm PLA edge wall. Use M2.5
machine screws and washers through the four 6 x
5.2 mm tolerance slots. The slots intentionally allow the mount to match the
actual board-hole pattern, which must be checked against the physical P4 before
final assembly.

Follow [the P4 physical fit-check procedure](p4-fit-check.md) before printing
the structural rear plate. It gives the board-specific pass/fail criteria for
the supplied low-material test ring, M2.5 fasteners, USB-C extension routes,
and rear magnets.

Use four 6 x 2 mm disc magnets (or revise the parameters in
`cad/luminary-7in-backplate.scad` for the magnets on hand) and epoxy matching
steel squares to the back of the wood frame. Confirm magnet polarity before
gluing.

## Verification

The render in `renders/nubble-concept.png` imports the same bas-relief STLs
listed above, not substitute Blender geometry. The scene source, land-free LCD
background, and all relief masks use one 1024 x 600 canvas in
`scenes/nubble-aligned/`.

The 5.5 x 3.5 in relief field is narrower than the P4's native active area.
`scripts/compile-bas-relief.sh` centre-crops the source, LCD background, and
every mask from the identical 943 x 600 source region before mapping them into
that physical field. This is required for parallax layers to line up with the
background at any viewing angle.

`renders/nubble-concept-rear-assembly.png` is the complementary rear
verification render. It imports `luminary-7in-p4-backplate.stl` into the same
assembly, exposing the fastener slots, four magnet pockets, and both circular
cable exits.

Blender mesh audit: the P4 backplate, door reference, island, breaker, and
foreground STLs each have zero non-manifold and zero boundary edges. The P4
registration land clears the 164.28 x 99.17 mm display envelope by 0.86 mm at
each side and 1.215 mm at the top and bottom.

Before each STL release, run `scripts/compile-bas-relief.sh nubble-aligned` and
inspect `scenes/nubble-aligned/compiled/reference-alignment.png`. It overlays
the printable masks on the original source photograph: island in red, breaker
in gold, and foreground rocks in cyan. Reject the export if any contour or
scale does not match the photograph. `scripts/verify-production.sh` regenerates
and size-checks this validation artifact along with the mesh audit.

For Nubble, `scenes/nubble-aligned/breaker-source-segmentation.png` is the
reviewed source-coordinate segmentation for the continuous left-to-centre
midground peninsula, including the breaker where the wave crashes. It replaces
the earlier isolated-rock mask: the breaker is not a floating island.

`scenes/nubble-aligned/island-ai-separation.jpg` is the supplied AI separation
registered to the original photograph by lighthouse and shoreline landmarks.
The compiler preserves its detailed exterior shore contour, while filling its
white structure cutouts into the opaque black island until a separately
validated white-building print layer is introduced.

The three rock STLs include a masked 0–1.35 mm tonal bas-relief sampled from
the original photograph, not a generated replacement image. The compiler
normalizes source tonal structure to retain both broad rock faces and local
cracks, then samples on a 700 x 444 grid (0.20 x 0.20 mm per sample on the
finished field).
It converts those values into three shallow, merged mesh bands rather than a
fragile per-pixel terrain mesh, then removes features below one nozzle width.
Import each texture companion with its base STL and use a 0.12 mm layer height;
retain at least three walls. The source mask is applied before those tiles are
created, so relief cannot extend beyond an opaque rock silhouette.

The approved release must retain
`scenes/nubble-aligned/compiled/background-cropped.png` beside the final
render. It is the exact LCD asset imported by the Blender file and is generated
deterministically by `scripts/compile-bas-relief.sh nubble-aligned`.
