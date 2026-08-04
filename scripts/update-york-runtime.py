#!/usr/bin/env python3
"""Refresh, validate, pack, and optionally deploy the live Nubble runtime."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PYTHON = sys.executable


def run(*arguments: str) -> None:
    subprocess.run([PYTHON, *arguments], cwd=ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="192.168.10.35")
    parser.add_argument("--deploy", action="store_true")
    parser.add_argument("--reuse-dmw", action="store_true",
                        help="Use the last downloaded DMW file during offline testing")
    parser.add_argument("--camera-optional", action="store_true",
                        help="Fall back explicitly to meteorological cloud state if live video is unavailable")
    args = parser.parse_args()
    work = ROOT / "tmp" / "york-live"
    clouds = work / "goes-clouds"
    conditions = work / "conditions.json"
    camera = work / "camera-observation.json"
    display_state = work / "display-state.json"
    dmw_file = clouds / "latest-dmwf-c14.nc"
    dmw_state = clouds / "dmw-shell-winds.json"
    runtime_scene = (ROOT / "firmware/luminary-background-viewer/assets/v2/"
                     "nubble-runtime-scene-v1.json")
    runtime_assets = ROOT / "firmware/luminary-background-viewer/assets/runtime"
    work.mkdir(parents=True, exist_ok=True)
    clouds.mkdir(parents=True, exist_ok=True)

    with conditions.open("w") as output:
        subprocess.run([PYTHON, "scripts/fetch-york-conditions.py", "--pretty"],
                       cwd=ROOT, check=True, stdout=output)
    camera_available = True
    try:
        run("scripts/fetch-nubble-camera-observation.py", "--output", str(camera),
            "--snapshot", str(work / "camera-frame.jpg"))
    except subprocess.CalledProcessError:
        if not args.camera_optional:
            raise
        camera_available = False
        print("live Nubble video unavailable; using explicit meteorological fallback",
              file=sys.stderr)
    enrich = ["scripts/enrich-york-display-state.py", "--input", str(conditions),
              "--output", str(display_state)]
    if camera_available:
        enrich.extend(["--visual-observation", str(camera)])
    run(*enrich)
    if args.reuse_dmw:
        run("scripts/extract-goes-dmw-winds.py", str(dmw_file), "--output", str(dmw_state))
    else:
        run("scripts/extract-goes-dmw-winds.py", "--fetch-latest", str(dmw_file),
            "--output", str(dmw_state))
    observed = json.loads(display_state.read_text())
    run("scripts/build-goes-york-cloud-plane.py", "--output", str(clouds),
        "--shell-winds", str(dmw_state), "--frames", "3", "--loop-seconds", "60",
        "--wind-from-deg", str(observed.get("wind_direction_deg") or 270),
        "--wind-knots", str(observed.get("wind_knots") or 8))
    run("scripts/build-runtime-scene-state.py", "--conditions", str(display_state),
        "--clouds", str(clouds / "goes-cloud-plane.json"), "--output", str(runtime_scene))
    run("scripts/validate-runtime-scene.py", str(runtime_scene))
    run("firmware/luminary-background-viewer/tools/runtime_pack.py",
        "--frame", str(runtime_assets / "nubble_runtime_base.jpg"),
        "--water-mask", str(runtime_assets / "nubble_runtime_water_mask.bin"),
        "--shore-distance", str(runtime_assets / "nubble_runtime_shore_distance.bin"),
        "--cloud-dir", str(clouds), "--state", str(runtime_scene),
        "--output", str(runtime_assets))
    if args.deploy:
        run("scripts/deploy-p4-runtime.py", "--device", args.device,
            "--state", str(runtime_scene), "--assets", str(runtime_assets))
    else:
        print(f"validated runtime ready at {runtime_scene}; use --deploy to update the P4")


if __name__ == "__main__":
    main()
