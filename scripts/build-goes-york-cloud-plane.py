#!/usr/bin/env python3
"""Fetch and camera-project live GOES-East clouds over York, Maine.

GOES ABI channel 13 is long-wave infrared, so it describes cloud-top
structure in both daylight and darkness.  This tool downloads the latest two
full-disk frames from NOAA's public GOES-19 bucket, derives a small cloud
motion vector, and projects the most-recent cloud field through Luminary's
locked Nubble camera.  The output is an RGBA overlay above -- never across --
the registered y=291 horizon.

The overlay deliberately carries only cloud opacity/structure.  The sky-dome
catalog remains responsible for plausible colour, sun, and twilight.  That
separation makes the satellite product inspectable and avoids treating a
top-down image as though it were a literal horizon photograph.

Requires the ``h5dump`` command from HDF5 (available with macOS Homebrew's
``hdf5`` package).  It avoids a Python netCDF dependency so the live renderer
can remain small and reproducible.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import re
import subprocess
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image


WIDTH, HEIGHT, HORIZON = 1024, 600, 291
NOAA_BUCKET = "https://noaa-goes19.s3.amazonaws.com"
PREFIX = "ABI-L2-CMIPF"
HEIGHT_PREFIX = "ABI-L2-ACHAF"
NS = {"s3": "http://s3.amazonaws.com/doc/2006-03-01/"}


def request(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "Luminary/0.1 contact@castalia.institute"})
    with urllib.request.urlopen(req, timeout=60) as response:
        return response.read()


def keys_for_hour(when: dt.datetime, product: str = PREFIX) -> list[str]:
    prefix = f"{product}/{when:%Y}/{when:%j}/{when:%H}/"
    root = ET.fromstring(request(f"{NOAA_BUCKET}/?list-type=2&prefix={prefix}&max-keys=1000"))
    keys = [node.text for node in root.findall("s3:Contents/s3:Key", NS)]
    return [key for key in keys if key and (product != PREFIX or "C13_" in key)]


def observation_time(key: str) -> dt.datetime:
    match = re.search(r"_s(\d{4})(\d{3})(\d{2})(\d{2})(\d{2})", key)
    if not match:
        raise ValueError(f"could not parse GOES time from {key}")
    year, day, hour, minute, second = map(int, match.groups())
    return dt.datetime(year, 1, 1, tzinfo=dt.timezone.utc) + dt.timedelta(
        days=day - 1, hours=hour, minutes=minute, seconds=second)


def latest_two_keys() -> list[str]:
    now = dt.datetime.now(dt.timezone.utc)
    keys: list[str] = []
    # A product can arrive several minutes after scan completion; search back
    # far enough to cover an hour boundary and a short upstream delay.
    for hour_back in range(4):
        keys.extend(keys_for_hour(now - dt.timedelta(hours=hour_back), PREFIX))
    unique = sorted(set(keys), key=observation_time)
    if len(unique) < 2:
        raise RuntimeError("NOAA GOES-19 channel 13 has fewer than two recent frames")
    return unique[-2:]


def closest_height_key(target: dt.datetime) -> str:
    keys: list[str] = []
    for hour_back in range(4):
        keys.extend(keys_for_hour(target - dt.timedelta(hours=hour_back), HEIGHT_PREFIX))
    if not keys:
        raise RuntimeError("NOAA GOES-19 cloud-top height has no recent frame")
    return min(set(keys), key=lambda key: abs((observation_time(key) - target).total_seconds()))


def run_h5dump(file: Path, dataset: str, *, dtype: str = "<i2",
               start: tuple[int, int] | None = None,
               count: tuple[int, int] | None = None) -> np.ndarray:
    """Extract an int16 HDF5 dataset through h5dump's raw output mode."""
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as temp:
        raw_path = Path(temp.name)
    command = ["h5dump", "-d", dataset, "-o", str(raw_path), "-b", "LE"]
    if start is not None:
        command.extend(["-s", ",".join(map(str, start))])
    if count is not None:
        command.extend(["-c", ",".join(map(str, count))])
    command.append(str(file))
    try:
        subprocess.run(command, check=True, capture_output=True, text=True)
        data = np.fromfile(raw_path, dtype=dtype)
    finally:
        raw_path.unlink(missing_ok=True)
    return data


