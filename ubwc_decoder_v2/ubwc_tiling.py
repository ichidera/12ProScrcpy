#!/usr/bin/env python3
"""
ubwc_tiling.py — Adreno UBWC layout + tiled-to-linear swizzle math,
ported from Mesa/Freedreno sources supplied alongside fbstream6:

    freedreno_layout.h   (alignment constants, fdl_ubwc_config, block helpers)
    freedreno_layout.c   (fdl_dump_layout / block-size tables -- reference only)
    fd6_tiled_memcpy.cc  (get_pixel_offset, block_x_xormask, block_y_xormask,
                          get_bank_mask/shift, get_block_size — the actual
                          UBWC "tiled_to_linear" swizzle used by turnip/mesa)

IMPORTANT CAVEAT — read before trusting the numbers blindly
-------------------------------------------------------------------------
The uploaded sources give us:
  * the *exact* pixel-in-block swizzle (get_pixel_offset)
  * the *exact* block-to-macrotile xor swizzle (block_x_xormask / block_y_xormask)
  * the *exact* bank-swizzle mask/shift formulas (fdl6_get_bank_mask/shift)
  * the alignment constants used for the UBWC meta (flag) plane
    (RGB_TILE_WIDTH_ALIGNMENT, RGB_TILE_HEIGHT_ALIGNMENT, UBWC_PLANE_SIZE_ALIGNMENT)

They do NOT include the body of fdl6_layout_image() itself (only its
prototype in the .h), so the meta-plane size formula below is a
reconstruction from those alignment constants + the per-cpp block table
in fd6_tiled_memcpy.cc's get_block_size(), which is the standard published
Freedreno approach (1 meta byte per compression block, pitch/height rounded
up to a 64x16-block "meta tile", plane padded to 4K). It matches the probe
dump we have (see decode_ubwc.py header) to within alignment, but treat it
as "best reconstruction from the given constants", not a byte-exact copy
of code we don't have.

Also NOT available from any provided source: the per-device UBWC bank
config (highest_bank_bit, bank_swizzle_levels, macrotile_mode/channel
count). Those are picked by the kernel display driver per SoC and aren't
recoverable from the DRM modifier alone (modifier 0x0500000000000001 is
just the generic "QCOM_COMPRESSED" tag — vendor QCOM=0x05, value=1). They
are exposed here as parameters with commonly-seen Adreno 7xx defaults, and
`autotune_bank_config()` is provided to brute-force search them against a
real captured frame.
"""
from __future__ import annotations

import numpy as np

# ---------------------------------------------------------------------------
# Constants (freedreno_layout.h)
# ---------------------------------------------------------------------------
RGB_TILE_WIDTH_ALIGNMENT = 64
RGB_TILE_HEIGHT_ALIGNMENT = 16
UBWC_PLANE_SIZE_ALIGNMENT = 4096

MACROTILE_4_CHANNEL = "4ch"
MACROTILE_8_CHANNEL = "8ch"


def _align(v: int, a: int) -> int:
    return (v + a - 1) // a * a


# ---------------------------------------------------------------------------
# get_block_size()  (fd6_tiled_memcpy.cc)
# ---------------------------------------------------------------------------
_BLOCK_SIZE_TABLE = {
    1: (32, 8),
    2: (32, 4),   # non-r8g8 case; r8g8 special-case (16,8) not needed for RGBA streams
    4: (16, 4),
    8: (8, 4),
    16: (4, 4),
    32: (4, 2),
    64: (2, 2),
}


def get_block_size(cpp: int, r8g8: bool = False):
    if cpp == 2 and r8g8:
        return 16, 8
    if cpp not in _BLOCK_SIZE_TABLE:
        raise ValueError(f"unsupported cpp={cpp}")
    return _BLOCK_SIZE_TABLE[cpp]


# ---------------------------------------------------------------------------
# UBWC meta (flag) plane size — reconstructed from the alignment constants
# in freedreno_layout.h + the block table above. See module docstring.
# ---------------------------------------------------------------------------
def compute_meta_plane_size(width: int, height: int, cpp: int, r8g8: bool = False):
    """
    Returns (meta_size_bytes, meta_pitch_bytes, meta_height_blocks).

    One meta byte covers one (block_width x block_height) compression block.
    The meta plane's own pitch/height are rounded up to a 64x16 "meta tile"
    (RGB_TILE_WIDTH_ALIGNMENT x RGB_TILE_HEIGHT_ALIGNMENT), and the whole
    plane is padded up to a 4K (UBWC_PLANE_SIZE_ALIGNMENT) boundary — this is
    the alignment the color/tiled plane starts at.
    """
    block_width, block_height = get_block_size(cpp, r8g8)
    blocks_w = (width + block_width - 1) // block_width
    blocks_h = (height + block_height - 1) // block_height
    meta_pitch = _align(blocks_w, RGB_TILE_WIDTH_ALIGNMENT)
    meta_height = _align(blocks_h, RGB_TILE_HEIGHT_ALIGNMENT)
    meta_size = _align(meta_pitch * meta_height, UBWC_PLANE_SIZE_ALIGNMENT)
    return meta_size, meta_pitch, meta_height


