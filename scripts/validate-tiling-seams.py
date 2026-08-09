#!/usr/bin/env python3
"""Detect unnatural straight seams and periodic tiling in a Luminary frame.

Real sky and sea are smooth or organically textured: cloud edges curve, ripples
scatter in orientation, and neither repeats on a fixed pitch. The rendering bugs
we keep hitting break exactly those properties -- a low-resolution field sampled
coarsely leaves the signature of its lattice:

  * long, straight, often parallel/regularly-spaced SEAMS (the projected solver
    grid in the water; the cloud-atlas texel boundaries in the sky), and
  * a sharp PERIODIC peak in the spatial spectrum at the tile pitch.

This validator isolates the sky and sea (by position + colour, so rocks, island
and lighthouse are excluded -- they have legitimate straight edges) and scores
each on both signatures. It needs no OpenCV; a Hough accumulator and a windowed
2-D FFT are done in numpy.

    scripts/validate-tiling-seams.py frame.png
    scripts/validate-tiling-seams.py frame.png --rotate 90 --debug out.png

Exit code is non-zero if either region fails, so it can gate CI or a deploy.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image


# ---- region isolation -------------------------------------------------------

def blueness(rgb: np.ndarray) -> np.ndarray:
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    return b.astype(float) - 0.5 * (r.astype(float) + g.astype(float))


def find_horizon(rgb: np.ndarray) -> int:
    """Row where the scene transitions from sky to sea/land: the sky above is
    uniformly bright and blue, so its per-row blueness variance is tiny and its
    brightness high. Take the lowest such row as the horizon."""
    h = rgb.shape[0]
    bright = rgb.mean(axis=2).mean(axis=1)
    bvar = blueness(rgb).std(axis=1)
    skyish = (bright > 150) & (bvar < 18)
    rows = np.where(skyish)[0]
    if rows.size == 0:
        return h // 3
    # horizon = last mostly-sky row in the top half
    top = rows[rows < h * 0.6]
    return int(top.max()) if top.size else h // 3


def erode(mask: np.ndarray, n: int) -> np.ndarray:
    """Binary erosion by n px (4-connected), so a region's own boundary edge --
    horizon, shoreline, rock rim -- never counts as a seam."""
    m = mask.copy()
    for _ in range(n):
        e = m.copy()
        e[1:, :] &= m[:-1, :]; e[:-1, :] &= m[1:, :]
        e[:, 1:] &= m[:, :-1]; e[:, :-1] &= m[:, 1:]
        m = e
    return m


def load_bitmask(path: Path, w: int, h: int) -> np.ndarray:
    bits = np.unpackbits(np.fromfile(path, dtype=np.uint8), bitorder="little")
    return bits[: w * h].reshape(h, w).astype(bool)


def region_masks(rgb: np.ndarray, horizon: int,
                 water_mask: np.ndarray | None = None,
                 land_mask: np.ndarray | None = None):
    """Boolean masks for the smooth-sky and open-water regions, each excluding
    land, rock, foam and structures so only surfaces that SHOULD be seamless are
    scored. With the render's own water/land masks (native mode) the regions are
    exact; otherwise they are recovered from colour + position."""
    h, w = rgb.shape[:2]
    bright = rgb.mean(axis=2)
    blue = blueness(rgb)
    yy = np.arange(h)[:, None]

    if water_mask is not None and land_mask is not None:
        sea = erode(water_mask & (bright < 215), 12)   # drop foam, shrink off rims
        sky = erode((yy < horizon) & ~land_mask, 12)
        return sky, sea

    sky = erode((yy < horizon) & (bright > 140) & (blue > 8), 6)
    # Sea: blue-dominant water only. blueness > 55 cleanly separates water from
    # wet rock (measured ~35-49 on rock vs ~64 on water); drop bright foam.
    sea = erode((yy >= horizon) & (blue > 55) & (bright < 210), 6)
    return sky, sea


# ---- shared prep ------------------------------------------------------------

def box_blur(a: np.ndarray, radius: int) -> np.ndarray:
    """Separable box blur (edge-padded moving average); cheap large-kernel
    low-pass used to build the high-pass."""
    def blur1d(x, r, axis):
        n = x.shape[axis]
        pad = [(0, 0)] * x.ndim
        pad[axis] = (r, r)
        cp = np.cumsum(np.pad(x, pad, mode="edge"), axis=axis)
        lo = np.take(cp, range(0, n), axis=axis)
        hi = np.take(cp, range(2 * r, n + 2 * r), axis=axis)
        return (hi - lo) / (2 * r + 1)
    return blur1d(blur1d(a, radius, 0), radius, 1)


def highpass(gray: np.ndarray, radius: int = 12) -> np.ndarray:
    return gray - box_blur(gray, radius)


def sobel_mag(a: np.ndarray):
    gx = np.zeros_like(a); gy = np.zeros_like(a)
    gx[:, 1:-1] = a[:, 2:] - a[:, :-2]
    gy[1:-1, :] = a[2:, :] - a[:-2, :]
    return np.hypot(gx, gy), gx, gy


# ---- seam detector (numpy Hough) -------------------------------------------

def seam_scan(gray: np.ndarray, mask: np.ndarray):
    """Straightness signature. Strong edges inside a should-be-smooth region are
    Hough-accumulated over (theta, rho); a genuine seam dumps huge collinear
    support into one bin, and a tiling grid lights a row of regularly spaced rho
    bins at one theta. Returns a dict with an interpretable score."""
    hp = highpass(gray)
    mag, gx, gy = sobel_mag(hp)
    m = mask & np.isfinite(mag)
    if m.sum() < 500:
        return {"score": 0.0, "seam_len": 0, "n_parallel": 0, "theta_deg": 0,
                "lines": [], "note": "region too small"}

    thr = np.percentile(mag[m], 98.0)
    ys, xs = np.where(m & (mag >= thr))
    if ys.size < 40:
        return {"score": 0.0, "seam_len": 0, "n_parallel": 0, "theta_deg": 0,
                "lines": []}
    # cap for speed
    if ys.size > 6000:
        idx = np.linspace(0, ys.size - 1, 6000).astype(int)
        ys, xs = ys[idx], xs[idx]

    thetas = np.deg2rad(np.arange(0, 180, 1.0))
    cos, sin = np.cos(thetas), np.sin(thetas)
    rho = xs[:, None] * cos[None, :] + ys[:, None] * sin[None, :]  # (N,180)
    rho_bin = 3.0
    diag = float(np.hypot(*gray.shape))
    nb = int(2 * diag / rho_bin) + 1
    off = diag
    ri = ((rho + off) / rho_bin).astype(int)
    ri = np.clip(ri, 0, nb - 1)

    N = ys.size
    acc = np.zeros((180, nb), dtype=np.int32)
    for t in range(180):
        np.add.at(acc[t], ri[:, t], 1)

    # expected count per bin if edges were random at this theta
    spread = (ri.max(axis=0) - ri.min(axis=0) + 1).clip(1)      # bins spanned
    expected = N / spread                                        # per-theta mean
    peak_per_theta = acc.max(axis=1)
    ratio = peak_per_theta / (expected + 1e-6)
    t_best = int(np.argmax(ratio))
    seam_len = int(peak_per_theta[t_best])
    score = float(ratio[t_best])

    # parallel/grid check at the dominant orientation
    row = acc[t_best]
    strong = np.where(row > 0.4 * seam_len)[0]
    n_parallel = int(strong.size)
    spacing = 0
    if strong.size >= 2:
        d = np.diff(np.sort(strong)) * rho_bin
        d = d[d > 4]
        spacing = int(np.median(d)) if d.size else 0

    # top lines for debug overlay
    flat = acc.ravel()
    top = np.argsort(flat)[-6:][::-1]
    lines = []
    for f in top:
        if flat[f] < 0.5 * seam_len:
            continue
        t, b = divmod(int(f), nb)
        lines.append((float(np.deg2rad(t)), (b * rho_bin) - off, int(flat[f])))

    return {"score": score, "seam_len": seam_len, "n_parallel": n_parallel,
            "spacing_px": spacing, "theta_deg": t_best, "lines": lines}


# ---- periodicity detector (FFT) --------------------------------------------

def _shift_corr(H: np.ndarray, M: np.ndarray, dy: int, dx: int) -> float:
    """Normalised correlation of the high-passed field with itself shifted by
    (dy,dx), over the pixels where both copies are inside the region mask."""
    h, w = H.shape
    ay0, ay1 = max(0, dy), min(h, h + dy)
    ax0, ax1 = max(0, dx), min(w, w + dx)
    A = H[ay0:ay1, ax0:ax1]; MA = M[ay0:ay1, ax0:ax1]
    B = H[ay0 - dy:ay1 - dy, ax0 - dx:ax1 - dx]
    MB = M[ay0 - dy:ay1 - dy, ax0 - dx:ax1 - dx]
    m = MA & MB
    if m.sum() < 2000:
        return 0.0
    a = A[m]; b = B[m]
    denom = np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9
    return float((a * b).sum() / denom)


def axis_seam_scan(gray: np.ndarray, mask: np.ndarray):
    """Axis-aligned seam signature: a vertical seam is a column across which the
    image jumps CONSISTENTLY over many rows (and likewise a horizontal seam over
    columns). For each adjacent column pair, average |I(x+1)-I(x)| over the
    masked rows, then subtract a smooth baseline; a seam spikes one column well
    above its neighbours, while organic texture keeps the cross-column gradient
    roughly uniform. Returns the strongest vertical and horizontal seam spikes
    (grey levels) and where they sit."""
    def scan_axis(axis: int):
        g = gray if axis == 1 else gray.T
        m = mask if axis == 1 else mask.T
        both = m[:, 1:] & m[:, :-1]
        diff = np.abs(g[:, 1:] - g[:, :-1]) * both
        cnt = both.sum(axis=0)
        prof = np.where(cnt > 40, diff.sum(axis=0) / np.maximum(cnt, 1), np.nan)
        prof = np.nan_to_num(prof, nan=np.nanmedian(prof) if np.isfinite(prof).any() else 0.0)
        k = 12
        base = np.array([np.median(prof[max(0, i - k):i + k + 1]) for i in range(prof.size)])
        resid = prof - base
        # Trim the region's own extent boundaries: the first/last valid rows or
        # columns sit against the horizon or shoreline, whose real gradient is
        # not a seam.
        valid = np.where(cnt > 40)[0]
        if valid.size:
            for i in range(resid.size):
                if i < valid.min() + 8 or i > valid.max() - 8:
                    resid[i] = 0.0
        idx = int(np.argmax(resid))
        return float(resid[idx]), idx
    v_spike, v_at = scan_axis(1)   # vertical seams: jumps across columns
    h_spike, h_at = scan_axis(0)   # horizontal seams: jumps across rows
    return {"v_spike": v_spike, "v_at": v_at, "h_spike": h_spike, "h_at": h_at}


def periodicity_scan(gray: np.ndarray, mask: np.ndarray):
    """Periodicity signature via directional shift-correlation. A tiling repeats
    on a fixed pitch, so the high-passed region correlates strongly with itself
    when shifted by the tile vector -- a bump standing PROUD of the smooth decay
    seen in organic ocean/cloud texture. Scanning four orientations (H, V, and
    both diagonals) over a range of pitches and taking the most prominent bump
    is directly sensitive to seams at any angle and does not explode on
    near-flat regions. Pitches <= 6 px are skipped: the renderer supersamples in
    2x2 blocks (px = x>>1, alternate rows), inherent and invisible at 1x."""
    ys, xs = np.where(mask)
    if ys.size < 4000:
        return {"score": 0.0, "pitch_px": 0, "angle_deg": 0, "note": "small"}
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    H = highpass(gray[y0:y1, x0:x1].astype(float), 12)
    M = mask[y0:y1, x0:x1]
    H = H * M

    dirs = {0: (0, 1), 90: (1, 0), 45: (1, 1), 135: (1, -1)}
    pitches = list(range(4, 81))
    best = {"score": 0.0, "pitch_px": 0, "angle_deg": 0}
    for ang, (uy, ux) in dirs.items():
        corr = np.array([_shift_corr(H, M, p * uy, p * ux) for p in pitches])
        for i, p in enumerate(pitches):
            if p < 7:
                continue
            lo = max(0, i - 4); hi = min(len(pitches), i + 5)
            baseline = np.median(np.concatenate([corr[lo:i], corr[i + 1:hi]])) \
                if hi - lo > 1 else 0.0
            prom = corr[i] - baseline           # bump above local decay
            if prom > best["score"]:
                best = {"score": float(prom), "pitch_px": p, "angle_deg": ang}
    return best


# ---- driver -----------------------------------------------------------------

# Thresholds calibrated on native renders (fixed vs. grid-injected / reverted).
SEAM_FAIL = 30.0      # collinear support ratio; only egregious straight seams
PERIOD_FAIL = 0.06    # shift-correlation side-lobe prominence
AXIS_SEAM_FAIL = 4.0  # grey-level spike of a consistent column/row seam


def verdict(region: str, seam: dict, per: dict, axis: dict) -> tuple[bool, str]:
    reasons = []
    if seam["score"] >= SEAM_FAIL:
        reasons.append(f"straight seam (support {seam['score']:.0f}x, "
                       f"{seam['seam_len']} px collinear @ {seam['theta_deg']}deg)")
    if per["score"] >= PERIOD_FAIL:
        reasons.append(f"periodic tiling (side-lobe {per['score']:.3f} @ "
                       f"~{per['pitch_px']} px pitch)")
    if axis["v_spike"] >= AXIS_SEAM_FAIL:
        reasons.append(f"vertical seam ({axis['v_spike']:.1f} grey @ x={axis['v_at']})")
    # Horizontal seams are only meaningful in the sky: in the sea a horizontal
    # transition is the horizon (and swell bands), which is natural, not a seam.
    if region == "SKY" and axis["h_spike"] >= AXIS_SEAM_FAIL:
        reasons.append(f"horizontal seam ({axis['h_spike']:.1f} grey @ y={axis['h_at']})")
    return (len(reasons) == 0, "; ".join(reasons) if reasons else "clean")


def draw_lines(img: Image.Image, lines, mask, color):
    a = np.array(img)
    h, w = a.shape[:2]
    xs = np.arange(w)
    for theta, rho, _ in lines:
        s = np.sin(theta)
        if abs(s) < 1e-3:
            continue
        ys = ((rho - xs * np.cos(theta)) / s).astype(int)
        ok = (ys >= 0) & (ys < h)
        a[ys[ok], xs[ok]] = color
    return Image.fromarray(a)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("frame", type=Path)
    ap.add_argument("--rotate", type=int, default=0,
                    help="rotate CCW degrees first (device screenshots need 90)")
    ap.add_argument("--horizon", type=int, default=-1)
    ap.add_argument("--water-mask", type=Path, default=None,
                    help="native mode: 1-bit LSB water mask (exact sea region)")
    ap.add_argument("--land-mask", type=Path, default=None,
                    help="native mode: 1-bit LSB land mask (exact sky region)")
    ap.add_argument("--debug", type=Path, default=None)
    args = ap.parse_args()

    im = Image.open(args.frame).convert("RGB")
    if args.rotate:
        im = im.rotate(args.rotate, expand=True)
    rgb = np.asarray(im)
    gray = rgb.mean(axis=2)

    h, w = rgb.shape[:2]
    wm = load_bitmask(args.water_mask, w, h) if args.water_mask else None
    lm = load_bitmask(args.land_mask, w, h) if args.land_mask else None
    native = wm is not None and lm is not None
    horizon = args.horizon if args.horizon >= 0 else (582 if native else find_horizon(rgb))
    sky, sea = region_masks(rgb, horizon, wm, lm)

    print(f"frame {args.frame.name}  {rgb.shape[1]}x{rgb.shape[0]}  "
          f"horizon={horizon}  sky_px={int(sky.sum())}  sea_px={int(sea.sum())}")
    ok_all = True
    dbg = im.copy()
    for name, mask, col in (("SKY", sky, [255, 60, 60]), ("SEA", sea, [60, 255, 90])):
        seam = seam_scan(gray, mask)
        per = periodicity_scan(gray, mask)
        axis = axis_seam_scan(gray, mask)
        ok, why = verdict(name, seam, per, axis)
        ok_all &= ok
        print(f"  {name:3s} {'PASS' if ok else 'FAIL'}  "
              f"seam={seam['score']:5.1f}x @ {seam['theta_deg']:3d}deg  "
              f"period={per['score']:.3f}@{per.get('pitch_px',0)}px  "
              f"vseam={axis['v_spike']:4.1f} hseam={axis['h_spike']:4.1f}  "
              f"-> {why}")
        if args.debug and seam.get("lines"):
            dbg = draw_lines(dbg, seam["lines"], mask, col)
    if args.debug:
        dbg.save(args.debug)
        print(f"  debug overlay -> {args.debug}")
    print("VERDICT:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
