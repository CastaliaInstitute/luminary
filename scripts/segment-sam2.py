#!/usr/bin/env python3
"""Create source-faithful, prompt-guided masks with a local SAM 2 model.

The input JSON stores positive and negative image-space points.  Keeping the
points alongside a scene makes each segmentation inspectable and repeatable;
it is deliberately not a colour-threshold workflow.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import numpy as np
import torch
from sam2.build_sam import build_sam2
from sam2.sam2_image_predictor import SAM2ImagePredictor


ROOT = Path(__file__).resolve().parents[1]
# build_sam2 resolves this through the installed sam2 package; it must not be
# an absolute filesystem path.
DEFAULT_CONFIG = "configs/sam2.1/sam2.1_hiera_t.yaml"
DEFAULT_CHECKPOINT = "/tmp/luminary-sam2/checkpoints/sam2.1_hiera_tiny.pt"


def render_review(image: np.ndarray, mask: np.ndarray, points: np.ndarray,
                  labels: np.ndarray, destination: Path) -> None:
    review = image.copy()
    tint = np.zeros_like(review)
    tint[:, :, 1] = 220
    review = np.where(mask[..., None], (0.45 * review + 0.55 * tint).astype(np.uint8), review)
    for (x, y), label in zip(points.astype(int), labels):
        colour = (0, 220, 0) if label else (220, 0, 0)
        cv2.circle(review, (x, y), 8, colour, -1)
        cv2.circle(review, (x, y), 11, (255, 255, 255), 2)
    cv2.imwrite(str(destination), cv2.cvtColor(review, cv2.COLOR_RGB2BGR))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("scene", help="Scene directory under scenes/")
    parser.add_argument("layer", help="Named layer in segmentation-prompts.json")
    parser.add_argument("--config", default=DEFAULT_CONFIG)
    parser.add_argument("--checkpoint", default=DEFAULT_CHECKPOINT)
    args = parser.parse_args()

    scene = ROOT / "scenes" / args.scene
    prompts = json.loads((scene / "segmentation-prompts.json").read_text())
    spec = prompts["layers"][args.layer]
    image = cv2.cvtColor(cv2.imread(str(scene / prompts.get("source", "source.png"))), cv2.COLOR_BGR2RGB)
    points = np.asarray(spec["positive"] + spec.get("negative", []), dtype=np.float32)
    labels = np.asarray([1] * len(spec["positive"]) + [0] * len(spec.get("negative", [])), dtype=np.int32)

    device = "mps" if torch.backends.mps.is_available() else "cpu"
    model = build_sam2(args.config, args.checkpoint, device=device)
    predictor = SAM2ImagePredictor(model)
    predictor.set_image(image)
    masks, scores, _ = predictor.predict(point_coords=points, point_labels=labels, multimask_output=True)
    best = masks[int(np.argmax(scores))]

    output = scene / spec["output"]
    review = scene / spec["review"]
    cv2.imwrite(str(output), (best.astype(np.uint8) * 255))
    render_review(image, best, points, labels, review)
    print(json.dumps({"output": str(output), "review": str(review), "score": float(scores.max()), "device": device}))


if __name__ == "__main__":
    main()
