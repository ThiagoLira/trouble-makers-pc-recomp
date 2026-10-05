#!/usr/bin/env python3
"""Review captures from test_widescreen_playable.sh (requires Pillow and numpy).

Usage: python3 tools/audit_widescreen_backgrounds.py CAPTURE_DIR
Writes background-audit.tsv and labeled contact sheets alongside the captures.
Scores are review hints, not pass/fail assertions: dark art, foreground objects,
and intentional pillarboxes can resemble missing backgrounds. Inspect the sheets
and compare flagged levels with --no-widescreen before changing rendering.
"""

import csv
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw


# These progression rows deliberately retain the original gameplay canvas.
FIXED = {11, 27, 47, 52, 56, 57}


def metrics(path):
    pixels = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    height, width, _ = pixels.shape
    # Avoid the horizontal letterbox and HUD. Original visible horizontal
    # borders are x=14 and x=302 in the 320x240 coordinate system.
    top, bottom = round(height * 0.24), round(height * 0.84)
    left = round(width / 2 + (14 - 160) * height / 240)
    right = round(width / 2 + (302 - 160) * height / 240)
    if not 2 < left < right < width - 2:
        raise ValueError(f"capture is not wider than 4:3: {path}")
    region = pixels[top:bottom]
    black = np.max(region, axis=2) < 8
    center_black = black[:, left:right].mean()
    wing_black = max(black[:, :left].mean(), black[:, right:].mean())
    # Compare the strongest vertical boundary near each former border to its
    # neighbors. A three-pixel window tolerates VI crop/resolution rounding.
    jumps = np.abs(np.diff(region, axis=1)).mean(axis=(0, 2)) / 255
    radius = max(8, round(16 * height / 240))
    seams = []
    for edge in (left, right):
        peak = jumps[edge - 3:edge + 3].max()
        nearby = np.concatenate((jumps[max(0, edge - radius):edge - 3],
                                 jumps[edge + 3:min(width - 1, edge + radius)]))
        seams.append(max(0.0, float(peak - np.median(nearby))))
    return float(wing_black), max(0.0, float(wing_black - center_black)), max(seams)


def main():
    root = Path(sys.argv[1])
    with (root / "results.tsv").open() as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))
    report = []
    thumbnails = []
    for row in rows:
        stage = int(row["stage"])
        frames = sorted(root.glob(f"stage-{stage:02d}-scene-*-frame-*.png"))
        values = [metrics(frame) for frame in frames]
        scores = tuple(max(v[i] for v in values) for i in range(3)) if values else (0, 0, 0)
        status = "missing capture" if not frames else (
            "fixed 4:3" if stage in FIXED else
            "review" if scores[1] > 0.08 or scores[2] > 0.08 else "inspect sheet")
        report.append({**row, "background_review": status,
                       "wing_black": f"{scores[0]:.4f}",
                       "excess_wing_black": f"{scores[1]:.4f}",
                       "border_jump": f"{scores[2]:.4f}"})
        strip = Image.new("RGB", (960, 204), "#202020")
        draw = ImageDraw.Draw(strip)
        draw.text((6, 4), f"stage {stage:02d} / scene {row['scene']} | "
                  f"{row['status']} | {status} | black excess {scores[1]:.2f}, "
                  f"border jump {scores[2]:.2f}", fill="white")
        for i, frame in enumerate(frames[:3]):
            with Image.open(frame) as source:
                source.thumbnail((320, 180))
                strip.paste(source, (i * 320, 22))
        thumbnails.append(strip)
    if not report:
        raise ValueError("no level results in manifest")
    with (root / "background-audit.tsv").open("w") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(report[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(report)
    for start in range(0, len(thumbnails), 8):
        group = thumbnails[start:start + 8]
        sheet = Image.new("RGB", (960, 204 * len(group)))
        for i, strip in enumerate(group):
            sheet.paste(strip, (0, i * 204))
        sheet.save(root / f"background-review-{start // 8 + 1:02d}.jpg")
    for row in report:
        if row["background_review"] in {"review", "missing capture"} or row["status"] != "pass":
            print(f"stage {row['stage']} scene {row['scene']}: "
                  f"{row['status']}, {row['background_review']}; "
                  f"black excess={row['excess_wing_black']} border jump={row['border_jump']}")
    print(f"Reviewed {len(report)} level entries; report and sheets in {root}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main()
