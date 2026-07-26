# Luminary

CastaliaInstitute project repository for **Luminary**, an illuminated coastal shadow box.

## Status

Production-candidate STL package and STL-based render are ready for physical P4 fit testing.
This repository is the source of truth for the Luminary project.

## Repository layout

```text
.
├── README.md
├── blender/        # Blender scene-generation script
├── cad/            # Parametric OpenSCAD model
├── docs/          # Product, design, and technical notes
├── renders/        # Concept render and fit-check STLs
└── src/           # Implementation source
```

## Getting started

The project page is published at [luminary.castalia.institute](https://luminary.castalia.institute).

Open `cad/lightbox.scad` in OpenSCAD to inspect or export the current parametric model.
The latest production-candidate render is `renders/nubble-concept.png`; the editable Blender
scene is `renders/nubble-concept.blend`. The release print set and physical assembly notes are
in [`docs/production-build.md`](docs/production-build.md).

For new scenes, follow the [landscape photo workflow](docs/landscape-photo-workflow.md).
It defines the AI background-fill, source-faithful silhouette, SVG, print-validation, and
off-axis display-alignment process.

The first three-layer example is documented in the [Nubble source run](docs/nubble-source-run.md).
Its exported meshes are in `renders/stl/nubble-*.stl`, with the corresponding actual-STL
Blender render at `renders/nubble-concept.png`.

## Project principles

- Keep the project understandable and easy to pick up.
- Record important product and technical decisions in `docs/`.
- Prefer small, reviewable changes with clear commit messages.

## Maintainer

CastaliaInstitute
