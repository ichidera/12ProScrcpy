#!/usr/bin/env python3
"""
compare_captures.py — compare meta-byte grids across multiple raw captures.

Isolates which blocks actually change between captures (the real on-screen
content area, vs. fixed screen chrome that's identical no matter what's
displayed), and prints a side-by-side histogram table.

Usage:
  python3 compare_captures.py captures/black_0000.bin captures/red_0000.bin \
      captures/gradient_0000.bin captures/checker_0000.bin
"""
import argparse
import sys

import numpy as np

from inspect_meta import load_capture, meta_grid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--cpp', type=int, default=4)
    args = ap.parse_args()

    grids = []
    for path in args.files:
        w, h, pitch, payload = load_capture(path)
        g, _ = meta_grid(payload, w, h, args.cpp)
        grids.append(g)

    shapes = {g.shape for g in grids}
    if len(shapes) != 1:
        print(f"error: captures have different block-grid shapes: {shapes}", file=sys.stderr)
        sys.exit(1)

    stacked = np.stack(grids)  # (n_files, blocks_h, blocks_w)
    varies = np.any(stacked != stacked[0], axis=0)  # (blocks_h, blocks_w) bool

    n_varying = int(varies.sum())
    print(f"{len(args.files)} captures, block grid {stacked.shape[2]}x{stacked.shape[1]} "
          f"(WxH), {n_varying} blocks vary across captures\n")

    ys, xs = np.where(varies)
    if n_varying:
        print(f"varying-block bounding box: x=[{xs.min()},{xs.max()}] y=[{ys.min()},{ys.max()}]\n")

    # Side-by-side histogram over just the varying region, easier to compare
    # than separate --histogram calls.
    all_vals = sorted(set(int(v) for g in grids for v in g[varies].tolist()))
    header = "value".ljust(8) + ''.join(f.split('/')[-1][:14].ljust(16) for f in args.files)
    print(header)
    for v in all_vals:
        row = f"0x{v:02x}".ljust(8)
        for g in grids:
            count = int((g[varies] == v).sum())
            row += str(count).ljust(16)
        print(row)

    print(f"\n(fixed/non-varying blocks: {int((~varies).sum())} -- same meta value in every "
          f"capture, i.e. screen chrome/letterboxing rather than your test pattern)")


if __name__ == '__main__':
    main()
