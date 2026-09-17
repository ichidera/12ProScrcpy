#!/usr/bin/env python3
"""
analyze_block_headers.py — pull apart the still-unsolved generic UBWC
compression code (0x11 by default) by extracting the TRUE contiguous
256-byte block record (not the pixel-swizzled view decode_ubwc.py uses
for display) and lining it up against known ground-truth colors.

Key insight this tool is built around:
  get_pixel_offset(x_pix, y_pix) * cpp, for x_pix in [0,16) and y_pix in
  [0,4), bijects onto [0, 256) for cpp=4. It only permutes bytes WITHIN
  a block. block_x_xormask/block_y_xormask/bank math give a single base
  address per block that doesn't depend on x_pix/y_pix. So each block's
  256 bytes live in one contiguous span [block_base, block_base+256) in
  the tiled color plane -- we can slice it directly with plain Python
  slicing, no swizzle needed, and look at it as a raw compressed record.

Input files: raw captures saved by decode_ubwc.py --save-raw, i.e.
  [16-byte header: magic,u32 W,u32 H,u32 pitch][raw payload bytes]

Usage:
  # Single capture, just dump header-byte stats for all 0x11 blocks
  python3 analyze_block_headers.py capture_black_0000.bin

  # Multiple captures with known solid RGBA fill color, to correlate
  # header bytes against the color that produced them
  python3 analyze_block_headers.py \
      --known captures/red_0000.bin=255,0,0,255 \
      --known captures/green_0000.bin=0,255,0,255 \
      --known captures/blue_0000.bin=0,0,255,255 \
      --known captures/gray128_0000.bin=128,128,128,255 \
      --code 0x11 --header-len 12

  # Look at ONE specific block's full raw record (any code), e.g. the
  # exact block quoted in the notes
  python3 analyze_block_headers.py capture.bin --block-xy 5 5 --dump-full
"""
import argparse
import struct
import sys
from collections import Counter, defaultdict

import numpy as np

sys.path.insert(0, "/home/claude")
from ubwc_tiling import (
    compute_meta_plane_size, meta_grid, get_block_size,
    block_x_xormask, block_y_xormask, get_bank_mask, get_bank_shift,
    MACROTILE_8_CHANNEL,
)

MAGIC = 0x46425354


def load_capture(path):
    with open(path, "rb") as f:
        hdr = f.read(16)
        magic, w, h, pitch = struct.unpack("<IIII", hdr)
        if magic != MAGIC:
            raise ValueError(f"{path}: bad magic 0x{magic:08x} (not a decode_ubwc.py --save-raw file)")
        payload = np.frombuffer(f.read(), dtype=np.uint8)
    return w, h, pitch, payload


