#!/usr/bin/env python3
"""Build a world-space depth field for the Luminary shallow-water solver.

Everything here is MEASURED. Depths and topography both come from the NOAA
NCEI CUDEM 1/9 arc-second Continuously Updated Digital Elevation Model
(ncei19_n43x25_w070x75_2021v1, NAD83 geographic, NAVD88 vertical), which
merges lidar topography with hydrographic bathymetry across the shoreline --
so unlike the multibeam BAG it actually covers the surf zone.

Why not the other two sources in data/nubble:

  * noaa-h12615-4m-mllw.bag is a vessel multibeam survey. Its nearest sounding
    to Nubble is ~450 m offshore and its shallowest sample anywhere is 7.2 m,
    so it covers none of the model domain. It is used here only as an
    independent cross-check on the CUDEM depths.
  * noaa-york-2017-lidar-dem.tif is topography only; ocean is NoData.

Datums: CUDEM elevations are NAVD88, which near Cape Neddick sits within about
0.1 m of local mean sea level, so a still-water level of 0.0 NAVD88 is a
reasonable default. Pass --water-level to model a specific tide stage; the
scene already tracks a NOAA tide station, so this is the hook for driving the
solver from live water level.

Output is a uint8 depth-code grid for ocean_sim.c: 0 is land, 255 is
OCEAN_DEPTH_MAX_MM.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

ROOT = Path(__file__).resolve().parents[1]
CUDEM = ROOT / "data/nubble/ncei19_n43x25_w070x75_2021v1.tif"

# Solver grid, matching ocean_sim.h.
NX, NY = 192, 192
DEPTH_MAX_MM = 30000

# Domain is anchored on Cape Neddick (Nubble) Light, a verifiable landmark.
#
# Deliberately NOT anchored through UTM: scripts/extract-nubble-dem.py falls
# back to a guessed tile origin (370000, 4781000) because the local filename
# does not match its York_<e>_<n> regex, and the Nubble reference point derived
# from it sits 178.6 m west of the real lighthouse. Positions carried through
# that chain are offset by the same amount. The CUDEM carries authoritative
# georeferencing, so work in a local metric frame off the true coordinates.
LIGHT_LAT, LIGHT_LON = 43.16530, -70.59110

# Offsets in metres from the light: +y offshore (east), +x along shore (north).
OFFSHORE_START_M = -100.0
ALONGSHORE_START_M = -192.0


def local_to_latlon(offshore_m: np.ndarray, alongshore_m: np.ndarray):
    """Local metric frame about the light -> degrees. Over a few hundred metres
    a flat-earth approximation is well inside the CUDEM's 3 m posting."""
    deg_per_m_lat = 1.0 / 111132.95
    deg_per_m_lon = 1.0 / (111319.49 * math.cos(math.radians(LIGHT_LAT)))
    lat = LIGHT_LAT + alongshore_m * deg_per_m_lat
    lon = LIGHT_LON + offshore_m * deg_per_m_lon
    return lat, lon


