#!/usr/bin/env python3
"""Publish a checksummed, cache-busted runtime bundle for autonomous P4 clients."""

from __future__ import annotations

import argparse
import hashlib
import json
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STATE = ROOT / "firmware/luminary-background-viewer/assets/v2/nubble-runtime-scene-v1.json"
DEFAULT_ASSETS = ROOT / "firmware/luminary-background-viewer/assets/runtime"
DEFAULT_OUTPUT = ROOT / "site/runtime/v1"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path, default=DEFAULT_STATE)
    parser.add_argument("--assets", type=Path, default=DEFAULT_ASSETS)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    scene = json.loads(args.state.read_text())
    if scene.get("camera", {}).get("horizon_y_px") != 291:
        raise ValueError("refusing to publish a scene whose horizon is not row 291")
    sources = {
        "cloud_high": args.assets / "nubble_runtime_cloud_high.bin",
        "cloud_mid": args.assets / "nubble_runtime_cloud_mid.bin",
        "cloud_low": args.assets / "nubble_runtime_cloud_low.bin",
        "ocean_phase": args.assets / "nubble_runtime_ocean_phase.bin",
        "state": args.state,
    }
    expected = {"cloud_high": 49152, "cloud_mid": 49152, "cloud_low": 49152,
                "ocean_phase": 512 * 300 * 3}
    payloads: dict[str, bytes] = {}
    for name, source in sources.items():
        payload = source.read_bytes()
        if name in expected and len(payload) != expected[name]:
            raise ValueError(f"{name}: expected {expected[name]} bytes, got {len(payload)}")
        if name == "state" and len(payload) > 8192:
            raise ValueError("runtime state exceeds the P4's 8192-byte limit")
        payloads[name] = payload

    bundle_hash = hashlib.sha256()
    for name in sorted(payloads):
        bundle_hash.update(name.encode())
        bundle_hash.update(payloads[name])
    bundle_id = bundle_hash.hexdigest()[:20]

    args.output.mkdir(parents=True, exist_ok=True)
    for old in args.output.iterdir():
        if old.is_file() and old.name != ".gitkeep":
            old.unlink()
    assets: dict[str, dict] = {}
    for name, payload in payloads.items():
        suffix = ".json" if name == "state" else ".bin"
        filename = f"{name.replace('_', '-')}-{bundle_id}{suffix}"
        destination = args.output / filename
        destination.write_bytes(payload)
        assets[name] = {
            "path": filename,
            "bytes": len(payload),
            "crc32": f"{zlib.crc32(payload) & 0xffffffff:08x}",
            "sha256": hashlib.sha256(payload).hexdigest(),
        }
    manifest = {
        "schema": "luminary-runtime-bundle/v1",
        "bundle_id": bundle_id,
        "updated_at": scene.get("updated_at"),
        "camera": {"width_px": 1024, "height_px": 600, "horizon_y_px": 291},
        "assets": assets,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"published runtime bundle {bundle_id} to {args.output}")


if __name__ == "__main__":
    main()