def block_base_offset(bx, by, pitch, cpp, highest_bank_bit, bank_swizzle_levels, macrotile_mode):
    """
    Single-block version of the addressing math in ubwc_tiling.ubwc_detile.
    Returns the byte offset, within the color plane, of this block's first
    (swizzled-position-0) byte. The block's other 255 bytes are exactly
    [offset, offset+256) -- see module docstring for why that's guaranteed.
    """
    block_width, _ = get_block_size(cpp)
    macrotile_stride = pitch // (4 * block_width * cpp)
    bank_mask = get_bank_mask(macrotile_stride, cpp, highest_bank_bit, bank_swizzle_levels)
    bank_shift = get_bank_shift(highest_bank_bit)
    macrotile_pitch = macrotile_stride * 4096

    x_mask = int(block_x_xormask(np.array([bx]), cpp, macrotile_mode)[0])
    y_mask = int(block_y_xormask(np.array([by]), cpp, bank_mask, bank_shift, macrotile_mode)[0])
    base_row = macrotile_pitch * (by // 4)
    return base_row + (x_mask ^ y_mask)


def iter_blocks_with_code(path, target_code, cpp, highest_bank_bit, bank_swizzle_levels, macrotile_mode):
    w, h, pitch, payload = load_capture(path)
    meta, meta_size = meta_grid(payload, w, h, cpp)
    color_plane = payload[meta_size:]
    blocks_h, blocks_w = meta.shape

    ys, xs = np.where(meta == target_code)
    for by, bx in zip(ys.tolist(), xs.tolist()):
        off = block_base_offset(bx, by, pitch, cpp, highest_bank_bit, bank_swizzle_levels, macrotile_mode)
        if off + 256 > color_plane.shape[0]:
            continue  # truncated capture, skip
        block = bytes(color_plane[off:off + 256])
        yield bx, by, off, block


def dump_full_block(path, bx, by, cpp, highest_bank_bit, bank_swizzle_levels, macrotile_mode):
    w, h, pitch, payload = load_capture(path)
    meta, meta_size = meta_grid(payload, w, h, cpp)
    color_plane = payload[meta_size:]
    code = int(meta[by, bx])
    off = block_base_offset(bx, by, pitch, cpp, highest_bank_bit, bank_swizzle_levels, macrotile_mode)
    block = bytes(color_plane[off:off + 256])
    print(f"{path}  block ({bx},{by})  meta_code=0x{code:02x}  base_offset=0x{off:x}")
    for row in range(0, 256, 16):
        chunk = block[row:row + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        print(f"  +{row:3d}: {hexs}")
    nz = sum(1 for b in block if b != 0)
    print(f"  non-zero bytes: {nz}/256, first non-zero-terminated run: "
          f"{len(block) - len(block.rstrip(bytes([0])))} trailing zeros stripped")


def header_stats(headers, header_len):
    """Per-byte-position min/max/mean/mode across a list of header byte-strings."""
    arr = np.array([list(h[:header_len]) for h in headers], dtype=np.uint8)
    lines = []
    for i in range(header_len):
        col = arr[:, i]
        counts = Counter(col.tolist())
        mode_val, mode_n = counts.most_common(1)[0]
        lines.append(
            f"    byte[{i:2d}]  min={col.min():3d} max={col.max():3d} "
            f"mean={col.mean():6.1f}  mode=0x{mode_val:02x} ({mode_n}/{len(col)})"
        )
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="*", help="capture .bin files (unlabeled mode)")
    ap.add_argument("--known", action="append", default=[],
                     help="path=R,G,B,A -- a capture with a known solid fill color, can repeat")
    ap.add_argument("--code", type=lambda s: int(s, 0), default=0x11, help="meta code to analyze (default 0x11)")
    ap.add_argument("--cpp", type=int, default=4)
    ap.add_argument("--highest-bank-bit", type=int, default=13)
    ap.add_argument("--bank-swizzle-levels", type=int, default=0x7)
    ap.add_argument("--macrotile-mode", default=MACROTILE_8_CHANNEL)
    ap.add_argument("--header-len", type=int, default=12, help="how many leading bytes per block to treat as 'header'")
    ap.add_argument("--max-print", type=int, default=8, help="how many example raw headers to print per capture")
    ap.add_argument("--block-xy", nargs=2, type=int, metavar=("BX", "BY"),
                     help="dump one specific block's full 256-byte record instead of aggregate stats")
    ap.add_argument("--dump-full", action="store_true", help="use with --block-xy")
    args = ap.parse_args()

    common = dict(cpp=args.cpp, highest_bank_bit=args.highest_bank_bit,
                  bank_swizzle_levels=args.bank_swizzle_levels, macrotile_mode=args.macrotile_mode)

    if args.block_xy:
        path = (args.captures + [k.split("=")[0] for k in args.known])[0]
        dump_full_block(path, args.block_xy[0], args.block_xy[1], **common)
        return

    known_entries = []
    for k in args.known:
        path, rgba = k.split("=")
        r, g, b, a = (int(x) for x in rgba.split(","))
        known_entries.append((path, (r, g, b, a)))

    all_entries = [(p, None) for p in args.captures] + known_entries

    per_capture_headers = {}
    for path, color in all_entries:
        headers = []
        n_blocks = 0
        for bx, by, off, block in iter_blocks_with_code(path, args.code, **common):
            headers.append(block[:args.header_len])
            n_blocks += 1
        per_capture_headers[path] = (color, headers)

        label = f"known color {color}" if color else "color unknown"
        print(f"\n=== {path}  ({label}) -- {n_blocks} blocks with code 0x{args.code:02x} ===")
        if n_blocks == 0:
            print("  (none found -- wrong code, or capture is truncated)")
            continue

        uniq = Counter(headers)
        print(f"  {len(uniq)} distinct header byte-patterns among {n_blocks} blocks")
        print("  most common headers (hex):")
        for h, cnt in uniq.most_common(args.max_print):
            print(f"    {' '.join(f'{b:02x}' for b in h)}   x{cnt}")
        print("  per-byte-position stats:")
        print(header_stats(headers, args.header_len))

    # Cross-capture correlation, only meaningful when >=2 known colors given
    known_only = [(p, c, h) for p, (c, h) in per_capture_headers.items() if c is not None]
    if len(known_only) >= 2:
        print("\n=== Cross-capture comparison (known colors) ===")
        print("  If a byte position's mean tracks a color channel roughly "
              "linearly across captures, that's very likely where that "
              "channel's base value (or a shared luma/base term) lives.")
        for i in range(args.header_len):
            print(f"\n  byte[{i}]:")
            for p, c, h in known_only:
                if not h:
                    continue
                vals = [x[i] for x in h]
                mean = sum(vals) / len(vals)
                print(f"    color={c!s:20s} mean=byte[{i}]={mean:6.1f}")


if __name__ == "__main__":
    main()
