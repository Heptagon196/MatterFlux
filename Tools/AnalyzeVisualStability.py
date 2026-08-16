"""Compare MatterFlux fixed-camera screenshot sequences.

Requires Pillow and NumPy. Pass one or more sequence directories. The report
focuses on stable scene regions as well as the intentionally dynamic river.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image


REGIONS = {
    "center_ground": (350, 250, 900, 650),
    "left_trees": (0, 120, 480, 560),
    "upper_ground": (300, 0, 1000, 300),
    "river": (950, 0, 1280, 720),
    "character": (560, 300, 760, 650),
}


def analyze(directory: Path) -> None:
    paths = sorted(directory.glob("Frame_*.png"))
    if len(paths) < 2:
        raise ValueError(f"{directory}: expected at least two Frame_*.png files")
    frames = np.stack(
        [np.asarray(Image.open(path).convert("RGB"), dtype=np.int16) for path in paths]
    )
    print(directory)
    for name, (x1, y1, x2, y2) in REGIONS.items():
        region = frames[:, y1:y2, x1:x2]
        difference = np.abs(region[1:] - region[0])
        temporal_range = region.max(axis=0) - region.min(axis=0)
        print(
            f"  {name:14} mean_abs={difference.mean():.3f} "
            f"range_mean={temporal_range.mean():.3f} "
            f"range_gt8={100.0 * (temporal_range > 8).mean():.2f}%"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("directories", nargs="+", type=Path)
    args = parser.parse_args()
    for directory in args.directories:
        analyze(directory)


if __name__ == "__main__":
    main()
