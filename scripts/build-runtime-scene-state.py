#!/usr/bin/env python3
"""Build the compact, continuous Luminary runtime scene-state contract."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


def vector_from(speed_mps: float, from_deg: float) -> dict:
    toward = math.radians((from_deg + 180.0) % 360.0)
    return {"east_mps": round(math.sin(toward) * speed_mps, 4),
            "north_mps": round(math.cos(toward) * speed_mps, 4)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--conditions", type=Path, required=True)
    parser.add_argument("--clouds", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    conditions = json.loads(args.conditions.read_text())
    clouds = json.loads(args.clouds.read_text())
    if clouds.get("schema") != "luminary-goes-cloud-shells/v2":
        raise ValueError("cloud input is not a measured shell manifest")

    camera_observation = conditions.get("visual_observation", {})
    fallback_cloud_fraction = {"clear": 0.0, "few": 0.2, "broken": 0.65,
                               "overcast": 1.0, "fog": 0.85}.get(conditions.get("sky"), 0.35)
    shells = []
    for shell in clouds["cloud_shells"]:
        speed_mps = float(shell["wind_knots"]) * 0.514444
        shells.append({
            "name": shell["name"],
            "height_range_m": shell["height_range_m"],
            "projection_height_m": shell["projection_height_m"],
            "wind_from_deg": shell["wind_from_deg"],
            "wind_speed_mps": round(speed_mps, 4),
            "advection": vector_from(speed_mps, shell["wind_from_deg"]),
            "wind_source": shell["wind_source"],
            "texture": f"cloud-{shell['name']}-latest.png",
        })

    wave_height = float(conditions.get("wave_height_m") or 0.8)
    wave_period = float(conditions.get("wave_period_s") or 7.0)
    wave_from = float(conditions.get("mean_wave_direction_deg") or 137.0)
    # Deep-water dispersion: wavelength = g*T^2/(2*pi). This is converted to
    # an image-space displacement only after projection through the camera.
    wavelength = 9.80665 * wave_period * wave_period / (2.0 * math.pi)
    state = {
        "schema": "luminary-runtime-scene/v1",
        "updated_at": conditions.get("updated_at"),
        "camera": {"width_px": 1024, "height_px": 600, "horizon_y_px": 291,
                   "bearing_deg": clouds["camera"]["bearing_deg"], "locked": True,
                   "vertical_motion": "forbidden"},
        "sky": {
            "palette_rgb": camera_observation.get("sky_rgb", [145, 190, 220]),
            "observed_cloud_fraction": camera_observation.get("cloud_fraction", fallback_cloud_fraction),
            "visibility_confidence": camera_observation.get("confidence", 0.0),
            "shells": shells,
            "advance": "continuous monotonic time; never reset at asset boundary",
            "composite_order": ["high", "mid", "low"],
        },
        "ocean": {
            "significant_wave_height_m": wave_height,
            "dominant_period_s": wave_period,
            "dominant_wavelength_m": round(wavelength, 3),
            "wave_from_deg": wave_from,
            "phase_velocity_mps": round(wavelength / wave_period, 3),
            "wind_from_deg": conditions.get("wind_direction_deg"),
            "wind_knots": conditions.get("wind_knots"),
            "tide_phase": conditions.get("tide", {}).get("phase"),
            "shore_interaction": "registered water mask + signed shore distance; foam only on incoming faces",
            "advance": "continuous spectral phase; no finite loop",
        },
        "sun": conditions.get("sun", {}),
        "moon": conditions.get("moon", {}),
        "sources": {"conditions": str(args.conditions), "cloud_manifest": str(args.clouds),
                    "cloud_provider": clouds["provider"]},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(state, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
