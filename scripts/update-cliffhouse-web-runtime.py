#!/usr/bin/env python3
"""Build the live Cliff House web runtime from the current York observations.

The Nubble and Cliff House views share the same GOES downloads and measured
height-resolved winds. This script reprojects that measured atmosphere through
the Cliff House camera and publishes only the compact browser assets.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PYTHON = sys.executable
CONFIG = ROOT / "config/cliff-house-conditions.json"
YORK_CLOUDS = ROOT / "tmp/york-live/goes-clouds"
OUTPUT = ROOT / "site/cliffhouse/runtime"
WORK = ROOT / "tmp/cliffhouse-live"


def run(*arguments: str) -> None:
    subprocess.run([PYTHON, *arguments], cwd=ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--loop-seconds", type=float, default=120.0)
    args = parser.parse_args()

    config = json.loads(CONFIG.read_text())
    camera = config["camera"]
    source_manifest = json.loads((YORK_CLOUDS / "goes-cloud-plane.json").read_text())
    source_files = [YORK_CLOUDS / Path(key).name for key in source_manifest["source_keys"]]
    height_file = YORK_CLOUDS / Path(source_manifest["cloud_top_height_source"]).name
    winds = YORK_CLOUDS / "dmw-shell-winds.json"
    required = [*source_files, height_file, winds]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise FileNotFoundError("Missing shared York runtime inputs: " + ", ".join(missing))

    WORK.mkdir(parents=True, exist_ok=True)
    conditions = WORK / "conditions.json"
    with conditions.open("w") as destination:
        subprocess.run(
            [PYTHON, "scripts/fetch-york-conditions.py", "--config", str(CONFIG), "--pretty"],
            cwd=ROOT, check=True, stdout=destination,
        )
    observed = json.loads(conditions.read_text())
    clouds = WORK / "clouds"
    clouds.mkdir(parents=True, exist_ok=True)
    run(
        "scripts/build-goes-york-cloud-plane.py",
        "--output", str(clouds),
        "--latitude", str(camera["latitude"]),
        "--longitude", str(camera["longitude"]),
        "--bearing-deg", str(camera["bearing_deg_true"]),
        "--source", *(str(path) for path in source_files),
        "--height-source", str(height_file),
        "--shell-winds", str(winds),
        "--wind-from-deg", str(observed.get("wind_direction_deg") or 270),
        "--wind-knots", str(observed.get("wind_knots") or 8),
        "--frames", str(args.frames),
        "--loop-seconds", str(args.loop_seconds),
    )

    cloud_manifest_path = clouds / "goes-cloud-plane.json"
    cloud_manifest = json.loads(cloud_manifest_path.read_text())
    # build-goes receives local files here so it cannot infer their original
    # observation timestamps. Preserve the authoritative metadata from the
    # shared York download for cache versioning and freshness diagnostics.
    cloud_manifest["source_keys"] = source_manifest["source_keys"]
    cloud_manifest["source_times_utc"] = source_manifest["source_times_utc"]
    cloud_manifest["cloud_top_height_source"] = source_manifest["cloud_top_height_source"]
    cloud_manifest_path.write_text(json.dumps(cloud_manifest, indent=2) + "\n")

    staged = WORK / "published"
    if staged.exists():
        shutil.rmtree(staged)
    (staged / "clouds").mkdir(parents=True)
    shutil.copy2(conditions, staged / "conditions.json")
    shutil.copy2(cloud_manifest_path, staged / "clouds/goes-cloud-plane.json")
    for index in range(args.frames):
        name = f"cloud-frame-{index:03d}.png"
        shutil.copy2(clouds / name, staged / "clouds" / name)

    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    shutil.copytree(staged, OUTPUT)
    print(f"Published {args.frames} current Cliff House cloud frames to {OUTPUT}")


if __name__ == "__main__":
    main()