# ---------------------------------------------------------------------------
# get_pixel_offset()  (fd6_tiled_memcpy.cc, verbatim translation)
# ---------------------------------------------------------------------------
def get_pixel_offset(x, y):
    x = np.asarray(x, dtype=np.int64)
    y = np.asarray(y, dtype=np.int64)
    return (
        (x & 1)
        | ((y & 1) << 1)
        | (((x & 2) >> 1) << 2)
        | (((y & 2) >> 1) << 3)
        | (((x & 0x1C) >> 2) << 4)
        | (((y & 4) >> 2) << 7)
    )


# ---------------------------------------------------------------------------
# block_x_xormask() / block_y_xormask()  (fd6_tiled_memcpy.cc, verbatim)
# ---------------------------------------------------------------------------
def block_x_xormask(x, cpp: int, mode: str):
    x = np.asarray(x, dtype=np.int64)
    hi_term = (x & 0b010) if cpp < 16 else 0
    if mode == MACROTILE_4_CHANNEL:
        core = ((x & 1) * 0b111) ^ hi_term ^ ((x >> 1) << 3)
    elif mode == MACROTILE_8_CHANNEL:
        hi_term = (x & 0b110) if cpp < 16 else 0
        core = ((x & 1) * 0b111) ^ hi_term ^ ((x >> 1) << 3)
    else:
        raise ValueError(f"unknown macrotile mode {mode!r}")
    return core << 8


def block_y_xormask(y, cpp: int, bank_mask: int, bank_shift: int, mode: str):
    y = np.asarray(y, dtype=np.int64)
    base = ((y & 1) * 0b110) ^ (((y >> 1) & 1) * 0b011)
    if mode == MACROTILE_4_CHANNEL:
        core = base << 8
    elif mode == MACROTILE_8_CHANNEL:
        extra = (y & 0b100) if cpp < 16 else 0
        core = (base ^ extra) << 8
    else:
        raise ValueError(f"unknown macrotile mode {mode!r}")
    return core | ((y & bank_mask) << bank_shift)


# ---------------------------------------------------------------------------
# fdl6_get_bank_mask() / fdl6_get_bank_shift()  (fd6_tiled_memcpy.cc, verbatim)
# then the get_bank_mask()/get_bank_shift() wrappers used by the hot loop.
# ---------------------------------------------------------------------------
def fdl6_get_bank_mask(macrotile_stride: int, cpp: int, highest_bank_bit: int,
                        bank_swizzle_levels: int, is_r8g8: bool = False):
    offset = 1 if (is_r8g8 or cpp == 1) else 0
    mask = 0

    def aligned(shift_bits):
        if shift_bits < 0:
            return True  # 2**negative -> always "aligned" for our purposes
        return (macrotile_stride & ((1 << shift_bits) - 1)) == 0

    if (bank_swizzle_levels & 0x2) and aligned(highest_bank_bit - 12 + offset):
        mask |= 0b1
    if (bank_swizzle_levels & 0x4) and aligned(highest_bank_bit - 11 + offset):
        mask |= 0b10
    if (bank_swizzle_levels & 0x1) and aligned(highest_bank_bit - 10 + offset):
        mask |= 0b100
    return mask


def fdl6_get_bank_shift(highest_bank_bit: int) -> int:
    return highest_bank_bit - 1


def get_bank_mask(macrotile_stride, cpp, highest_bank_bit, bank_swizzle_levels, is_r8g8=False):
    return fdl6_get_bank_mask(macrotile_stride, cpp, highest_bank_bit,
                               bank_swizzle_levels, is_r8g8) << 2


def get_bank_shift(highest_bank_bit) -> int:
    return fdl6_get_bank_shift(highest_bank_bit) - 2


