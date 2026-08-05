#!/usr/bin/env python3
"""Atomically deploy measured Luminary runtime state to a local P4 viewer."""

from __future__ import annotations

import argparse
import json
import urllib.request
from pathlib import Path


def post(url: str, payload: bytes, content_type: str) -> None:
    request = urllib.request.Request(
        url, data=payload, method="POST",
        headers={"Content-Type": content_type, "User-Agent": "Luminary-runtime-deployer/1"})
    with urllib.request.urlopen(request, timeout=20) as response:
        if response.status != 200 or response.read().strip() != b"ok":
            raise RuntimeError(f"{url}: device rejected update ({response.status})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="192.168.10.35")
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True,
                        help="Directory produced by firmware tools/runtime_pack.py")
    args = parser.parse_args()
    state = json.loads(args.state.read_text())
    if state.get("camera", {}).get("horizon_y_px") != 291:
        raise ValueError("refusing deployment: runtime horizon is not row 291")
    base = f"http://{args.device}"
    # Upload far-to-near atlases first. State is the commit point: visible
    # cloud coverage changes only after all three measured textures are ready.
    for shell in ("high", "mid", "low"):
        payload = (args.assets / f"nubble_runtime_cloud_{shell}.bin").read_bytes()
        if len(payload) != 49152:
            raise ValueError(f"{shell}: expected 49152-byte cloud atlas")
        post(f"{base}/runtime/cloud/{shell}", payload, "application/octet-stream")
    ocean_phase = (args.assets / "nubble_runtime_ocean_phase.bin").read_bytes()
    if len(ocean_phase) != 512 * 300 * 3:
        raise ValueError("expected 460800-byte three-component ocean phase atlas")
    post(f"{base}/runtime/ocean-phase", ocean_phase, "application/octet-stream")
    post(f"{base}/runtime/state", args.state.read_bytes(), "application/json")
    print(f"deployed {state.get('updated_at')} to {base}; horizon=291")


if __name__ == "__main__":
    main()
