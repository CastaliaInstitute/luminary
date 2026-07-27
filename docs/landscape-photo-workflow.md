# Landscape photo to Luminary bas-relief workflow

The photograph is the composition authority. AI may segment or fill an image,
but it must not redesign the landform, move the horizon, or change the scale
relationship between a distant island, breaker, and foreground rocks.

Luminary's production configuration is a 7 x 5 in landscape shadow box with a
Waveshare ESP32-P4-WIFI6-Touch-LCD-7B at the rear. The physical scene field is
5.5 x 3.5 in (139.7 x 88.9 mm) behind a 5.25 x 3.25 in wood sight aperture.
It uses a 1024 x 600 working canvas. The narrow sight mat deliberately hides
the LCD edge and the cropped field margin.

## 1. Normalize the source

Create `scenes/<scene>/source.png` from the best available photograph:

- landscape orientation;
- exactly 1024 x 600 pixels;
- preserve the intended island, breaker, foreground contours, and horizon;
- do not independently crop later layer inputs.

The source coordinates are geometry. Every generated mask must retain this
same 1024 x 600 canvas and pixel registration.

## 2. Create the land-free LCD background

Use image generation with the source photograph as a reference:

> Edit this supplied landscape photograph into a photorealistic background.
> Preserve its exact horizon height, clouds, atmosphere, water direction, and
> lighting. Remove all land, buildings, shoreline, rocks, foam, and people;
> naturally fill the removed areas with continuous water. Do not move or crop
> the composition, add text, or change the photographic style.

Save the result at `scenes/<scene>/background.png`, exactly 1024 x 600. This
is the only sky and water image displayed by the P4.

## 3. Segment opaque printed layers

Make three source-registered, black-on-white PNG masks, each exactly 1024 x
600, and save them next to the source:

| Layer | File | Physical placement |
| --- | --- | --- |
| Distant island/shore | `island.png` | LCD contact plane |
| Midground peninsula including the breaking-wave rock | `breaker-source-segmentation.png` | 2.5 mm forward |
| Nearest foreground rocks | `foreground.png` | 5.0 mm forward |

The source-derived midground peninsula is intentionally separate from the
distant island and foreground. It must remain connected to its source-side
field edge; do not make its breaker tip into a floating island. Exclude water
and foam. Rocks are opaque; transparent rock areas make the display show
through and invalidate the physical scene.

Use AI only as a segmentation aid. Validate each mask by compositing it over
the original source, at identical pixel dimensions. Reject a mask if it moves
shoreline edges, flattens the bottom profile, makes the foreground too large,
or invents a landform.

White building/lighthouse inserts are optional and must remain out of the print
set until a separately segmented, source-aligned architecture mask is reviewed.
Do not improvise their scale from a generative image.

## 4. Compile and inspect registration

Run:

```sh
scripts/compile-bas-relief.sh <scene>
```

The compiler centre-crops the same 943 x 600 source region from the LCD
background and every mask, then normalizes it back to 1024 x 600 for the
5.5 x 3.5 in physical field. This shared crop is essential: independent
resize/crop steps create visible parallax errors.

Inspect `scenes/<scene>/compiled/reference-alignment.png` before exporting:

- island appears red;
- breaker appears gold;
- foreground appears cyan.

All contours must track the original photograph. This review is against the
source photo, not the generated LCD background.

## 5. Build the printable bas-relief

For Nubble, export the three opaque bases from
`cad/bas_relief_scene.scad`, then create the source-tonal companion meshes:

```sh
python3 scripts/build-rock-texture-mesh.py \
  scenes/<scene>/compiled/island-texture.png \
  renders/stl/<scene>-basrelief-island-texture.stl --base-z 1.6
```

Repeat for breaker (`--base-z 3.2`) and foreground (`--base-z 5.0`). The mesh
samples 0.2 x 0.2 mm XY detail and converts source tones into a maximum 1.35
mm printable height field. Load each texture STL with its matching base STL as
one object in the slicer; print at 0.12 mm layers.

Do not use the 2 in box depth as a 20-35 mm parallax gap. The scene is a
shallow bas-relief: the contact island is at the display, breaker 2.5 mm
forward, and foreground 5 mm forward. That depth is enough to reveal form at
a slight angle while retaining source/background registration.

## 6. Render and verify

Render only imported production STLs:

```sh
LUMINARY_SCENE=nubble /Applications/Blender.app/Contents/MacOS/Blender \
  --background --python blender/luminary_scene.py
scripts/verify-production.sh
```

The verifier rebuilds the source-alignment artifact and checks every required
STL for nonmanifold and boundary edges. Reject the scene if the product render
shows a background shift at the selected three-quarter camera angle, or if the
reference-alignment overlay disagrees with the source photograph.
