#!/usr/bin/env python3
"""
inspect_meta.py — offline analysis tool for raw captures saved with
decode_ubwc.py's --save-raw flag.

This is the workhorse for reverse-engineering the actual UBWC compression:
  1. Display a known, controlled pattern on the phone (solid color, a
     gradient, a checkerboard, etc).
  2. Capture with --save-raw while it's on screen.
  3. Run this tool to see the meta-byte value for any block, and the raw
     256 bytes stored at that block's real (address-corrected) location.
  4. Since you know what color that block *should* be, compare it against
     what's actually in memory and start forming hypotheses about how the
     meta byte selects the packing/bit-width scheme.

Usage:
  python3 inspect_meta.py capture_0000.bin --histogram
  python3 inspect_meta.py capture_0000.bin --block 10 20
  python3 inspect_meta.py capture_0000.bin --block 10 20 --expect-rgba 255 0 0 255
"""
import argparse
import struct
import sys

import numpy as np

from ubwc_tiling import (
    compute_meta_plane_size, get_block_size, block_x_xormask,
    block_y_xormask, get_bank_mask, get_bank_shift, get_pixel_offset,
    MACROTILE_8_CHANNEL,
)

MAGIC = 0x46425354


def load_capture(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic, w, h, pitch = struct.unpack('<IIII', data[:16])
    if magic != MAGIC:
        print(f"warning: bad magic 0x{magic:08x} (expected 0x{MAGIC:08x}) "
              f"-- file may not be a decode_ubwc.py --save-raw dump", file=sys.stderr)
    payload = np.frombuffer(data[16:], dtype=np.uint8)
    return w, h, pitch, payload


def meta_grid(payload, w, h, cpp):
    meta_size, meta_pitch, meta_height = compute_meta_plane_size(w, h, cpp)
    block_width, block_height = get_block_size(cpp)
    blocks_w = (w + block_width - 1) // block_width
    blocks_h = (h + block_height - 1) // block_height
    meta = payload[:meta_size].reshape(meta_height, meta_pitch)
    # only the top-left blocks_h x blocks_w corner is real data, the rest is padding
    return meta[:blocks_h, :blocks_w], meta_size


def block_byte_offset(x_block, y_block, pitch, cpp, hbb, levels, mode):
    block_width, block_height = get_block_size(cpp)
    macrotile_stride = pitch // (4 * block_width * cpp)
    bank_mask = get_bank_mask(macrotile_stride, cpp, hbb, levels)
    bank_shift = get_bank_shift(hbb)
    macrotile_pitch = macrotile_stride * 4096
    x_mask = int(block_x_xormask(np.array([x_block]), cpp, mode)[0])
    y_mask = int(block_y_xormask(np.array([y_block]), cpp, bank_mask, bank_shift, mode)[0])
    base_row = macrotile_pitch * (y_block // 4)
    return base_row + (x_mask ^ y_mask)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--cpp', type=int, default=4)
    ap.add_argument('--highest-bank-bit', type=int, default=13)
    ap.add_argument('--bank-swizzle-levels', type=int, default=0)
    ap.add_argument('--macrotile-mode', default=MACROTILE_8_CHANNEL)
    ap.add_argument('--histogram', action='store_true', help='print a histogram of meta byte values')
    ap.add_argument('--block', nargs=2, type=int, metavar=('X_BLOCK', 'Y_BLOCK'),
                     help='inspect one block: prints its meta byte and raw 256 bytes')
    ap.add_argument('--expect-rgba', nargs=4, type=int, metavar=('R', 'G', 'B', 'A'),
                     help='what you expect this block\'s color to be (for note-taking in the output)')
    args = ap.parse_args()

    w, h, pitch, payload = load_capture(args.file)
    print(f"{args.file}: {w}x{h} pitch={pitch}")

    meta, meta_size = meta_grid(payload, w, h, args.cpp)
    print(f"meta plane: {meta_size} bytes, block grid {meta.shape[1]}x{meta.shape[0]} (WxH in blocks)")

    if args.histogram:
        vals, counts = np.unique(meta, return_counts=True)
        print("\nmeta byte histogram:")
        for v, c in sorted(zip(vals.tolist(), counts.tolist()), key=lambda t: -t[1]):
            print(f"  0x{v:02x} ({v:3d}, 0b{v:08b}): {c} blocks")

    if args.block:
        xb, yb = args.block
        if yb >= meta.shape[0] or xb >= meta.shape[1]:
            print(f"block ({xb},{yb}) out of range for grid {meta.shape[1]}x{meta.shape[0]}")
            return
        meta_val = int(meta[yb, xb])
        print(f"\nblock ({xb},{yb}): meta = 0x{meta_val:02x} ({meta_val:3d}, 0b{meta_val:08b})")
        if args.expect_rgba:
            print(f"  (expected color for note-taking: RGBA{tuple(args.expect_rgba)})")

        color_plane = payload[meta_size:]
        off = block_byte_offset(xb, yb, pitch, args.cpp, args.highest_bank_bit,
                                 args.bank_swizzle_levels, args.macrotile_mode)
        block_bytes = color_plane[off:off + 256]
        print(f"  raw 256 bytes at color-plane offset 0x{off:x}:")
        for row in range(0, 256, 16):
            hexed = ' '.join(f'{b:02x}' for b in block_bytes[row:row + 16])
            print(f"    {hexed}")

        # Also show it de-swizzled into (block_height, block_width, cpp) using the
        # *uncompressed*-block pixel_offset mapping, purely as a reference view --
        # if the block IS actually compressed this will look wrong/noisy, which is
        # itself useful signal.
        block_width, block_height = get_block_size(args.cpp)
        print(f"  same bytes, read as if uncompressed ({block_width}x{block_height} px, "
              f"cpp={args.cpp}) -- noisy here suggests real compression is in play:")
        for y in range(block_height):
            row_pixels = []
            for x in range(block_width):
                po = int(get_pixel_offset(np.array([x]), np.array([y]))[0]) * args.cpp
                px = block_bytes[po:po + args.cpp]
                row_pixels.append(''.join(f'{b:02x}' for b in px))
            print("    " + ' '.join(row_pixels))


if __name__ == '__main__':
    main()
