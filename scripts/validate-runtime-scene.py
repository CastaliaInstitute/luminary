#!/usr/bin/env python3
"""Reject runtime scene states that can break Nubble registration or continuity."""

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", type=Path)
    args = parser.parse_args()
    state = json.loads(args.scene.read_text())
    assert state["schema"] == "luminary-runtime-scene/v1"
    camera = state["camera"]
    assert (camera["width_px"], camera["height_px"], camera["horizon_y_px"]) == (1024, 600, 291)
    assert camera["locked"] is True and camera["vertical_motion"] == "forbidden"
    shells = state["sky"]["shells"]
    assert [shell["name"] for shell in shells] == ["low", "mid", "high"]
    assert state["sky"]["composite_order"] == ["high", "mid", "low"]
    for shell in shells:
        assert shell["projection_height_m"] > 0
        assert shell["wind_speed_mps"] >= 0
        assert "GOES" in shell["wind_source"] or "fallback" in shell["wind_source"]
    ocean = state["ocean"]
    assert ocean["dominant_period_s"] > 0 and ocean["dominant_wavelength_m"] > 0
    components = ocean["components"]
    assert 1 <= len(components) <= 3
    for component in components:
        assert component["height_m"] >= 0 and component["period_s"] > 0
        assert component["wavelength_m"] > 0 and 0 <= component["wave_from_deg"] < 360
    assert ocean["advance"] == "continuous spectral phase; no finite loop"
    assert state["sky"]["advance"] == "continuous monotonic time; never reset at asset boundary"
    print(f"Runtime scene OK: 1024x600, horizon 291, {len(shells)} measured cloud shells, "
          f"{len(components)} measured wave components")


if __name__ == "__main__":
    main()
