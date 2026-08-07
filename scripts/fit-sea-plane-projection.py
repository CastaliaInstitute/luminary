#!/usr/bin/env python3
"""Recover the Nubble camera's sea-plane projection from measured data.

The display renders the sea behind a printed relief, and the ocean solver
works in world space, so every water pixel needs a world position on the sea
plane. The camera model that provides it was never recorded: the scene JSON
carries the bearing (90 deg) and the horizon row (291) but not the eye height,
focal length, or exact camera position, and the base image is a real webcam
photograph.

This script fits those missing parameters instead of guessing them. Two
measured inputs constrain the model:

  * the runtime water mask, whose island waterline (the bottom edge of the
    island silhouette, where island meets water at sea level) is a contour of
    known world position: the island's shoreline in the NOAA CUDEM;
  * the water pixels themselves, which must all project onto CUDEM water --
    a projection that drops sea pixels onto the island or the mainland is
    geometrically impossible.

Land pixels above the waterline are NOT usable: relief rises above the sea
plane, so a land pixel's ray-plane intersection lands beyond its true
footprint. Only the waterline touches the plane.

The camera is a pinhole: real webcams have lens distortion this ignores, so
expect a few metres of registration error at the frame edges. Pitch is not a
free parameter -- it is pinned by the locked horizon row.

Outputs config/nubble-sea-projection.json plus a diagnostic overlay. The fit
prior for the camera position is the Sohier Park webcam, but the optimiser is
free to move it by tens of metres.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage, optimize

Image.MAX_IMAGE_PIXELS = None

ROOT = Path(__file__).resolve().parents[1]
MASK = ROOT / "firmware/luminary-background-viewer/assets/runtime/nubble_runtime_water_mask.bin"
CUDEM = ROOT / "data/nubble/ncei19_n43x25_w070x75_2021v1.tif"

WIDTH, HEIGHT, HORIZON = 1024, 600, 291
CX, CY = (WIDTH - 1) / 2.0, (HEIGHT - 1) / 2.0

# Same anchor and flat-earth frame as build-ocean-bathymetry.py: north and
# east in metres relative to Cape Neddick Light.
LIGHT_LAT, LIGHT_LON = 43.16530, -70.59110
DEG_PER_M_LAT = 1.0 / 111132.95
DEG_PER_M_LON = 1.0 / (111319.49 * math.cos(math.radians(LIGHT_LAT)))

# Local land raster extent (metres from the light) and resolution.
NORTH_MIN, NORTH_MAX = -450.0, 450.0
EAST_MIN, EAST_MAX = -350.0, 900.0
CELL_M = 2.0

# Sohier Park webcam, the prior for the camera position.
CAM_PRIOR_LAT, CAM_PRIOR_LON = 43.16418, -70.59349


def load_water_mask() -> np.ndarray:
    bits = np.unpackbits(np.fromfile(MASK, dtype=np.uint8), bitorder="little")
    return bits.reshape(HEIGHT, WIDTH)


def load_land_raster(water_level: float):
    """CUDEM elevations resampled onto the local metric frame."""
    im = Image.open(CUDEM)
    scale = im.tag_v2[33550]
    tie = im.tag_v2[33922]
    lon0, lat0 = tie[3], tie[4]
    dlon, dlat = scale[0], scale[1]
    dem = np.asarray(im, dtype=np.float32)

    north = np.arange(NORTH_MIN, NORTH_MAX, CELL_M)
    east = np.arange(EAST_MIN, EAST_MAX, CELL_M)
    ee, nn = np.meshgrid(east, north)
    lat = LIGHT_LAT + nn * DEG_PER_M_LAT
    lon = LIGHT_LON + ee * DEG_PER_M_LON
    col = np.rint((lon - lon0) / dlon).astype(np.int64)
    row = np.rint((lat0 - lat) / dlat).astype(np.int64)
    valid = (row >= 0) & (row < dem.shape[0]) & (col >= 0) & (col < dem.shape[1])
    elev = np.full(nn.shape, np.nan, dtype=np.float64)
    elev[valid] = dem[row[valid], col[valid]]
    nodata = ~np.isfinite(elev) | (elev < -1e5)
    land = nodata | (elev >= water_level)

    # Metres to the nearest land cell (0 on land) and to the nearest water
    # cell (0 on water). Their max is distance to the shoreline itself.
    d_to_land = ndimage.distance_transform_edt(~land) * CELL_M
    d_to_water = ndimage.distance_transform_edt(land) * CELL_M
    return land, d_to_land, d_to_water


def world_to_cell(east: np.ndarray, north: np.ndarray):
    ix = (east - EAST_MIN) / CELL_M
    iy = (north - NORTH_MIN) / CELL_M
    return ix, iy


def island_waterline(mask: np.ndarray):
    """Bottom edge of the island silhouette: its sea-level contact line.

    The island is the land blob whose extent touches the horizon band; its
    lowest pixels per column, with water immediately below, sit on the sea
    plane at the island's true shoreline.
    """
    land = mask == 0
    land[:HORIZON] = False
    labels, count = ndimage.label(land)
    island_label = 0
    for index in range(1, count + 1):
        ys = np.where(labels == index)[0]
        if ys.min() <= HORIZON + 2:
            island_label = index
            break
    if island_label == 0:
        raise SystemExit("no land blob touches the horizon; wrong mask?")

    points = []
    island = labels == island_label
    for x in range(WIDTH):
        ys = np.where(island[:, x])[0]
        if ys.size == 0:
            continue
        bottom = ys.max()
        # Require real water below, not another blob or the frame edge.
        if bottom + 4 < HEIGHT and mask[bottom + 1 : bottom + 4, x].all():
            points.append((x, bottom + 0.5))
    return np.array(points, dtype=np.float64)


def project(params, xs, ys):
    """Pixel coordinates -> sea-plane (east, north). NaN above horizon."""
    cam_e, cam_n, cam_h, focal, yaw_deg = params
    pitch = math.atan2(CY - HORIZON, focal)  # pinned by the locked horizon
    yaw = math.radians(yaw_deg)

    fwd = np.array([math.sin(yaw) * math.cos(pitch),
                    math.cos(yaw) * math.cos(pitch),
                    -math.sin(pitch)])
    right = np.array([math.cos(yaw), -math.sin(yaw), 0.0])
    down = np.cross(fwd, right)

    u = (xs - CX) / focal
    v = (ys - CY) / focal
    ray = fwd[None, :] + u[:, None] * right[None, :] + v[:, None] * down[None, :]
    rz = ray[:, 2]
    with np.errstate(divide="ignore", invalid="ignore"):
        t = np.where(rz < -1e-9, cam_h / -rz, np.nan)
    east = cam_e + t * ray[:, 0]
    north = cam_n + t * ray[:, 1]
    return east, north


def visible_island_shoreline(land: np.ndarray, cam_e: float, cam_n: float):
    """Island shoreline cells with a clear water path to the camera.

    The island footprint is the land component containing the light (the
    frame origin). A shoreline cell only constrains the fit if the camera can
    actually see it, so occluded cells -- the island's far side -- are
    dropped by marching each sight line and requiring it to stay on water.
    """
    labels, _ = ndimage.label(land)
    ox, oy = world_to_cell(np.array([0.0]), np.array([0.0]))
    island = labels == labels[int(oy[0]), int(ox[0])]
    boundary = island & ~ndimage.binary_erosion(island)
    by, bx = np.where(boundary)
    east = EAST_MIN + bx * CELL_M
    north = NORTH_MIN + by * CELL_M

    steps = np.linspace(0.02, 0.98, 160)
    se = cam_e + (east[:, None] - cam_e) * steps[None, :]
    sn = cam_n + (north[:, None] - cam_n) * steps[None, :]
    ix = np.clip(((se - EAST_MIN) / CELL_M).astype(int), 0, land.shape[1] - 1)
    iy = np.clip(((sn - NORTH_MIN) / CELL_M).astype(int), 0, land.shape[0] - 1)
    blocked = (land[iy, ix] & ~island[iy, ix]).any(axis=1)
    onto_self = island[iy, ix].sum(axis=1) > 3  # grazing its own coast
    visible = ~blocked & ~onto_self
    return east[visible], north[visible]


def sample_distance(field: np.ndarray, east: np.ndarray, north: np.ndarray,
                    outside: float) -> np.ndarray:
    ix, iy = world_to_cell(east, north)
    inside = (np.isfinite(ix) & np.isfinite(iy) &
              (ix >= 0) & (ix < field.shape[1] - 1) &
              (iy >= 0) & (iy < field.shape[0] - 1))
    result = np.full(east.shape, outside)
    result[inside] = field[iy[inside].astype(int), ix[inside].astype(int)]
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--water-level", type=float, default=0.0,
                        help="NAVD88 still-water level used for the shoreline")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "config/nubble-sea-projection.json")
    parser.add_argument("--diagnostic", type=Path,
                        default=ROOT / "scenes/nubble-aligned/compiled/sea-projection-fit.png")
    args = parser.parse_args()

    mask = load_water_mask()
    land, d_to_land, d_to_water = load_land_raster(args.water_level)
    waterline = island_waterline(mask)
    print(f"island waterline: {len(waterline)} columns")

    # Water sample: every water pixel on a coarse lattice, clear of the
    # ambiguous band just under the horizon where the mask carries the
    # printed breaker strip.
    wy, wx = np.where(mask[HORIZON + 40 :] == 1)
    wy = wy + HORIZON + 40
    step = max(1, wy.size // 4000)
    wx, wy = wx[::step].astype(np.float64), wy[::step].astype(np.float64)

    cam_prior_e = (CAM_PRIOR_LON - LIGHT_LON) / DEG_PER_M_LON
    cam_prior_n = (CAM_PRIOR_LAT - LIGHT_LAT) / DEG_PER_M_LAT
    print(f"camera prior: east={cam_prior_e:.0f} m north={cam_prior_n:.0f} m of the light")

    from scipy.spatial import cKDTree

    def cost(params):
        # Island waterline points belong ON the shoreline: penalise distance
        # to it in either direction.
        we, wn = project(params, waterline[:, 0], waterline[:, 1])
        shoreline_error = np.maximum(
            sample_distance(d_to_land, we, wn, 60.0),
            sample_distance(d_to_water, we, wn, 0.0))
        # Water pixels must not land on shore; clip so one bad region does
        # not dominate.
        pe, pn = project(params, wx, wy)
        intrusion = sample_distance(d_to_water, pe, pn, 0.0)
        # Coverage: the projected waterline must SPAN the shoreline the
        # camera can see, not cluster on one patch of it. Without this term
        # the fit collapses: it parks the camera just off the island at
        # minimal height so every waterline pixel lands a cell or two from
        # shore while covering almost none of it.
        vis_e, vis_n = visible_island_shoreline(land, params[0], params[1])
        if vis_e.size < 20:
            return 1e6
        finite = np.isfinite(we) & np.isfinite(wn)
        if finite.sum() < 20:
            return 1e6
        tree = cKDTree(np.column_stack([we[finite], wn[finite]]))
        gap, _ = tree.query(np.column_stack([vis_e, vis_n]))
        return (np.mean(np.minimum(shoreline_error, 60.0)) +
                2.0 * np.mean(np.minimum(intrusion, 40.0)) +
                np.mean(np.minimum(gap, 60.0)))

    bounds = [
        (cam_prior_e - 160.0, cam_prior_e + 160.0),
        (cam_prior_n - 160.0, cam_prior_n + 160.0),
        (1.0, 40.0),          # eye height, metres above still water
        (300.0, 6000.0),      # focal length, pixels
        (40.0, 130.0),        # yaw, degrees true
    ]
    coarse = optimize.differential_evolution(cost, bounds, seed=7, tol=1e-6,
                                             maxiter=120, popsize=24,
                                             polish=False)
    fine = optimize.minimize(cost, coarse.x, method="Nelder-Mead",
                             options={"xatol": 1e-3, "fatol": 1e-4,
                                      "maxiter": 4000})
    cam_e, cam_n, cam_h, focal, yaw = fine.x
    vfov = 2.0 * math.degrees(math.atan(HEIGHT / 2.0 / focal))
    pitch = math.degrees(math.atan2(CY - HORIZON, focal))
    print(f"cost: coarse {coarse.fun:.2f} -> polished {fine.fun:.2f}")
    print(f"camera: east={cam_e:.1f} north={cam_n:.1f} h={cam_h:.2f} m")
    print(f"        focal={focal:.0f} px (vfov {vfov:.1f} deg) yaw={yaw:.2f} pitch={pitch:.3f} deg down")

    we, wn = project(fine.x, waterline[:, 0], waterline[:, 1])
    residual = np.maximum(sample_distance(d_to_land, we, wn, 60.0),
                          sample_distance(d_to_water, we, wn, 0.0))
    pe, pn = project(fine.x, wx, wy)
    intrusion = sample_distance(d_to_water, pe, pn, 0.0)
    print(f"waterline residual: median {np.median(residual):.1f} m, "
          f"90th pct {np.percentile(residual, 90):.1f} m")
    print(f"water-on-land: {(intrusion > 4.0).mean() * 100.0:.1f}% of samples beyond 4 m")

    lat = LIGHT_LAT + cam_n * DEG_PER_M_LAT
    lon = LIGHT_LON + cam_e * DEG_PER_M_LON
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({
        "$schema": "https://luminary.castalia.institute/schemas/sea-projection-v1.json",
        "method": "fit of a pinhole sea-plane projection to the island waterline "
                  "in the runtime water mask against the NOAA CUDEM shoreline; "
                  "see scripts/fit-sea-plane-projection.py",
        "anchor_light_latlon": [LIGHT_LAT, LIGHT_LON],
        "camera": {
            "east_m": round(cam_e, 2),
            "north_m": round(cam_n, 2),
            "height_m": round(cam_h, 2),
            "latlon": [round(lat, 6), round(lon, 6)],
            "focal_px": round(focal, 1),
            "vfov_deg": round(vfov, 2),
            "yaw_deg": round(yaw, 3),
            "pitch_deg_down": round(pitch, 4),
        },
        "panel": {"width_px": WIDTH, "height_px": HEIGHT, "horizon_y_px": HORIZON},
        "water_level_navd88_m": args.water_level,
        "fit": {
            "cost": round(float(fine.fun), 3),
            "waterline_residual_median_m": round(float(np.median(residual)), 2),
            "waterline_residual_p90_m": round(float(np.percentile(residual, 90)), 2),
            "water_on_land_fraction": round(float((intrusion > 4.0).mean()), 4),
        },
    }, indent=2) + "\n")
    print(f"wrote {args.output}")

    # Diagnostic: the land raster with the projected waterline and a water
    # sample scattered over it.
    diag = np.zeros((land.shape[0], land.shape[1], 3), np.uint8)
    diag[land] = (140, 115, 90)
    diag[~land] = (25, 45, 90)
    def mark(east, north, colour):
        ix, iy = world_to_cell(np.asarray(east), np.asarray(north))
        ok = (np.isfinite(ix) & (ix >= 0) & (ix < land.shape[1]) &
              np.isfinite(iy) & (iy >= 0) & (iy < land.shape[0]))
        diag[iy[ok].astype(int), ix[ok].astype(int)] = colour
    mark(pe, pn, (70, 170, 220))
    mark(we, wn, (255, 220, 40))
    cx, cy_ = world_to_cell(np.array([cam_e]), np.array([cam_n]))
    if 0 <= cx[0] < land.shape[1] and 0 <= cy_[0] < land.shape[0]:
        diag[int(cy_[0]) - 2 : int(cy_[0]) + 3, int(cx[0]) - 2 : int(cx[0]) + 3] = (255, 60, 60)
    args.diagnostic.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(diag[::-1]).save(args.diagnostic)
    print(f"wrote {args.diagnostic} (north up, camera red, waterline yellow, water blue)")


if __name__ == "__main__":
    main()