def attribute(file: Path, name: str) -> float:
    result = subprocess.run(["h5dump", "-a", name, str(file)], check=True,
                            capture_output=True, text=True)
    match = re.search(r"\(0\):\s*([-+0-9.eE]+)", result.stdout)
    if not match:
        raise ValueError(f"could not read HDF5 attribute {name}")
    return float(match.group(1))


def projected_indices(lat_deg: np.ndarray, lon_deg: np.ndarray, *, height: float,
                      semi_major: float, semi_minor: float, lon_origin_deg: float,
                      x_coords: np.ndarray, y_coords: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Map geodetic coordinates to GOES fixed-grid row/column indices."""
    lat = np.radians(lat_deg)
    lon_delta = np.radians(lon_deg - lon_origin_deg)
    # NOAA's GOES fixed-grid forward transform (sweep axis x).
    phi_c = np.arctan((semi_minor ** 2 / semi_major ** 2) * np.tan(lat))
    e2 = (semi_major ** 2 - semi_minor ** 2) / semi_major ** 2
    rc = semi_minor / np.sqrt(1.0 - e2 * np.cos(phi_c) ** 2)
    sx = height - rc * np.cos(phi_c) * np.cos(lon_delta)
    sy = -rc * np.cos(phi_c) * np.sin(lon_delta)
    sz = rc * np.sin(phi_c)
    rn = np.sqrt(sx * sx + sy * sy + sz * sz)
    scan_x = np.arcsin(-sy / rn)
    scan_y = np.arctan(sz / sx)
    cols = np.interp(scan_x, x_coords, np.arange(x_coords.size), left=-1, right=-1)
    # y coordinates descend north-to-south in the ABI product.
    rows = np.interp(scan_y, y_coords[::-1], np.arange(y_coords.size)[::-1], left=-1, right=-1)
    valid = (cols >= 0) & (rows >= 0)
    return rows, cols, valid


def bilinear(image: np.ndarray, rows: np.ndarray, cols: np.ndarray) -> np.ndarray:
    r0 = np.floor(rows).astype(np.int32)
    c0 = np.floor(cols).astype(np.int32)
    r1 = np.minimum(r0 + 1, image.shape[0] - 1)
    c1 = np.minimum(c0 + 1, image.shape[1] - 1)
    tr, tc = rows - r0, cols - c0
    return ((image[r0, c0] * (1 - tc) + image[r0, c1] * tc) * (1 - tr) +
            (image[r1, c0] * (1 - tc) + image[r1, c1] * tc) * tr)


def phase_correlation(older: np.ndarray, newer: np.ndarray) -> tuple[tuple[float, float], float]:
    """Small dependency-free translation estimate for the saved GOES metadata."""
    # This estimate is diagnostic only; runtime motion comes from the observed
    # York wind. Removing a broken OpenCV binding keeps the fetch pipeline
    # runnable on a clean macOS machine.
    old = older - older.mean()
    new = newer - newer.mean()
    cross = np.fft.fft2(old) * np.conj(np.fft.fft2(new))
    cross /= np.maximum(np.abs(cross), 1e-9)
    correlation = np.fft.ifft2(cross).real
    row, col = np.unravel_index(np.argmax(correlation), correlation.shape)
    if col > correlation.shape[1] // 2:
        col -= correlation.shape[1]
    if row > correlation.shape[0] // 2:
        row -= correlation.shape[0]
    confidence = float(correlation.max() / max(correlation.std(), 1e-9))
    return (float(-col), float(-row)), confidence


def goesscan(file: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray, dict[str, float]]:
    """Read one C13 frame and its fixed-grid coordinates using only h5dump."""
    # These full coordinate vectors are only 5,424 entries each.  Reading
    # them permits a precise local crop rather than baking GOES grid constants.
    x_raw = run_h5dump(file, "/x")
    y_raw = run_h5dump(file, "/y")
    x = x_raw.astype(np.float32) * attribute(file, "/x/scale_factor") + attribute(file, "/x/add_offset")
    y = y_raw.astype(np.float32) * attribute(file, "/y/scale_factor") + attribute(file, "/y/add_offset")
    shape = (y.size, x.size)
    # We read the image only once here. C13 full disk is roughly 59 MB decoded;
    # acceptable on the host and discarded after the overlay is emitted.
    raw = run_h5dump(file, "/CMI")
    if raw.size != shape[0] * shape[1]:
        raise ValueError(f"{file}: CMI dimensions do not match coordinate vectors")
    temp_k = raw.reshape(shape).astype(np.float32) * attribute(file, "/CMI/scale_factor") + attribute(file, "/CMI/add_offset")
    metadata = {
        "height_m": attribute(file, "/goes_imager_projection/perspective_point_height") +
                    attribute(file, "/goes_imager_projection/semi_major_axis"),
        "semi_major_m": attribute(file, "/goes_imager_projection/semi_major_axis"),
        "semi_minor_m": attribute(file, "/goes_imager_projection/semi_minor_axis"),
        "lon_origin_deg": attribute(file, "/goes_imager_projection/longitude_of_projection_origin"),
    }
    return temp_k, x, y, metadata


def goesheight(file: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray, dict[str, float]]:
    """Read GOES ACHA cloud-top height in metres on its native fixed grid."""
    x_raw = run_h5dump(file, "/x")
    y_raw = run_h5dump(file, "/y")
    x = x_raw.astype(np.float32) * attribute(file, "/x/scale_factor") + attribute(file, "/x/add_offset")
    y = y_raw.astype(np.float32) * attribute(file, "/y/scale_factor") + attribute(file, "/y/add_offset")
    raw = run_h5dump(file, "/HT", dtype="<u2")
    shape = (y.size, x.size)
    if raw.size != shape[0] * shape[1]:
        raise ValueError(f"{file}: HT dimensions do not match coordinate vectors")
    raw = raw.reshape(shape)
    height_m = raw.astype(np.float32) * attribute(file, "/HT/scale_factor") + attribute(file, "/HT/add_offset")
    height_m[raw == int(attribute(file, "/HT/_FillValue"))] = np.nan
    metadata = {
        "height_m": attribute(file, "/goes_imager_projection/perspective_point_height") +
                    attribute(file, "/goes_imager_projection/semi_major_axis"),
        "semi_major_m": attribute(file, "/goes_imager_projection/semi_major_axis"),
        "semi_minor_m": attribute(file, "/goes_imager_projection/semi_minor_axis"),
        "lon_origin_deg": attribute(file, "/goes_imager_projection/longitude_of_projection_origin"),
    }
    return height_m, x, y, metadata


def render_overlay(temp_k: np.ndarray, x: np.ndarray, y: np.ndarray, geo: dict[str, float], *,
                   latitude: float, longitude: float, bearing_deg: float, cloud_height_m: float,
                   height_field: tuple[np.ndarray, np.ndarray, np.ndarray, dict[str, float]] | None = None,
                   height_range_m: tuple[float, float] | None = None,
                   color_scale: tuple[float, float, float] = (0.94, 0.98, 1.0)) -> np.ndarray:
    """Project one measured cloud-height shell through the registered camera."""
    yy, xx = np.mgrid[0:HEIGHT, 0:WIDTH].astype(np.float32)
    focal_x = (WIDTH / 2.0) / math.tan(math.radians(110.0) / 2.0)
    focal_y = (HEIGHT * 0.88) / math.tan(math.radians(68.0) / 2.0)
    azimuth = np.arctan2(xx - WIDTH / 2.0, focal_x)
    elevation = np.arctan2(HORIZON - yy, focal_y)
    horizontal_range = cloud_height_m / np.maximum(np.tan(elevation), 1e-4)
    # A finite cloud layer is not usefully represented at the mathematical
    # horizon.  The feather below also prevents a false hard line above y=291.
    # At a 3.5 km cloud plane one native 2 km satellite pixel expands into
    # vertical bars very near the horizon.  Let the registered sky dome carry
    # that distant atmospheric band; GOES stays authoritative in the portion
    # of the view where its top-down pixels have useful angular resolution.
    valid = (yy < HORIZON - 22) & (horizontal_range < 80_000.0)
    bearing = math.radians(bearing_deg) + azimuth
    east = horizontal_range * np.sin(bearing)
    north = horizontal_range * np.cos(bearing)
    lat = latitude + north / 110_540.0
    lon = longitude + east / (111_320.0 * math.cos(math.radians(latitude)))
    rows, cols, mapped = projected_indices(
        lat, lon, height=geo["height_m"], semi_major=geo["semi_major_m"],
        semi_minor=geo["semi_minor_m"], lon_origin_deg=geo["lon_origin_deg"],
        x_coords=x, y_coords=y)
    safe_rows = np.clip(rows, 0, temp_k.shape[0] - 1)
    safe_cols = np.clip(cols, 0, temp_k.shape[1] - 1)
    sampled = bilinear(temp_k, safe_rows, safe_cols)
    layer_valid = np.ones_like(valid, dtype=bool)
    if height_field is not None and height_range_m is not None:
        heights, height_x, height_y, height_geo = height_field
        height_rows, height_cols, height_mapped = projected_indices(
            lat, lon, height=height_geo["height_m"], semi_major=height_geo["semi_major_m"],
            semi_minor=height_geo["semi_minor_m"], lon_origin_deg=height_geo["lon_origin_deg"],
            x_coords=height_x, y_coords=height_y)
        nearest_rows = np.clip(np.rint(height_rows).astype(np.int32), 0, heights.shape[0] - 1)
        nearest_cols = np.clip(np.rint(height_cols).astype(np.int32), 0, heights.shape[1] - 1)
        sampled_height = heights[nearest_rows, nearest_cols]
        layer_valid = (height_mapped & np.isfinite(sampled_height) &
                       (sampled_height >= height_range_m[0]) &
                       (sampled_height < height_range_m[1]))
    # C13 is brightness temperature. Warm ocean/ground is mostly transparent;
    # cold high cloud becomes more opaque.  The gentle response keeps thin
    # cloud from turning into synthetic gray bands.
    opacity = np.clip((284.0 - sampled) / 47.0, 0.0, 1.0) ** 0.85
    opacity *= np.clip(((HORIZON - 26.0) - yy) / 62.0, 0.0, 1.0)
    # 89.6 K is the decoded ABI fill value around the limb; never mistake it
    # for an extremely cold cloud.
    opacity *= valid & mapped & layer_valid & (sampled > 150.0)
    # Retain measured cloud-top variation as a neutral, high-cloud-tinted
    # overlay; the colour dome below remains legible through it.
    brightness = np.clip(184.0 + (sampled - 205.0) * 1.45, 178.0, 242.0)
    rgba = np.empty((HEIGHT, WIDTH, 4), dtype=np.uint8)
    rgba[..., 0] = brightness * color_scale[0]
    rgba[..., 1] = brightness * color_scale[1]
    rgba[..., 2] = brightness * color_scale[2]
    rgba[..., 3] = np.clip(opacity * 190.0, 0, 190).astype(np.uint8)
    return rgba


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("assets/display/goes-york"))
    parser.add_argument("--latitude", type=float, default=43.1637)
    parser.add_argument("--longitude", type=float, default=-70.6480)
    parser.add_argument("--bearing-deg", type=float, default=90.0)
    parser.add_argument("--cloud-height-m", type=float, default=3500.0,
                        help="Assumed lower-cloud plane; configurable because GOES provides cloud top, not ceiling.")
    parser.add_argument("--wind-from-deg", type=float, default=270.0,
                        help="Meteorological wind direction (the direction wind comes from).")
    parser.add_argument("--wind-knots", type=float, default=8.0)
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--loop-seconds", type=float, default=24.0,
                        help="Small wind-advection interval between NOAA refreshes.")
    parser.add_argument("--source", type=Path, nargs=2, metavar=("OLDER", "NEWER"),
                        help="Use two downloaded C13 NetCDF files instead of fetching NOAA.")
    parser.add_argument("--height-source", type=Path,
                        help="Use a downloaded GOES ACHA cloud-top-height NetCDF file.")
    parser.add_argument("--shell-winds", type=Path,
                        help="Height-resolved wind JSON from extract-goes-dmw-winds.py.")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    if args.source:
        older, newer = args.source
        source_keys = [str(older), str(newer)]
    else:
        source_keys = latest_two_keys()
        older = args.output / Path(source_keys[0]).name
        newer = args.output / Path(source_keys[1]).name
        for key, destination in zip(source_keys, (older, newer)):
            if not destination.exists():
                print(f"downloading {key}")
                destination.write_bytes(request(f"{NOAA_BUCKET}/{key}"))

    height_key = None
    if args.height_source:
        height_file = args.height_source
    else:
        height_key = closest_height_key(observation_time(source_keys[-1]))
        height_file = args.output / Path(height_key).name
        if not height_file.exists():
            print(f"downloading {height_key}")
            height_file.write_bytes(request(f"{NOAA_BUCKET}/{height_key}"))

    old_temp, old_x, old_y, old_geo = goesscan(older)
    new_temp, new_x, new_y, new_geo = goesscan(newer)
    height_field = goesheight(height_file)
    if old_temp.shape != new_temp.shape or not np.allclose(old_x, new_x) or not np.allclose(old_y, new_y):
        raise ValueError("GOES frames do not share an ABI fixed grid")
    shells = (
        {"name": "low", "height_range_m": (0.0, 3250.0), "projection_height_m": 2000.0,
         "color_scale": (0.95, 0.98, 1.0), "wind_scale": 1.0},
        {"name": "mid", "height_range_m": (3250.0, 6500.0), "projection_height_m": 5000.0,
         "color_scale": (0.94, 0.97, 1.0), "wind_scale": 1.0},
        {"name": "high", "height_range_m": (6500.0, 21000.0), "projection_height_m": 10000.0,
         "color_scale": (0.92, 0.96, 1.0), "wind_scale": 1.0},
    )
    measured_shell_winds = json.loads(args.shell_winds.read_text()).get("shells", {}) if args.shell_winds else {}
    for shell in shells:
        measured = measured_shell_winds.get(shell["name"], {})
        if measured.get("available"):
            shell["wind_from_deg"] = float(measured["wind_from_deg"])
            shell["wind_knots"] = float(measured["wind_knots"])
            shell["wind_source"] = f"GOES Derived Motion Winds ({measured['vector_count']} quality vectors)"
        else:
            shell["wind_from_deg"] = args.wind_from_deg
            shell["wind_knots"] = args.wind_knots
            shell["wind_source"] = "York observed surface wind fallback"

    def shell_view(temperature: np.ndarray, scan_x: np.ndarray, scan_y: np.ndarray,
                   scan_geo: dict[str, float], shell: dict, latitude: float, longitude: float) -> np.ndarray:
        return render_overlay(
            temperature, scan_x, scan_y, scan_geo, latitude=latitude, longitude=longitude,
            bearing_deg=args.bearing_deg, cloud_height_m=shell["projection_height_m"],
            height_field=height_field, height_range_m=shell["height_range_m"],
            color_scale=shell["color_scale"])

    old_shells = {shell["name"]: shell_view(old_temp, old_x, old_y, old_geo, shell,
                                             args.latitude, args.longitude) for shell in shells}
    new_shells = {shell["name"]: shell_view(new_temp, new_x, new_y, new_geo, shell,
                                             args.latitude, args.longitude) for shell in shells}
    # The legacy combined image remains for diagnostics and old viewers. New
    # renderers consume the independent high/mid/low shell images below.
    def composite_shells(shell_images: dict[str, np.ndarray]) -> np.ndarray:
        result = np.zeros((HEIGHT, WIDTH, 4), dtype=np.float32)
        for name in ("high", "mid", "low"):
            layer = shell_images[name].astype(np.float32)
            alpha = layer[..., 3:4] / 255.0
            result[..., :3] = result[..., :3] * (1.0 - alpha) + layer[..., :3] * alpha
            result[..., 3:4] = 255.0 * (1.0 - (1.0 - result[..., 3:4] / 255.0) * (1.0 - alpha))
        return np.clip(result, 0, 255).astype(np.uint8)

    old_view = composite_shells(old_shells)
    new_view = composite_shells(new_shells)
    # Phase correlation reports the image-space displacement from older to
    # newer.  It is metadata for the display renderer; generated frames use
    # actual satellite imagery rather than inventing a procedural wave pattern.
    response_image_old = old_view[..., 3].astype(np.float32) / 255.0
    response_image_new = new_view[..., 3].astype(np.float32) / 255.0
    shift, confidence = phase_correlation(response_image_old, response_image_new)
    Image.fromarray(old_view, mode="RGBA").save(args.output / "cloud-older.png")
    Image.fromarray(new_view, mode="RGBA").save(args.output / "cloud-latest.png")
    for shell in shells:
        name = shell["name"]
        Image.fromarray(old_shells[name], mode="RGBA").save(args.output / f"cloud-{name}-older.png")
        Image.fromarray(new_shells[name], mode="RGBA").save(args.output / f"cloud-{name}-latest.png")
    # The satellite pair tells us the present cloud structure; local NWS wind
    # drives the in-between frames.  Each frame samples the same top-down
    # cloud field at the upstream location, then reprojects it through the
    # camera. This is materially different from sliding a sky bitmap across
    # the screen: the apparent direction varies naturally with elevation.
    for index in range(args.frames):
        seconds = args.loop_seconds * index / args.frames
        frame_shells = {}
        for shell in shells:
            wind_to = math.radians(shell["wind_from_deg"] + 180.0)
            shell_speed = shell["wind_knots"] * 0.514444 * shell["wind_scale"]
            east = math.sin(wind_to) * shell_speed * seconds
            north = math.cos(wind_to) * shell_speed * seconds
            # Cloud at a later position comes from its upwind source position.
            frame_shells[shell["name"]] = shell_view(
                new_temp, new_x, new_y, new_geo, shell,
                args.latitude - north / 110_540.0,
                args.longitude - east / (111_320.0 * math.cos(math.radians(args.latitude))))
            Image.fromarray(frame_shells[shell["name"]], mode="RGBA").save(
                args.output / f"cloud-{shell['name']}-frame-{index:03d}.png")
        frame = composite_shells(frame_shells)
        Image.fromarray(frame, mode="RGBA").save(args.output / f"cloud-frame-{index:03d}.png")
    metadata = {
        "schema": "luminary-goes-cloud-shells/v2",
        "provider": "NOAA GOES-19 ABI L2 CMIPF channel 13 plus ABI L2 ACHA cloud-top height",
        "source_keys": source_keys,
        "cloud_top_height_source": height_key or str(height_file),
        "source_times_utc": [observation_time(key).isoformat() if not args.source else None for key in source_keys],
        "camera": {"latitude": args.latitude, "longitude": args.longitude,
                   "bearing_deg": args.bearing_deg, "horizon_y_px": HORIZON,
                   "width": WIDTH, "height": HEIGHT},
        "cloud_shells": [
            {"name": shell["name"], "height_range_m": list(shell["height_range_m"]),
             "projection_height_m": shell["projection_height_m"],
             "wind_scale": shell["wind_scale"],
             "wind_source": shell["wind_source"],
             "wind_from_deg": shell["wind_from_deg"], "wind_knots": shell["wind_knots"],
             "height_source": "GOES ACHA measured cloud-top height"}
            for shell in shells
        ],
        "motion": {"older_to_latest_px": [round(float(shift[0]), 3), round(float(shift[1]), 3)],
                   "phase_correlation_confidence": round(float(confidence), 5),
                   "loop_driver": "York wind advects the latest GOES cloud field between satellite refreshes",
                   "wind_from_deg": args.wind_from_deg, "wind_to_deg": (args.wind_from_deg + 180.0) % 360.0,
                   "wind_knots": args.wind_knots, "loop_seconds": args.loop_seconds,
                   "frame_count": args.frames},
        "outputs": {"older": "cloud-older.png", "latest": "cloud-latest.png",
                    "combined_frames": "cloud-frame-000.png … cloud-frame-%03d.png" % (args.frames - 1),
                    "shell_frames": "cloud-{low,mid,high}-frame-000.png … frame-%03d.png" % (args.frames - 1)},
        "horizon_rule": "alpha is zero at and below y=284; satellite clouds cannot move the registered sea/sky horizon."
    }
    (args.output / "goes-cloud-plane.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"wrote live GOES cloud projection to {args.output}")


if __name__ == "__main__":
    main()
