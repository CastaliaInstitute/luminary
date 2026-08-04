#!/usr/bin/env python3
"""Reduce the live Nubble camera to display-safe visual calibration values.

The camera is used only as color/coverage ground truth. GOES remains the cloud
geometry source and the registered Luminary horizon remains fixed at row 291.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from PIL import Image


DEFAULT_URL = "https://www.youtube.com/live/M6LiCaJKfMA"


def capture(url: str, destination: Path) -> None:
    if not shutil.which("yt-dlp") or not shutil.which("ffmpeg"):
        raise RuntimeError("live capture requires yt-dlp and ffmpeg")
    stream = subprocess.run(
        ["yt-dlp", "--no-warnings", "--get-url", "-f", "best[height<=720]", url],
        check=True, capture_output=True, text=True,
    ).stdout.splitlines()[0]
    subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-y", "-i", stream, "-frames:v", "1", str(destination)],
        check=True,
    )


def analyze(path: Path, source_url: str) -> dict:
    image = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    height, width = image.shape[:2]
    # The live camera's upper-center field is sky in every useful daytime
    # composition. Avoid the left utility poles and lower island/parking area.
    roi_bounds = (round(width * 0.22), round(height * 0.025),
                  round(width * 0.94), round(height * 0.30))
    x0, y0, x1, y1 = roi_bounds
    roi = image[y0:y1, x0:x1]
    maximum = roi.max(axis=2)
    minimum = roi.min(axis=2)
    saturation = (maximum - minimum) / np.maximum(maximum, 1.0)
    brightness = roi.mean(axis=2)
    blue = (roi[..., 2] > roi[..., 0] * 1.025) & (roi[..., 2] >= roi[..., 1] * 0.96)
    valid = brightness > 55
    blue_valid = blue & valid
    palette_pixels = roi[blue_valid] if blue_valid.any() else roi[valid]
    sky_rgb = np.percentile(palette_pixels, 58, axis=0)
    # Bright, weakly saturated pixels are visible clouds/haze. This estimate
    # calibrates GOES opacity; it does not replace the satellite cloud mask.
    cloud = valid & (saturation < 0.13) & (brightness > 145)
    cloud_fraction = float(np.clip(cloud.sum() / max(valid.sum(), 1), 0.0, 1.0))
    state = ("clear" if cloud_fraction < 0.10 else "few" if cloud_fraction < 0.30
             else "broken" if cloud_fraction < 0.72 else "overcast")
    blue_fraction = float(blue_valid.sum() / max(valid.sum(), 1))
    confidence = float(np.clip(0.35 + blue_fraction * 0.85, 0.0, 1.0))
    return {
        "schema": "luminary-nubble-camera-observation/v1",
        "observed_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "source": source_url,
        "role": "visual palette and cloud-coverage calibration only",
        "sky_rgb": [round(float(channel)) for channel in sky_rgb],
        "sky_state": state,
        "cloud_fraction": round(cloud_fraction, 3),
        "blue_fraction": round(blue_fraction, 3),
        "confidence": round(confidence, 3),
        "roi_px": {"x0": x0, "y0": y0, "x1": x1, "y1": y1},
        "registration_rule": "never infer or move the Luminary horizon from this camera",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument("--input", type=Path, help="Analyze a local camera frame instead of fetching")
    parser.add_argument("--snapshot", type=Path, help="Retain the fetched frame for visual QA")
    parser.add_argument("--output", type=Path, help="Defaults to stdout")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="luminary-camera-") as directory:
        frame = args.input or Path(directory) / "nubble-live.jpg"
        if args.input is None:
            capture(args.url, frame)
        observation = analyze(frame, args.url)
        if args.snapshot:
            args.snapshot.parent.mkdir(parents=True, exist_ok=True)
            Image.open(frame).convert("RGB").save(args.snapshot, quality=92)
    encoded = json.dumps(observation, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
