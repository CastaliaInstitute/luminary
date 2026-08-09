#!/usr/bin/env python3
"""Build the screen-to-solver map for the Luminary sea renderer.

For every half-resolution water cell of the panel, this precomputes which
shallow-water solver cell it looks at, using the fitted sea-plane projection
(config/nubble-sea-projection.json) and the solver domain recorded alongside
the bathymetry (tools/ocean-sim/nubble_depth_meta.json).

The output is a 512 x 155 grid covering screen rows 290..599 -- one entry
per 2x2 pixel cell, matching the renderer's existing half-resolution wave
grid. Each entry is a pair of Q8.8 solver-grid coordinates (alongshore x,
offshore y) so the renderer can sample the solver fields bilinearly:
nearest-cell sampling draws the 2 m world grid as hard-edged blocks, which
read as a checkerboard in the near field where one solver cell spans tens of
screen pixels. A y coordinate of 0xFFFF marks cells the solver cannot serve:

  * water west of the solver domain (the near channel; the domain starts
    60 m east of the light);
  * water projecting far beyond the domain (the far field near the horizon
    projects kilometres out; the domain is 256 m deep);
  * sky rows and anything behind the camera.

The renderer keeps the analytic sine-phase path for those cells, so the
solver drives the surf zone it actually models and hands off smoothly
everywhere else. Projections within CLAMP_M of the domain edge are clamped
onto it rather than dropped, so the handoff border is not a hard visual seam
at the domain boundary itself.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PROJECTION = ROOT / "config/nubble-sea-projection.json"
DEPTH_META = ROOT / "tools/ocean-sim/nubble_depth_meta.json"

MASK = ROOT / "firmware/luminary-background-viewer/assets/runtime/nubble_runtime_water_mask.bin"
OUTPUT = ROOT / "firmware/luminary-background-viewer/assets/runtime/nubble_runtime_ocean_map.bin"

WIDTH, HEIGHT, HORIZON = 1024, 600, 291
CX, CY = (WIDTH - 1) / 2.0, (HEIGHT - 1) / 2.0
MAP_W, MAP_ROW0 = 512, 145          # half-res columns; first half-res row (y=290)
MAP_H = 300 - MAP_ROW0              # through y=599
SENTINEL = 0xFFFF
CLAMP_M = 24.0                      # snap near-misses onto the domain edge


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", type=Path,
                        default=ROOT / "scenes/nubble-aligned/compiled/ocean-map-preview.png")
    parser.add_argument("--scale", type=int, default=1,
                        help="integer screen-resolution multiplier; the fitted "
                             "camera model is continuous, so the map regenerates "
                             "at any scale rather than being resampled")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()

    global WIDTH, HEIGHT, HORIZON, CX, CY, MAP_W, MAP_ROW0, MAP_H
    WIDTH *= args.scale
    HEIGHT *= args.scale
    HORIZON *= args.scale
    CX, CY = (WIDTH - 1) / 2.0, (HEIGHT - 1) / 2.0
    MAP_W = WIDTH // 2
    MAP_ROW0 = (HORIZON - 1) // 2
    MAP_H = HEIGHT // 2 - MAP_ROW0

    projection = json.loads(PROJECTION.read_text())
    camera = projection["camera"]
    meta = json.loads(DEPTH_META.read_text())
    nx, ny = meta["grid"]
    dx_m = meta["dx_m"]
    offshore0, alongshore0 = meta["offsets_m"]
    depth = np.fromfile(ROOT / f"tools/ocean-sim/nubble_depth_{nx}x{ny}.bin",
                        dtype=np.uint8).reshape(ny, nx)

    cam_e, cam_n, cam_h = camera["east_m"], camera["north_m"], camera["height_m"]
    # Intrinsics scale linearly with resolution; the fit was at 1x.
    focal, yaw = camera["focal_px"] * args.scale, math.radians(camera["yaw_deg"])
    pitch = math.atan2(CY - HORIZON, focal)

    fwd = np.array([math.sin(yaw) * math.cos(pitch),
                    math.cos(yaw) * math.cos(pitch),
                    -math.sin(pitch)])
    right = np.array([math.cos(yaw), -math.sin(yaw), 0.0])
    down = np.cross(fwd, right)

    # Half-res cell centres in panel pixels.
    px = np.arange(MAP_W) * 2.0 + 0.5
    py = (np.arange(MAP_H) + MAP_ROW0) * 2.0 + 0.5
    u = (px[None, :] - CX) / focal
    v = (py[:, None] - CY) / focal
    ray_e = fwd[0] + u * right[0] + v * down[0]
    ray_n = fwd[1] + u * right[1] + v * down[1]
    ray_z = fwd[2] + u * right[2] + v * down[2]
    with np.errstate(divide="ignore", invalid="ignore"):
        t = np.where(ray_z < -1e-9, cam_h / -ray_z, np.nan)
    east = cam_e + t * ray_e
    north = cam_n + t * ray_n

    # World -> solver grid: x is alongshore (north), y is offshore (east).
    gx = (north - alongshore0) / dx_m
    gy = (east - offshore0) / dx_m
    clamp_cells = CLAMP_M / dx_m
    # The outermost seaward rows hold the Mur boundary and the TF/SF
    # scattered-field strip; their surface carries boundary artifacts, not
    # sea. Cells looking there keep the analytic path.
    seaward_limit = ny - 20
    inside = (np.isfinite(gx) &
              (gx > -clamp_cells) & (gx < nx - 1 + clamp_cells) &
              (gy > -clamp_cells) & (gy < seaward_limit))
    ix = np.clip(np.rint(gx), 0, nx - 1).astype(np.int64)
    iy = np.clip(np.rint(gy), 0, ny - 1).astype(np.int64)

    # Q8.8 coordinates, clamped so a bilinear +1 neighbour stays in range.
    gx_q = np.clip(np.nan_to_num(gx, nan=0.0) * 256.0, 0, (nx - 1) * 256 - 1)
    gy_q = np.clip(np.nan_to_num(gy, nan=0.0) * 256.0, 0, (ny - 1) * 256 - 1)
    gy_q = np.minimum(gy_q, (seaward_limit - 1) * 256 - 1)
    grid = np.zeros((MAP_H, MAP_W, 2), dtype=np.uint16)
    grid[..., 0] = np.where(inside, gx_q.astype(np.uint16), 0)
    grid[..., 1] = np.where(inside, gy_q.astype(np.uint16), SENTINEL)

    # A cell whose nearest solver cell is land gives the renderer nothing to
    # shade; hand those to the analytic path too. (They are almost all
    # covered by the printed relief anyway.)
    mapped = grid[..., 1] != SENTINEL
    cell = (np.clip(np.rint(gy), 0, ny - 1).astype(np.int64) * nx +
            np.clip(np.rint(gx), 0, nx - 1).astype(np.int64))
    on_land = np.zeros_like(mapped)
    on_land[mapped] = depth.reshape(-1)[cell[mapped]] == 0
    grid[..., 1][on_land] = SENTINEL

    grid.tofile(args.output)
    print(f"wrote {args.output} ({grid.nbytes} bytes, {MAP_W}x{MAP_H} Q8.8 pairs)")

    # The coverage report reads the 1x runtime mask regardless of --scale
    # (only the map's sampling density changes with scale, not which screen
    # cells are water). Sample it at the map's own row/column stride.
    mask = np.unpackbits(np.fromfile(MASK, dtype=np.uint8),
                         bitorder="little").reshape(600, 1024)
    row0_1x = MAP_ROW0 * 2 * 600 // HEIGHT
    rows_1x = MAP_H * 2 * 600 // HEIGHT
    col_stride = 1024 // MAP_W if MAP_W <= 1024 else 1
    row_stride = max(1, 600 * 2 // HEIGHT)
    water_half = mask[row0_1x : row0_1x + rows_1x : row_stride, ::col_stride] == 1
    water_half = water_half[:MAP_H, :MAP_W]
    served = (grid[..., 1] != SENTINEL) & water_half[:grid.shape[0], :grid.shape[1]]
    print(f"solver serves {served.sum()} of {water_half.sum()} water cells "
          f"({served.sum() / water_half.sum() * 100.0:.1f}%); "
          f"the rest keep the sine-phase path")

    preview = np.zeros((MAP_H, MAP_W, 3), np.uint8)
    preview[water_half] = (40, 60, 130)          # analytic water
    preview[served] = (60, 180, 90)              # solver-served water
    preview[~water_half] = (90, 80, 70)          # relief / sky
    Image.fromarray(np.repeat(np.repeat(preview, 2, 0), 2, 1)).save(args.preview)
    print(f"wrote {args.preview}")


if __name__ == "__main__":
    main()
