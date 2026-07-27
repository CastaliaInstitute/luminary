#!/usr/bin/env python3
"""Turn a source-derived grayscale rock map into a printable binary STL.

Each 700 x 444 pixel map represents the 139.7 x 88.9 mm Luminary field, so a
pixel is exactly 0.2 mm. Contiguous equal-width runs are merged vertically into
rectangular tiles before export. This keeps the source detail at nozzle scale
without a costly OpenSCAD boolean over a dense raster terrain.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image


def row_runs(values: list[bool]):
    start = None
    for index, value in enumerate(values + [False]):
        if value and start is None:
            start = index
        elif not value and start is not None:
            yield start, index
            start = None


def merged_rectangles(image: Image.Image, threshold: int):
    pixels = image.load()
    active: dict[tuple[int, int], tuple[int, int]] = {}
    rectangles: list[tuple[int, int, int, int]] = []
    for y in range(image.height):
        runs = set(row_runs([pixels[x, y] >= threshold for x in range(image.width)]))
        for run, (start_y, last_y) in list(active.items()):
            if run not in runs:
                rectangles.append((run[0], run[1], start_y, last_y + 1))
                del active[run]
        for run in runs:
            if run in active:
                active[run] = (active[run][0], y)
            else:
                active[run] = (y, y)
    for run, (start_y, last_y) in active.items():
        rectangles.append((run[0], run[1], start_y, last_y + 1))
    return rectangles


def surface_triangles(cells, pixel_x, pixel_y, base_z, step_height):
    """Emit only external faces of the three-level voxel texture volume."""
    triangles = []
    for x, y, z in cells:
        x0, x1 = -69.85 + x * pixel_x, -69.85 + (x + 1) * pixel_x
        y0, y1 = 44.45 - (y + 1) * pixel_y, 44.45 - y * pixel_y
        z0, z1 = base_z + z * step_height, base_z + (z + 1) * step_height
        faces = {
            (-1, 0, 0): ((x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)),
            (1, 0, 0): ((x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)),
            # Raster Y increases downward while the scene Y increases upward.
            (0, 1, 0): ((x0, y0, z0), (x0, y0, z1), (x1, y0, z1), (x1, y0, z0)),
            (0, -1, 0): ((x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)),
            (0, 0, -1): ((x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)),
            (0, 0, 1): ((x0, y0, z1), (x0, y1, z1), (x1, y1, z1), (x1, y0, z1)),
        }
        for direction, quad in faces.items():
            neighbor = (x + direction[0], y + direction[1], z + direction[2])
            if neighbor not in cells:
                triangles.append((quad[0], quad[1], quad[2]))
                triangles.append((quad[0], quad[2], quad[3]))
    return triangles


def bridge_diagonals(cells):
    """Turn corner-only pixel contacts into one-nozzle orthogonal bridges."""
    changed = True
    while changed:
        changed = False
        additions = set()
        for x, y, z in cells:
            for dx, dy in ((1, 1), (1, -1)):
                diagonal = (x + dx, y + dy, z)
                side_a = (x + dx, y, z)
                side_b = (x, y + dy, z)
                if diagonal in cells and side_a not in cells and side_b not in cells:
                    additions.add(side_a)
        if additions:
            cells.update(additions)
            changed = True
    return cells


def support_columns(cells):
    """Fill every lower voxel beneath a height sample.

    The threshold masks are nested in intent, but morphology can leave a
    raised voxel diagonally touching a lower contour.  A physical relief is a
    height field, not a collection of floating shells, so make every column
    explicitly solid before extracting its external surface.
    """
    supported = set()
    for x, y, z in cells:
        supported.update((x, y, lower) for lower in range(z + 1))
    return supported


def build(input_path: Path, output_path: Path, base_z: float, step_height: float):
    image = Image.open(input_path).convert("L")
    pixel_x = 139.7 / image.width
    pixel_y = 88.9 / image.height
    bands = ((45, 0.00, 0.25), (65, 0.25, 0.50), (82, 0.50, 0.75))
    cells = set()
    for level, (threshold, _, _) in enumerate(bands):
        mask_path = input_path.with_name(f"{input_path.stem}-{threshold}.png")
        mask = Image.open(mask_path).convert("L")
        if mask.size != image.size:
            raise ValueError(f"{mask_path} does not match {input_path}")
        for y in range(image.height):
            for x in range(image.width):
                # The compiler writes PBM-derived band maps with *black* as
                # the active rock detail and white as empty space.  Treating
                # white as solid creates a full opaque slab over the LCD.
                if mask.getpixel((x, y)) < 128:
                    cells.add((x, y, level))
    cells = support_columns(cells)
    cells = bridge_diagonals(cells)
    cells = support_columns(cells)
    triangles = surface_triangles(cells, pixel_x, pixel_y, base_z, step_height)
    with output_path.open("wb") as handle:
        handle.write(b"Luminary 0.2mm source-derived rock texture".ljust(80, b" "))
        handle.write(struct.pack("<I", len(triangles)))
        for triangle in triangles:
            handle.write(struct.pack("<3f", 0.0, 0.0, 1.0))
            for vertex in triangle:
                handle.write(struct.pack("<3f", *vertex))
            handle.write(struct.pack("<H", 0))
    print(f"{output_path}: triangles={len(triangles)} voxels={len(cells)}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--base-z", type=float, required=True)
    parser.add_argument("--step-height", type=float, default=0.45,
                        help="mm per tonal level (default: 0.45; three levels = 1.35 mm)")
    args = parser.parse_args()
    build(args.input, args.output, args.base_z, args.step_height)


if __name__ == "__main__":
    main()
