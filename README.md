# Luminary

CastaliaInstitute project repository for **Luminary**, an illuminated coastal shadow box.

## Status

Early development. This repository is the source of truth for the Luminary project.

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

## Project principles

- Keep the project understandable and easy to pick up.
- Record important product and technical decisions in `docs/`.
- Prefer small, reviewable changes with clear commit messages.

## Maintainer

CastaliaInstitute