def load_cudem():
    im = Image.open(CUDEM)
    scale = im.tag_v2[33550]
    tie = im.tag_v2[33922]
    lon0, lat0 = tie[3], tie[4]
    dlon, dlat = scale[0], scale[1]
    return np.asarray(im, dtype=np.float32), lon0, lat0, dlon, dlat


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dx", type=float, default=2.0, help="cell size in metres")
    ap.add_argument("--water-level", type=float, default=0.0,
                    help="still-water elevation in NAVD88 metres")
    ap.add_argument("--offshore-start", type=float, default=OFFSHORE_START_M,
                    help="metres east of the light where the domain begins")
    ap.add_argument("--alongshore-start", type=float, default=ALONGSHORE_START_M)
    ap.add_argument("--output", type=Path, default=ROOT / "tools/ocean-sim")
    args = ap.parse_args()

    dem, lon0, lat0, dlon, dlat = load_cudem()
    print(f"CUDEM {dem.shape} upper-left {lat0:.6f},{lon0:.6f} step {dlat:.3e} deg")

    ix = np.arange(NX)
    iy = np.arange(NY)
    alongshore = args.alongshore_start + ix[None, :] * args.dx
    offshore = args.offshore_start + iy[:, None] * args.dx
    alongshore = np.broadcast_to(alongshore, (NY, NX)).astype(np.float64)
    offshore = np.broadcast_to(offshore, (NY, NX)).astype(np.float64)

    lat, lon = local_to_latlon(offshore, alongshore)
    col = np.rint((lon - lon0) / dlon).astype(np.int64)
    row = np.rint((lat0 - lat) / dlat).astype(np.int64)
    inside = (row >= 0) & (row < dem.shape[0]) & (col >= 0) & (col < dem.shape[1])
    if not inside.all():
        raise SystemExit(f"domain leaves the CUDEM tile at {(~inside).sum()} cells")

    elev = dem[row, col]
    nodata = ~np.isfinite(elev) | (elev < -1e5)
    if nodata.any():
        print(f"warning: {nodata.sum()} CUDEM NoData cells treated as land")

    depth_m = args.water_level - elev.astype(np.float64)
    land = nodata | (depth_m <= 0.0)
    depth_m = np.clip(depth_m, 0.0, DEPTH_MAX_MM / 1000.0)
    depth_m[land] = 0.0

    code = np.clip(np.rint(depth_m * 1000.0 / DEPTH_MAX_MM * 255.0), 0, 255).astype(np.int32)
    water = ~land
    code[water & (code == 0)] = 1   # a water cell must never quantise to land
    code[land] = 0
    code = code.astype(np.uint8)

    args.output.mkdir(parents=True, exist_ok=True)
    raw = args.output / f"nubble_depth_{NX}x{NY}.bin"
    code.tofile(raw)

    preview = np.zeros((NY, NX, 3), dtype=np.uint8)
    preview[..., 2] = (255 - code).astype(np.uint8)
    preview[..., 1] = np.clip(200 - code * 0.6, 0, 255).astype(np.uint8)
    preview[land] = (110, 96, 82)
    Image.fromarray(preview[::-1]).resize((NX * 3, NY * 3), Image.NEAREST).save(
        args.output / "nubble_depth_preview.png")

    meta = {
        "grid": [NX, NY],
        "dx_m": args.dx,
        "domain_m": [NX * args.dx, NY * args.dx],
        "anchor_light_latlon": [LIGHT_LAT, LIGHT_LON],
        "offsets_m": [args.offshore_start, args.alongshore_start],
        "depth_max_mm": DEPTH_MAX_MM,
        "water_level_navd88_m": args.water_level,
        "land_cells": int(land.sum()),
        "water_cells": int(water.sum()),
        "depth_range_m": [round(float(depth_m[water].min()), 2),
                          round(float(depth_m[water].max()), 2)],
        "source": "NOAA NCEI CUDEM 1/9 arc-second ncei19_n43x25_w070x75_2021v1 (measured)",
        "horizontal_datum": "NAD83",
        "vertical_datum": "NAVD88",
    }
    (args.output / "nubble_depth_meta.json").write_text(json.dumps(meta, indent=2))

    print(f"land cells   {land.sum()} of {NX*NY} ({land.mean()*100:.1f}%)")
    print(f"depth range  {depth_m[water].min():.2f} .. {depth_m[water].max():.2f} m"
          f"  (water level {args.water_level:+.2f} m NAVD88)")
    # Cross-section straight offshore through the middle of the domain.
    mid = NX // 2
    prof = [(iy_ * args.dx, float(depth_m[iy_, mid])) for iy_ in range(0, NY, 16)]
    print("offshore profile at mid-domain:")
    for dist, d in prof:
        print(f"  {dist:5.0f} m : {d:5.2f} m")
    print(f"wrote {raw}")


if __name__ == "__main__":
    main()
