#!/usr/bin/env python3
"""Build a compact naked-eye star catalogue from ESA Hipparcos I/239.

Source fields are ICRS positions and proper motions at epoch J1991.25 plus
Johnson V magnitude. The generated fixed-point table is consumed directly by
the P4; no network astronomy service is needed after firmware is flashed.
"""

from __future__ import annotations

import argparse
import urllib.request
from pathlib import Path


CATALOG_URL = "https://cdsarc.cds.unistra.fr/ftp/I/239/hip_main.dat"


def number(field: str) -> float | None:
    field = field.strip()
    return float(field) if field else None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--magnitude", type=float, default=6.5)
    args = parser.parse_args()

    stars: list[tuple[float, int, int, int, int]] = []
    with urllib.request.urlopen(CATALOG_URL, timeout=120) as response:
        for raw in response:
            line = raw.decode("ascii", "strict")
            magnitude = number(line[41:46])
            ra = number(line[51:63])
            dec = number(line[64:76])
            pm_ra = number(line[87:95]) or 0.0
            pm_dec = number(line[96:104]) or 0.0
            if magnitude is None or ra is None or dec is None or magnitude > args.magnitude:
                continue
            stars.append((magnitude, round(ra * 1_000_000), round(dec * 1_000_000),
                          round(pm_ra), round(pm_dec)))
    stars.sort()
    lines = [
        "#pragma once",
        "/* Generated from ESA Hipparcos I/239 hip_main.dat; do not hand edit. */",
        "typedef struct {",
        "    int32_t ra_microdeg, dec_microdeg;",
        "    int16_t pm_ra_mas_year, pm_dec_mas_year;",
        "    int16_t vmag_centimag;",
        "} hipparcos_star_t;",
        "static const hipparcos_star_t HIPPARCOS_STARS[] = {",
    ]
    for magnitude, ra, dec, pm_ra, pm_dec in stars:
        lines.append(f"    {{{ra}, {dec}, {pm_ra}, {pm_dec}, {round(magnitude * 100)}}},")
    lines.extend([
        "};",
        f"#define HIPPARCOS_STAR_COUNT {len(stars)}U",
        "#define HIPPARCOS_EPOCH_YEAR 1991.25",
        f"#define HIPPARCOS_LIMITING_MAGNITUDE_CENTI {round(args.magnitude * 100)}",
        "",
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))
    print(f"wrote {len(stars)} stars to {args.output}")


if __name__ == "__main__":
    main()
