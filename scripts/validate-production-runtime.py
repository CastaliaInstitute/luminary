#!/usr/bin/env python3
"""Validate the production P4 runtime assets and immutable registration rules."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


W, H, HORIZON = 1024, 600, 291
TW, TH = 256, 96


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    state = json.loads(args.state.read_text())
    assert state["schema"] == "luminary-runtime-scene/v1"
    assert state["camera"] == {"width_px": W, "height_px": H, "horizon_y_px": HORIZON,
                                "bearing_deg": 90.0, "locked": True,
                                "vertical_motion": "forbidden"}
    assert [shell["name"] for shell in state["sky"]["shells"]] == ["low", "mid", "high"]
    assert state["sky"]["composite_order"] == ["high", "mid", "low"]
    assert state["sky"]["advance"].startswith("continuous")
    assert state["ocean"]["advance"].startswith("continuous")

    base = Image.open(args.assets / "nubble_runtime_base.jpg")
    assert base.size == (W, H)
    packed = np.fromfile(args.assets / "nubble_runtime_water_mask.bin", dtype=np.uint8)
    assert packed.size == W * H // 8
    water = np.unpackbits(packed, bitorder="little")[:W * H].reshape(H, W)
    assert not water[:HORIZON].any(), "water renderer may not touch sky pixels"
    shore = np.fromfile(args.assets / "nubble_runtime_shore_distance.bin", dtype=np.uint8)
    assert shore.size == W * H
    ocean_phase = np.fromfile(args.assets / "nubble_runtime_ocean_phase.bin", dtype=np.uint8)
    assert ocean_phase.size == (W // 2) * (H // 2) * 3
    ocean_phase = ocean_phase.reshape(H // 2, W // 2, 3)
    assert not ocean_phase[:HORIZON // 2].any(), "projected wave phase may not enter sky"
    for name in ("low", "mid", "high"):
        atlas = np.fromfile(args.assets / f"nubble_runtime_cloud_{name}.bin", dtype=np.uint8)
        assert atlas.size == TW * TH * 2
        atlas = atlas.reshape(TH, TW, 2)
        assert np.array_equal(atlas[:, 0], atlas[:, -1]), f"{name} longitude seam is not periodic"
    if float(state["sky"].get("observed_cloud_fraction") or 0) < .01:
        assert state["sky"].get("visibility_confidence", 0) >= .45, \
            "a clear override requires confident live-camera evidence"
    if args.preview:
        frames = sorted(args.preview.glob("frame-*.png"))
        assert len(frames) >= 3
        for frame in frames:
            assert Image.open(frame).size == (W, H)
    print("Production runtime OK: fixed 1024x600/y291, periodic 3-shell sky, masked continuous ocean")


if __name__ == "__main__":
    main()
