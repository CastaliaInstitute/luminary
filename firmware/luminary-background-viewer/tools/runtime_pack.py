#!/usr/bin/env python3
"""Pack registered runtime assets and generate measured-state C constants."""

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frame", type=Path, required=True)
    parser.add_argument("--water-mask", type=Path, required=True)
    parser.add_argument("--shore-distance", type=Path, required=True)
    parser.add_argument("--cloud-dir", type=Path,
                        help="Directory containing cloud-{low,mid,high}-latest.png")
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    frame = Image.open(args.frame).convert("RGB")
    if frame.size != (1024, 600):
        raise ValueError("runtime frame must be 1024x600")
    frame.save(args.output / "nubble_runtime_base.jpg", quality=82, subsampling=0)

    if args.water_mask.suffix.lower() == ".bin":
        packed = np.fromfile(args.water_mask, dtype=np.uint8)
        water = np.unpackbits(packed, bitorder="little")[:600 * 1024].reshape(600, 1024) > 0
    else:
        water = np.asarray(Image.open(args.water_mask).convert("L")) > 127
    if water.shape != (600, 1024):
        raise ValueError("water mask must be 1024x600")
    np.packbits(water.reshape(-1), bitorder="little").tofile(args.output / "nubble_runtime_water_mask.bin")
    if args.shore_distance.suffix.lower() == ".bin":
        shore = np.fromfile(args.shore_distance, dtype=np.uint8).reshape(600, 1024)
    else:
        shore = np.asarray(Image.open(args.shore_distance).convert("L"), dtype=np.uint8)
    if shore.shape != water.shape:
        raise ValueError("shore distance must match water mask")
    shore.tofile(args.output / "nubble_runtime_shore_distance.bin")

    # Runtime cloud shells are compact luminance/alpha atlases.  The source
    # images are already the registered GOES camera projection; the firmware
    # treats their azimuth/elevation domain as a spherical shell and rotates
    # each atlas independently from its measured height-resolved wind.
    cloud_width, cloud_height = 256, 96
    for name in ("low", "mid", "high"):
        source = args.cloud_dir / f"cloud-{name}-latest.png" if args.cloud_dir else None
        if source and source.exists():
            rgba = Image.open(source).convert("RGBA")
            if rgba.size != (1024, 600):
                raise ValueError(f"{source}: cloud shell must be 1024x600")
            # Only the immutable sky side of the horizon belongs in an atlas.
            rgba = rgba.crop((0, 0, 1024, 291)).resize(
                (cloud_width, cloud_height), Image.Resampling.LANCZOS)
            pixels = np.asarray(rgba, dtype=np.uint8)
            luminance = np.rint(
                pixels[..., 0] * 0.2126 + pixels[..., 1] * 0.7152 + pixels[..., 2] * 0.0722
            ).astype(np.uint8).copy()
            alpha = pixels[..., 3].copy()
            # Longitude wraps continuously in the runtime shell. Blend only
            # the outer 1/8 of the atlas toward paired edge values so a stale
            # atlas can circulate for hours without exposing a vertical seam.
            blend = cloud_width // 8
            for offset in range(blend):
                strength = 1.0 - offset / max(blend - 1, 1)
                opposite = cloud_width - 1 - offset
                for channel in (luminance, alpha):
                    average = ((channel[:, offset].astype(np.uint16) +
                                channel[:, opposite].astype(np.uint16)) // 2).astype(np.uint8)
                    channel[:, offset] = np.rint(channel[:, offset] * (1 - strength) +
                                                 average * strength).astype(np.uint8)
                    channel[:, opposite] = np.rint(channel[:, opposite] * (1 - strength) +
                                                   average * strength).astype(np.uint8)
        else:
            luminance = np.zeros((cloud_height, cloud_width), dtype=np.uint8)
            alpha = np.zeros_like(luminance)
        np.dstack((luminance, alpha)).tofile(
            args.output / f"nubble_runtime_cloud_{name}.bin")

    state = json.loads(args.state.read_text())
    if state["camera"]["horizon_y_px"] != 291:
        raise ValueError("runtime horizon must be row 291")
    ocean = state["ocean"]
    relative = math.radians(float(ocean["wave_from_deg"]) - float(state["camera"]["bearing_deg"]))
    # Perspective-project the measured dominant wavelength onto the sea plane.
    # A byte stores one full phase cycle. Runtime advances that phase from
    # monotonic time, preserving small distant waves and broad foreground swell
    # without moving the registered horizon.
    yy, xx = np.mgrid[0:600, 0:1024].astype(np.float64)
    focal_x, focal_y = 358.53, 782.79
    depression_px = np.maximum(yy - 291.0, 1.0)
    horizontal_range_m = np.minimum(800.0, 12.0 * focal_y / depression_px)
    azimuth = np.arctan2(xx - 512.0, focal_x)
    forward = horizontal_range_m * np.cos(azimuth)
    cross = horizontal_range_m * np.sin(azimuth)
    wave_to_relative = math.radians((float(ocean["wave_from_deg"]) + 180.0) -
                                    float(state["camera"]["bearing_deg"]))
    along_wave = cross * math.sin(wave_to_relative) + forward * math.cos(wave_to_relative)
    wavelength = max(float(ocean["dominant_wavelength_m"]), 1.0)
    ocean_phase = np.mod(np.rint(along_wave / wavelength * 256.0), 256).astype(np.uint8)
    ocean_phase[~water] = 0
    ocean_phase.tofile(args.output / "nubble_runtime_ocean_phase.bin")
    sun_modes = {"day": 0, "civil_twilight": 1, "nautical_twilight": 2, "night": 3}
    sun_mode = sun_modes.get(state.get("sun", {}).get("state", "day"), 0)
    moon = state.get("moon", {})
    if moon.get("visible"):
        moon_relative = math.radians(float(moon.get("azimuth_deg", 0)) - float(state["camera"]["bearing_deg"]))
        moon_x = round(512 + ((1024 / 2) / math.tan(math.radians(110) / 2)) * math.tan(moon_relative))
        moon_y = round(291 - ((600 * 0.88) / math.tan(math.radians(68) / 2)) *
                       math.tan(math.radians(float(moon.get("altitude_deg", 0)))))
    else:
        moon_x = moon_y = -100
    lines = [
        "#pragma once", "/* Generated by runtime_pack.py; do not hand edit. */",
        f"#define LUMINARY_RUNTIME_HORIZON {state['camera']['horizon_y_px']}U",
        f"#define LUMINARY_SKY_R {int(state['sky']['palette_rgb'][0])}U",
        f"#define LUMINARY_SKY_G {int(state['sky']['palette_rgb'][1])}U",
        f"#define LUMINARY_SKY_B {int(state['sky']['palette_rgb'][2])}U",
        f"#define LUMINARY_CLOUD_COVER_PERMILLE {round(float(state['sky']['observed_cloud_fraction'] or 0) * 1000)}U",
        f"#define LUMINARY_CLOUD_TEXTURE_WIDTH {cloud_width}U",
        f"#define LUMINARY_CLOUD_TEXTURE_HEIGHT {cloud_height}U",
        f"#define LUMINARY_SUN_MODE {sun_mode}U",
        f"#define LUMINARY_MOON_VISIBLE {1 if moon.get('visible') else 0}U",
        f"#define LUMINARY_MOON_X {moon_x}",
        f"#define LUMINARY_MOON_Y {moon_y}",
        f"#define LUMINARY_MOON_ILLUMINATION_PERMILLE {round(float(moon.get('illumination', 0)) * 1000)}U",
        f"#define LUMINARY_WAVE_HEIGHT_MM {round(float(ocean['significant_wave_height_m']) * 1000)}U",
        f"#define LUMINARY_WAVE_PERIOD_MS {round(float(ocean['dominant_period_s']) * 1000)}U",
        f"#define LUMINARY_WAVE_KX_Q10 {round(math.sin(relative) * 1024)}",
        f"#define LUMINARY_WAVE_KY_Q10 {round(math.cos(relative) * 1024)}",
        f"#define LUMINARY_WAVE_FROM_DEG {round(float(ocean['wave_from_deg']))}U",
    ]
    for shell in state["sky"]["shells"]:
        prefix = shell["name"].upper()
        lines.extend([
            f"#define LUMINARY_{prefix}_CLOUD_HEIGHT_M {round(shell['projection_height_m'])}U",
            f"#define LUMINARY_{prefix}_WIND_EAST_MMPS {round(shell['advection']['east_mps'] * 1000)}",
            f"#define LUMINARY_{prefix}_WIND_NORTH_MMPS {round(shell['advection']['north_mps'] * 1000)}",
        ])
    (args.output / "luminary_runtime_state.h").write_text("\n".join(lines) + "\n")
    print(f"packed runtime assets in {args.output}")


if __name__ == "__main__":
    main()
