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

  # Read known colors from a file (one "path=R,G,B,A" per line, # comments
  # ok) instead of a huge --known flag list, auto-isolate the per-capture
  # content-region header (skipping all-zero and fixed-chrome headers),
  # and run the automated YCoCg-R bit-field search against them:
  python3 analyze_block_headers.py --known-file colors.txt --bitfield-search

  # Still supports --known path=R,G,B,A directly, can mix with --known-file
  python3 analyze_block_headers.py \
      --known captures/red_0000.bin=255,0,0,255 \
      --known captures/green_0000.bin=0,255,0,255 \
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


def ycocg_r(r, g, b):
    """Reversible YCoCg-R transform (the §7.1 patent lead), all terms mod 256."""
    co = (r - b) & 0xff
    tmp = (b + (co >> 1)) & 0xff
    cg = (g - tmp) & 0xff
    y = (tmp + (cg >> 1)) & 0xff
    return {"Y": y, "Co": co, "Cg": cg, "tmp": tmp}


def solid_fields(r, g, b):
    """The three confirmed exact fields (§7.5/7.6) for a given color, as (value, bit_start, width)."""
    yco = ycocg_r(r, g, b)
    return {
        "Y6":  (yco["Y"] & 0x3f, 3, 6),
        "Co8": (yco["Co"], 11, 8),
        "Cg7": (yco["Cg"] & 0x7f, 20, 7),
    }


def find_blocks_by_color(path, rgb, code, cpp, highest_bank_bit, bank_swizzle_levels, macrotile_mode):
    """
    Scan every block tagged `code` and return those whose header matches the
    three confirmed exact fields (Y6/Co8/Cg7) for the given RGB -- regardless
    of block count. Needed when the target color only covers part of the
    screen (e.g. a small rectangle), where "most common header" would just
    find the background instead.
    """
    r, g, b = rgb
    fields = solid_fields(r, g, b)
    matches = []
    total = 0
    for bx, by, off, block in iter_blocks_with_code(path, code, cpp, highest_bank_bit,
                                                      bank_swizzle_levels, macrotile_mode):
        total += 1
        v = bytes_to_int_le(block[:7])
        ok = all(((v >> start) & ((1 << width) - 1)) == val for val, start, width in fields.values())
        if ok:
            matches.append((bx, by, block[:12]))
    return matches, total


def unresolved_bits(header12):
    v = bytes_to_int_le(header12[:7])
    return {
        "tag(0-2)": v & 0b111,
        "bits9-10": (v >> 9) & 0b11,
        "bit19": (v >> 19) & 1,
        "bit27": (v >> 27) & 1,
    }


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


def parse_known_file(path):
    """path=R,G,B,A per line; blank lines and lines starting with # ignored."""
    entries = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                print(f"  (warning: {path}:{lineno}: no '=' found, skipping: {line!r})", file=sys.stderr)
                continue
            capture_path, rgba = line.split("=", 1)
            try:
                r, g, b, a = (int(x) for x in rgba.split(","))
            except ValueError:
                print(f"  (warning: {path}:{lineno}: expected R,G,B,A, skipping: {line!r})", file=sys.stderr)
                continue
            entries.append((capture_path.strip(), (r, g, b, a)))
    return entries


