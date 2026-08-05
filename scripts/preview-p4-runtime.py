#!/usr/bin/env python3
"""Render a host preview of the integer P4 sea/sky runtime contract."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image


W, H, HORIZON = 1024, 600, 291
TW, TH = 256, 96


def runtime_frame(base: np.ndarray, water: np.ndarray, shore: np.ndarray,
                  ocean_phase: np.ndarray,
                  atlases: dict[str, np.ndarray], state: dict, elapsed_ms: int,
                  fixture_clouds: bool) -> np.ndarray:
    out = base.copy().astype(np.int32)
    yy, xx = np.mgrid[0:H, 0:W]
    sky = yy < HORIZON
    palette = np.asarray(state["sky"]["palette_rgb"], dtype=np.int32)
    out[sky] = np.clip(out[sky] + (palette - np.array([141, 192, 223])), 0, 255)
    sun = state.get("sun", {}).get("state", "day")
    if sun == "civil_twilight":
        glow = np.where(sky, np.maximum(0, 150 - (HORIZON - yy)) * 150 / 150, 0)[..., None] / 255
        out = out * (1 - glow) + np.array([236, 145, 93]) * glow
    elif sun == "nautical_twilight":
        out[sky] = out[sky] * np.array([.34, .43, .56])
    elif sun == "night":
        out[sky] = out[sky] * np.array([.20, .28, .42])

    cover = float(state["sky"].get("observed_cloud_fraction") or 0)
    for shell in reversed(state["sky"]["shells"]):  # high, mid, low
        name = shell["name"]
        atlas = atlases[name].copy()
        if fixture_clouds:
            ay, ax = np.mgrid[0:TH, 0:TW]
            density = np.maximum(0, np.sin(2 * np.pi * (ax * 4 / TW + ay * 1.3 / TH)) +
                                 .55 * np.sin(2 * np.pi * (ax * 2 / TW - ay * .7 / TH)) - .35)
            atlas[..., 0] = np.clip(192 + density * 28, 0, 245)
            atlas[..., 1] = np.clip(density * 125, 0, 190)
            cover = .72
        height_mm = float(shell["projection_height_m"]) * 1000
        east = float(shell["advection"]["east_mps"]) * 1000
        north = float(shell["advection"]["north_mps"]) * 1000
        t = elapsed_ms % 21_600_000
        sx = int(-north * t * TW * 256 / (height_mm * 1919)) >> 8
        sy = int(east * t * TH * 256 / (height_mm * 1187)) >> 8
        ax = (xx * TW // W + sx) % TW
        raw_y = yy * TH // HORIZON + sy
        period_y = 2 * (TH - 1)
        ay = raw_y % period_y
        ay = np.where(ay < TH, ay, period_y - ay)
        luma, alpha = atlas[ay, ax, 0], atlas[ay, ax, 1] * cover
        alpha *= np.where(yy < HORIZON, np.minimum(1, (HORIZON - yy) / 22), 0)
        alpha = alpha[..., None] / 255
        tint = {"high": np.array([242, 235, 228]), "mid": np.array([242, 238, 233]),
                "low": np.array([242, 240, 238])}[name]
        out = np.where(sky[..., None], out * (1 - alpha) + tint * alpha, out)

    ocean = state["ocean"]
    sine = np.rint(127 * np.sin(np.arange(256) * 2 * np.pi / 256)).astype(np.int32)
    components = ocean["components"]
    normal = np.zeros((H, W), dtype=np.int32)
    total_weight = 0
    phases = []
    for index, component in enumerate(components):
        period = max(500, round(float(component["period_s"]) * 1000))
        phase = elapsed_ms * 256 // period
        surface_phase = (ocean_phase[..., index].astype(np.int32) - phase) & 255
        phases.append(surface_phase)
        weight = 1 + min(30, round(float(component["height_m"]) * 1000 / 80))
        normal += sine[(surface_phase + 64) & 255] * weight
        total_weight += weight
    shade = normal / max(total_weight * 4, 1)
    out[water] = base[water]
    horizon_fresnel = 255 - np.minimum(220, (yy - HORIZON) * 220 / (H - HORIZON))
    glint = np.maximum(shade, 0) * horizon_fresnel / 255
    reflection = palette.reshape(1, 1, 3)
    lit = out + (reflection - out) * glint[..., None] / 48
    lit += (255 - lit) * np.maximum(shade, 0)[..., None] / 96
    lit += lit * np.minimum(shade, 0)[..., None] / 96
    out[water] = lit[water]
    breaker = sine[(phases[0] + shore * 5) & 255]
    foam = np.where(water & (shore < 36) & (breaker > 58),
                    np.minimum(240, (breaker - 58) * (36 - shore) / 48), 0)[..., None] / 256
    out = np.where(water[..., None], out + (255 - out) * foam, out)
    if sun == "civil_twilight":
        glow = np.where(water, np.maximum(0, 130 - (yy - HORIZON)) * 46 / 130, 0)[..., None] / 255
        out = out * (1 - glow) + np.array([205, 120, 88]) * glow
    elif sun == "nautical_twilight":
        out[water] = out[water] * np.array([.34, .42, .52])
    elif sun == "night":
        out[water] = out[water] * np.array([.18, .24, .34])
    return np.clip(out, 0, 255).astype(np.uint8)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fixture-clouds", action="store_true",
                        help="Exercise shell projection with labeled synthetic density; never deploy it")
    args = parser.parse_args()
    state = json.loads(args.state.read_text())
    if state["camera"]["horizon_y_px"] != HORIZON:
        raise ValueError("preview requires fixed horizon row 291")
    base = np.asarray(Image.open(args.assets / "nubble_runtime_base.jpg").convert("RGB"))
    packed = np.fromfile(args.assets / "nubble_runtime_water_mask.bin", dtype=np.uint8)
    water = np.unpackbits(packed, bitorder="little")[:W * H].reshape(H, W).astype(bool)
    shore = np.fromfile(args.assets / "nubble_runtime_shore_distance.bin", dtype=np.uint8).reshape(H, W)
    ocean_phase = np.fromfile(args.assets / "nubble_runtime_ocean_phase.bin", dtype=np.uint8)
    ocean_phase = ocean_phase.reshape(H // 2, W // 2, 3).repeat(2, axis=0).repeat(2, axis=1)
    atlases = {name: np.fromfile(args.assets / f"nubble_runtime_cloud_{name}.bin", dtype=np.uint8)
               .reshape(TH, TW, 2) for name in ("low", "mid", "high")}
    args.output.mkdir(parents=True, exist_ok=True)
    for index, elapsed in enumerate((0, 4000, 8000)):
        image = runtime_frame(base, water, shore, ocean_phase, atlases, state, elapsed,
                              args.fixture_clouds)
        Image.fromarray(image).save(args.output / f"frame-{index}.png")
    print(args.output)


if __name__ == "__main__":
    main()