# ---------------------------------------------------------------------------
# Full tiled -> linear detile, vectorized with numpy.
# This is the direct numpy translation of fdl6_memcpy_tiled_to_linear's
# hot loop in fd6_tiled_memcpy.cc (USE_SLOW_PATH branch, which is the
# straightforward per-pixel version — the fast paths just batch the same
# math with SIMD, they don't change the addressing).
# ---------------------------------------------------------------------------
def ubwc_detile(color_plane: np.ndarray, width: int, height: int, pitch: int,
                 cpp: int = 4, highest_bank_bit: int = 15,
                 bank_swizzle_levels: int = 0x7,
                 macrotile_mode: str = MACROTILE_8_CHANNEL,
                 is_r8g8: bool = False) -> np.ndarray:
    """
    color_plane: flat uint8 array containing ONLY the tiled color plane
                 (i.e. with the UBWC meta/flag plane already sliced off
                 the front — see compute_meta_plane_size()).
    Returns an (height, width, cpp) uint8 array in normal row-major order.
    """
    block_width, block_height = get_block_size(cpp, is_r8g8)
    macrotile_stride = pitch // (4 * block_width * cpp)
    bank_mask = get_bank_mask(macrotile_stride, cpp, highest_bank_bit,
                               bank_swizzle_levels, is_r8g8)
    bank_shift = get_bank_shift(highest_bank_bit)
    macrotile_pitch = macrotile_stride * 4096

    X = np.arange(width, dtype=np.int64)
    Y = np.arange(height, dtype=np.int64)

    x_block = X // block_width
    x_pix = X % block_width
    y_block = Y // block_height
    y_pix = Y % block_height

    # Per-column / per-row xor components (broadcast into the full grid).
    x_mask_col = block_x_xormask(x_block, cpp, macrotile_mode)              # (W,)
    y_mask_row = block_y_xormask(y_block, cpp, bank_mask, bank_shift,
                                  macrotile_mode)                            # (H,)
    block_offset_grid = x_mask_col[None, :] ^ y_mask_row[:, None]           # (H,W)

    base_row = macrotile_pitch * (y_block // 4)                            # (H,)

    pixel_byte = get_pixel_offset(x_pix[None, :], y_pix[:, None]) * cpp    # (H,W)

    total_offset = base_row[:, None] + block_offset_grid + pixel_byte      # (H,W) int64

    needed = int(total_offset.max()) + cpp
    if needed > color_plane.shape[0]:
        pad = needed - color_plane.shape[0]
        color_plane = np.concatenate(
            [color_plane, np.zeros(pad, dtype=color_plane.dtype)]
        )

    idx = total_offset[..., None] + np.arange(cpp, dtype=np.int64)
    out = color_plane[idx]
    return out


# ---------------------------------------------------------------------------
# Autotune: brute-force the (highest_bank_bit, bank_swizzle_levels,
# macrotile_mode) triple against one real captured (still-tiled) frame.
# We don't know the ground truth, so we score by "local smoothness relative
# to global variance" — real screen content is locally smooth-ish, a wrong
# swizzle produces near-random per-pixel noise. Trivial degenerate decodes
# (flat/blank) are penalized by requiring a minimum global variance.
# ---------------------------------------------------------------------------
def _smoothness_score(frame: np.ndarray) -> float:
    f = frame[..., :3].astype(np.float32)
    dx = np.diff(f, axis=1)
    dy = np.diff(f, axis=0)
    local_noise = float(np.mean(dx * dx) + np.mean(dy * dy))
    global_var = float(np.var(f)) + 1e-3
    return local_noise / global_var  # lower is better


def autotune_bank_config(color_plane: np.ndarray, width: int, height: int,
                          pitch: int, cpp: int = 4,
                          hbb_range=range(13, 18),
                          levels_range=range(0, 8),
                          modes=(MACROTILE_4_CHANNEL, MACROTILE_8_CHANNEL),
                          downscale: int = 1):
    """
    Tries every combination and returns (best_params_dict, all_results_sorted).
    Pass downscale>1 (e.g. 4) to only decode a subsampled width/height for speed
    while searching -- NOTE: because the swizzle isn't scale-invariant, prefer
    downscale=1 unless the search is too slow.
    """
    w, h = width, height
    results = []
    for mode in modes:
        for hbb in hbb_range:
            for levels in levels_range:
                try:
                    frame = ubwc_detile(color_plane, w, h, pitch, cpp=cpp,
                                        highest_bank_bit=hbb,
                                        bank_swizzle_levels=levels,
                                        macrotile_mode=mode)
                except Exception:
                    continue
                score = _smoothness_score(frame)
                results.append({
                    "macrotile_mode": mode,
                    "highest_bank_bit": hbb,
                    "bank_swizzle_levels": levels,
                    "score": score,
                })
    results.sort(key=lambda r: r["score"])
    best = results[0] if results else None
    return best, results