def isolate_content_headers(per_capture_headers, header_len):
    """
    Auto-detect, per capture, the single header byte-pattern that actually
    represents the fill color -- as opposed to (a) the literal all-zero
    header, which dominates by block count but isn't color data, and
    (b) headers that are byte-identical across every capture regardless
    of displayed color (fixed screen chrome: status bar, nav bar, etc).

    Only what's left after excluding both of those, per capture, is a
    genuine content signal. Returns {path: entry or None}, where entry is
    a dict {header, count, low_confidence}.

    low_confidence is set when a capture's best remaining count is well
    below the batch median -- this happens for solid black/white fills,
    where nearly the whole screen gets routed to the already-solved exact
    0x05/0x0d codes instead of 0x11, leaving only a handful of boundary/
    antialiased 0x11 blocks whose true pixel content isn't actually pure
    black or white. Including those in a color correlation silently
    corrupts it (confirmed empirically -- see notes). Flagged, not
    dropped outright, so the caller can decide.
    """
    zero_header = bytes(header_len)
    counters = {p: Counter(h[:header_len] for h in headers) for p, (_, headers) in per_capture_headers.items()}

    # Headers present (with any count) in every single capture -- fixed chrome.
    header_sets = [set(c.keys()) for c in counters.values()]
    common_to_all = set.intersection(*header_sets) if len(header_sets) > 1 else set()

    raw = {}
    for p, counter in counters.items():
        candidates = [(h, cnt) for h, cnt in counter.items()
                      if h != zero_header and h not in common_to_all]
        candidates.sort(key=lambda hc: -hc[1])
        raw[p] = candidates[0] if candidates else None

    counts = [cnt for r in raw.values() if r is not None for _, cnt in [r]]
    median_count = sorted(counts)[len(counts) // 2] if counts else 0

    results = {}
    for p, r in raw.items():
        if r is None:
            results[p] = None
            continue
        h, cnt = r
        low_confidence = median_count > 0 and cnt < 0.3 * median_count
        results[p] = {"header": h, "count": cnt, "low_confidence": low_confidence}
    return results


def bytes_to_int_le(b):
    v = 0
    for i, x in enumerate(b):
        v |= x << (8 * i)
    return v


def bitfield_search(colors_and_headers, min_width=2, max_width=16, focus_bits=None, min_distinct=4):
    """
    Brute-force every (start_bit, width) window of the header, read as one
    little-endian bitstream, against several color-derived candidate
    fields (R, G, B, min/max/mid/sum, and the YCoCg-R terms). Reports EXACT
    matches (field == candidate mod 2**width for every capture) -- this is
    what surfaced the confirmed Y/Co/Cg fields in the notes; a plain
    correlation search misses these because of modular wraparound.

    colors_and_headers: list of ((r,g,b,a), header_bytes)
    focus_bits: optional set of bit indices to restrict the report to
                (e.g. the still-unresolved {9,10,19,27}) -- still searches
                everything, just filters what's printed.
    """
    ints = [bytes_to_int_le(h) for _, h in colors_and_headers]
    total_bits = 8 * len(colors_and_headers[0][1])

    def channel_targets(rgb):
        r, g, b, a = rgb
        yco = ycocg_r(r, g, b)
        return {
            "R": r, "G": g, "B": b,
            "min": min(r, g, b), "max": max(r, g, b), "mid": sorted((r, g, b))[1],
            "sum": r + g + b,
            **yco,
        }

    all_targets = [channel_targets(rgb) for rgb, _ in colors_and_headers]
    target_names = list(all_targets[0].keys())

    exact, near = [], []
    for width in range(min_width, max_width + 1):
        for start in range(0, total_bits - width + 1):
            if focus_bits is not None and not (set(range(start, start + width)) & focus_bits):
                continue
            mask = (1 << width) - 1
            xs = [(v >> start) & mask for v in ints]
            if len(set(xs)) < min_distinct:
                continue
            for name in target_names:
                ys = [t[name] for t in all_targets]
                if len(set(ys)) < min_distinct:
                    continue
                mismatches = sum(1 for x, y in zip(xs, ys) if x != (y & mask))
                if mismatches == 0:
                    exact.append((start, width, name))
                elif mismatches <= 2:
                    near.append((mismatches, start, width, name))

    return exact, sorted(near)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="*", help="capture .bin files (unlabeled mode)")
    ap.add_argument("--known", action="append", default=[],
                     help="path=R,G,B,A -- a capture with a known solid fill color, can repeat")
    ap.add_argument("--known-file", help="file with one 'path=R,G,B,A' per line (# comments ok); "
                                          "combines with --known if both given")
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
    ap.add_argument("--quiet", action="store_true", help="skip the per-capture header dump/stats, "
                                                           "just show content-header isolation (and --bitfield-search if given)")
    ap.add_argument("--bitfield-search", action="store_true",
                     help="auto-isolate each known capture's content header, then brute-force search "
                          "little-endian (start_bit,width) windows against R/G/B/YCoCg-R fields for "
                          "exact bit-level matches")
    ap.add_argument("--focus-bits", help="comma-separated bit indices to restrict --bitfield-search output to, "
                                          "e.g. '9,10,19,27' for the still-unresolved bits from the notes")
    ap.add_argument("--include-low-confidence", action="store_true",
                     help="include content headers flagged low-confidence (anomalously low block "
                          "count, e.g. black/white fills where most of the screen is on the "
                          "already-solved exact 0x05/0x0d codes) in --bitfield-search anyway")
    ap.add_argument("--find-by-color", help="R,G,B -- instead of the usual analysis, scan the given "
                                             "capture(s) for blocks matching this color's predicted "
                                             "Y6/Co8/Cg7 fields (regardless of block count) and print "
                                             "their position + still-unresolved bits. For colors that "
                                             "only cover part of the screen (e.g. a small rectangle), "
                                             "where 'most common header' would just find the background.")
    args = ap.parse_args()

    common = dict(cpp=args.cpp, highest_bank_bit=args.highest_bank_bit,
                  bank_swizzle_levels=args.bank_swizzle_levels, macrotile_mode=args.macrotile_mode)

    if args.block_xy:
        path = (args.captures + [k.split("=")[0] for k in args.known])[0]
        dump_full_block(path, args.block_xy[0], args.block_xy[1], **common)
        return

    if args.find_by_color:
        r, g, b = (int(x) for x in args.find_by_color.split(","))
        paths = args.captures + [k.split("=")[0] for k in args.known]
        if args.known_file:
            paths += [p for p, _ in parse_known_file(args.known_file)]
        for path in paths:
            matches, total = find_blocks_by_color(path, (r, g, b), args.code, **common)
            print(f"\n=== {path} -- searching for RGB({r},{g},{b}) among {total} 0x{args.code:02x} blocks ===")
            if not matches:
                print("  no matching blocks found")
                continue
            print(f"  {len(matches)} matching blocks:")
            bits_seen = defaultdict(set)
            for bx, by, h in matches:
                ub = unresolved_bits(h)
                for k, v in ub.items():
                    bits_seen[k].add(v)
                print(f"    block ({bx:4d},{by:4d})  {' '.join(f'{x:02x}' for x in h)}  "
                      f"tag={ub['tag(0-2)']} bits9-10={ub['bits9-10']} bit19={ub['bit19']} bit27={ub['bit27']}")
            print(f"  value ranges across these {len(matches)} matching blocks:")
            for k, vals in bits_seen.items():
                print(f"    {k}: {sorted(vals)}")
        return

    known_entries = []
    for k in args.known:
        path, rgba = k.split("=")
        r, g, b, a = (int(x) for x in rgba.split(","))
        known_entries.append((path, (r, g, b, a)))
    if args.known_file:
        known_entries.extend(parse_known_file(args.known_file))

    all_entries = [(p, None) for p in args.captures] + known_entries

    per_capture_headers = {}
    for path, color in all_entries:
        headers = []
        n_blocks = 0
        for bx, by, off, block in iter_blocks_with_code(path, args.code, **common):
            headers.append(block[:args.header_len])
            n_blocks += 1
        per_capture_headers[path] = (color, headers)

        if args.quiet:
            continue

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

    # Auto-isolate the content-region header per capture (skips all-zero
    # headers and headers shared byte-identically across every capture --
    # see isolate_content_headers() docstring / notes §7.4 for why that
    # matters more than it sounds like it should).
    content = isolate_content_headers(per_capture_headers, args.header_len)
    print("\n=== Auto-isolated content header per capture ===")
    print("  (excludes the all-zero header and any header byte-identical")
    print("   across every capture -- those are padding/fixed-chrome, not color data)")
    for p, (color, _) in per_capture_headers.items():
        entry = content.get(p)
        if entry is None:
            print(f"  {p:40s} color={color!s:20s}  (no content header found)")
        else:
            flag = "  [LOW CONFIDENCE -- see below]" if entry["low_confidence"] else ""
            print(f"  {p:40s} color={color!s:20s}  x{entry['count']:5d}  "
                  f"{' '.join(f'{b:02x}' for b in entry['header'])}{flag}")

    low_conf_paths = [p for p, e in content.items() if e is not None and e["low_confidence"]]
    if low_conf_paths:
        print("\n  Low-confidence captures (block count far below the rest of the batch --")
        print("  likely a solid fill where most of the screen landed on the already-solved")
        print("  0x05/0x0d exact codes, leaving only unrepresentative boundary/AA blocks")
        print("  tagged 0x11). Excluded from --bitfield-search by default:")
        for p in low_conf_paths:
            print(f"    {p}")

    if args.bitfield_search:
        known_content = [
            (color, content[p]["header"]) for p, (color, _) in per_capture_headers.items()
            if color is not None and content.get(p) is not None
            and (args.include_low_confidence or not content[p]["low_confidence"])
        ]
        if len(known_content) < 3:
            print("\n(--bitfield-search needs at least 3 known-color captures with a detected content header)")
            return

        focus_bits = None
        if args.focus_bits:
            focus_bits = set(int(x) for x in args.focus_bits.split(","))

        exact, near = bitfield_search(known_content, focus_bits=focus_bits)

        print(f"\n=== Bit-field search across {len(known_content)} known colors "
              f"({'all bits' if focus_bits is None else 'bits ' + args.focus_bits}) ===")
        print("  EXACT matches (field == target mod 2^width, zero exceptions):")
        # de-dup: a shorter exact match is implied by a longer one at the same start+target,
        # so only print the widest exact match per (start, target).
        best_exact = {}
        for start, width, name in exact:
            key = (start, name)
            if key not in best_exact or width > best_exact[key][0]:
                best_exact[key] = (width, start, name)
        for width, start, name in sorted(best_exact.values(), key=lambda t: t[1]):
            print(f"    bits {start:2d}-{start+width-1:<2d} (width {width:2d})  ==  {name} mod 2^{width}")

        if near:
            print("\n  NEAR matches (1-2 exceptions out of "
                  f"{len(known_content)} colors -- possible but not confirmed):")
            for mism, start, width, name in near[:20]:
                print(f"    bits {start:2d}-{start+width-1:<2d} (width {width:2d})  ~=  {name} mod 2^{width}"
                      f"   ({mism} mismatch{'es' if mism != 1 else ''})")
        return

    # Cross-capture correlation, only meaningful when >=2 known colors given
    known_only = [(p, c, h) for p, (c, h) in per_capture_headers.items() if c is not None]
    if not args.quiet and len(known_only) >= 2:
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
