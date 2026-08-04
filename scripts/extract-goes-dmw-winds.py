#!/usr/bin/env python3
"""Extract York-local low/mid/high cloud winds from a GOES DMW NetCDF file."""

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


NOAA_BUCKET = "https://noaa-goes19.s3.amazonaws.com"
DMW_PREFIX = "ABI-L2-DMWF"
NS = {"s3": "http://s3.amazonaws.com/doc/2006-03-01/"}


def observation_time(key: str) -> dt.datetime:
    match = re.search(r"_s(\d{4})(\d{3})(\d{2})(\d{2})(\d{2})", key)
    if not match:
        raise ValueError(f"could not parse GOES time from {key}")
    year, day, hour, minute, second = map(int, match.groups())
    return dt.datetime(year, 1, 1, tzinfo=dt.timezone.utc) + dt.timedelta(
        days=day - 1, hours=hour, minutes=minute, seconds=second)


def fetch_latest(destination: Path) -> Path:
    now = dt.datetime.now(dt.timezone.utc)
    keys: list[str] = []
    for hour_back in range(4):
        when = now - dt.timedelta(hours=hour_back)
        prefix = f"{DMW_PREFIX}/{when:%Y}/{when:%j}/{when:%H}/"
        url = f"{NOAA_BUCKET}/?list-type=2&prefix={prefix}&max-keys=1000"
        request = urllib.request.Request(url, headers={"User-Agent": "Luminary/0.1 contact@castalia.institute"})
        with urllib.request.urlopen(request, timeout=60) as response:
            root = ET.fromstring(response.read())
        keys.extend(node.text for node in root.findall("s3:Contents/s3:Key", NS)
                    if node.text and "C14_" in node.text)
    if not keys:
        raise RuntimeError("NOAA GOES-19 DMW C14 has no recent file")
    key = max(set(keys), key=observation_time)
    destination.parent.mkdir(parents=True, exist_ok=True)
    urllib.request.urlretrieve(f"{NOAA_BUCKET}/{key}", destination)
    return destination


def dataset(path: Path, name: str, dtype: str) -> np.ndarray:
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as temp:
        raw = Path(temp.name)
    try:
        subprocess.run(["h5dump", "-d", f"/{name}", "-o", str(raw), "-b", "LE", str(path)],
                       check=True, capture_output=True)
        return np.fromfile(raw, dtype=dtype)
    finally:
        raw.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("--fetch-latest", type=Path, metavar="DESTINATION",
                        help="Download the latest GOES-19 DMW C14 file before extracting")
    parser.add_argument("--latitude", type=float, default=43.1637)
    parser.add_argument("--longitude", type=float, default=-70.6480)
    parser.add_argument("--radius-km", type=float, default=650.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.fetch_latest:
        args.source = fetch_latest(args.fetch_latest)
    if not args.source:
        parser.error("provide SOURCE or --fetch-latest DESTINATION")

    lat = dataset(args.source, "lat", "<f8")
    lon = dataset(args.source, "lon", "<f8")
    pressure = dataset(args.source, "pressure", "<f4")
    speed = dataset(args.source, "wind_speed", "<f4")
    direction = dataset(args.source, "wind_direction", "<f4")
    dqf = dataset(args.source, "DQF", "i1")
    count = min(map(len, (lat, lon, pressure, speed, direction, dqf)))
    lat, lon, pressure, speed, direction, dqf = [v[:count] for v in (lat, lon, pressure, speed, direction, dqf)]
    dx = (lon - args.longitude) * 111.32 * math.cos(math.radians(args.latitude))
    dy = (lat - args.latitude) * 110.54
    distance = np.hypot(dx, dy)
    valid = (np.isfinite(lat) & np.isfinite(lon) & np.isfinite(pressure) &
             np.isfinite(speed) & np.isfinite(direction) & (distance <= args.radius_km) &
             (speed >= 0) & (speed < 100) & (pressure > 50) & (pressure < 1100) & (dqf == 0))
    layers = {"low": (680.0, 1100.0), "mid": (440.0, 680.0), "high": (50.0, 440.0)}
    result = {"schema": "luminary-goes-dmw-shell-winds/v1", "source": str(args.source),
              "location": {"latitude": args.latitude, "longitude": args.longitude}, "shells": {}}
    for name, (pmin, pmax) in layers.items():
        choose = valid & (pressure >= pmin) & (pressure < pmax)
        if not choose.any():
            result["shells"][name] = {"available": False, "vector_count": 0}
            continue
        weights = np.exp(-np.square(distance[choose] / 350.0))
        radians = np.radians(direction[choose])
        # Meteorological direction is where wind comes from.
        east = np.average(-speed[choose] * np.sin(radians), weights=weights)
        north = np.average(-speed[choose] * np.cos(radians), weights=weights)
        mean_speed = math.hypot(east, north)
        from_deg = (math.degrees(math.atan2(-east, -north)) + 360.0) % 360.0
        result["shells"][name] = {"available": True, "vector_count": int(choose.sum()),
                                   "wind_from_deg": round(from_deg, 2),
                                   "wind_speed_mps": round(mean_speed, 3),
                                   "wind_knots": round(mean_speed / 0.514444, 2),
                                   "pressure_range_hpa": [pmin, pmax]}
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
