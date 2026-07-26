# Luminary

CastaliaInstitute project repository for **Luminary**, an illuminated coastal shadow box.

## Status

Printable prototype and STL-based product render complete. This repository is the source of
truth for the Luminary project.

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
The latest concept render is `renders/luminary-concept.png`; the editable Blender scene is
`renders/luminary-concept.blend`.

For new scenes, follow the [landscape photo workflow](docs/landscape-photo-workflow.md).
It defines the AI background-fill, source-faithful silhouette, SVG, print-validation, and
off-axis display-alignment process.

## Project principles

- Keep the project understandable and easy to pick up.
- Record important product and technical decisions in `docs/`.
- Prefer small, reviewable changes with clear commit messages.

## Maintainer

CastaliaInstitute
