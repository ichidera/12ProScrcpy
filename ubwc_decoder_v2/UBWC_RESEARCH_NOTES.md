# UBWC Framebuffer Capture — Research Notes

Handoff doc for anyone picking this up. Covers what we tried, what we
ruled out and why, what we've confirmed, and what's still open. Read this
before re-deriving anything below — most of the dead ends here took a full
investigation loop to close out.

Companion file: `CHANGELOG.md` has the same information as discrete,
dated iterations if you want the blow-by-blow instead of the narrative.

---

## 1. The goal

Capture the phone's screen content with **as close to zero added latency
and device load as possible** — explicitly *not* willing to trade that
for convenience (this ruled out two otherwise-reasonable approaches; see
§4). "PC has plenty of compute, phone does the bare minimum" is the
standing design constraint.

The phone's primary display framebuffer is captured via raw DRM ioctls
(`fbstream.c`, phone side) — `mmap`ing the GEM buffer behind the active
scanout plane and streaming the raw bytes over TCP to a PC-side Python
decoder (`decode_ubwc.py` + `ubwc_tiling.py`).

The obstacle: that framebuffer isn't stored as plain RGBA. It's in
Qualcomm's **UBWC (Universal Bandwidth Compression)** format — a
proprietary tiled + compressed layout Adreno GPUs use for scanout buffers
to save memory bandwidth. Decoding it turned out to have two separable
problems, described below.

---

## 2. Two separate problems, solved in two different ways

UBWC has:

1. **A tiling/addressing scheme** — pixels aren't stored in row-major
   order. They're grouped into 16x4-pixel blocks (for 32bpp formats),
   blocks are arranged into "macrotiles," and macrotile positions are
   further XOR-swizzled based on a per-SoC "bank config" to spread memory
   traffic across DRAM channels evenly.
2. **Lossless block compression** — each block's actual color data may be
   stored in a compressed (bit-packed) form rather than as literal bytes,
   selected per-block based on content, with a "meta"/"flag" byte per
   block indicating (among other things) which encoding was used.

**Problem 1 (addressing) is solved.** It's public, documented math, and we
ported it directly from real Mesa/Freedreno source (`fd6_tiled_memcpy.cc`,
`freedreno_layout.h/c`, provided by the user). See §5.

**Problem 2 (compression) is not solved in general.** There is no public
reference decoder for it anywhere in the open-source graphics stack —
Mesa/Freedreno never implement it because the GPU texture unit and display
controller decode it transparently in fixed-function hardware. We've
reverse-engineered two specific special-case codes empirically (see §7),
but the general case remains open.

---

## 3. Architecture / file map

```
fbstream.c              phone side: raw DRM plane -> GEM handle -> mmap -> TCP stream
                         (NOT modified during this investigation — capture logic
                         stayed intentionally dumb per the zero-phone-load goal)

decode_ubwc.py           PC side: receives frames, slices meta plane off the front,
                          calls ubwc_tiling.ubwc_detile(), displays/saves via OpenCV

ubwc_tiling.py            Core math module:
                            - compute_meta_plane_size() — meta/flag plane sizing
                            - meta_grid() — slice+reshape the meta plane into a
                              (blocks_h, blocks_w) grid
                            - get_pixel_offset(), block_x_xormask(), block_y_xormask(),
                              get_bank_mask(), get_bank_shift() — ported verbatim from
                              fd6_tiled_memcpy.cc
                            - ubwc_detile() — full address-based tiled->linear decode,
                              now with a meta-aware override for the two solved codes
                            - autotune_bank_config() — brute-forces the per-SoC bank
                              config against a captured frame (see §6)
                            - META_ALL_ZERO / META_ALL_FF constants (§7)

inspect_meta.py           Offline analysis tool for raw captures: --histogram shows
                          meta-byte distribution, --block X Y dumps one block's meta
                          byte + raw 256 bytes + a naive pixel-mapped view

compare_captures.py       Diffs meta-byte grids across N captures at once — isolates
                          which blocks actually vary (real content) vs. fixed screen
                          chrome, and prints a side-by-side histogram

probe_writeback.c         Phone-side: checks for a DRM writeback connector (§4)
probe_v4l2.c              Phone-side: identifies /dev/videoN nodes via VIIOC_QUERYCAP (§4)

patterns/*.html           Controlled test content (solid colors, gradients, stripes)
                          served over HTTP from the PC and opened in Chrome on the
                          phone, for the empirical reverse-engineering loop (§7)
```

---

## 4. Approaches considered and explicitly rejected

Documenting these so they don't get re-proposed and re-investigated from
scratch.

### 4.1 GPU blit + `glReadPixels`

**Idea:** import the dma-buf as a GL texture, blit to a linear FBO
(decompression happens for free via the texture sampler), `glReadPixels`
back.

**Rejected because:** `glReadPixels` is a synchronous GPU→CPU pipeline
stall, and the blit itself costs shader/ROP cycles and memory bandwidth
every frame. Directly conflicts with the zero-load/zero-latency goal.
Never implemented.

### 4.2 Hardware video encoder (Venus / `msm_vidc`)

**Idea:** feed the same dma-buf directly into `/dev/video33`
(`msm_vidc_encoder`, confirmed present via `probe_v4l2.c`, see §4.3) as a
V4L2 M2M input buffer, since video encoders on this SoC accept UBWC input
natively and decompress it as part of normal reference-frame handling.
Output would be H.264/HEVC — tiny payload, trivial PC-side decode.

**Rejected because:** even though it's a dedicated ASIC (not the CPU, not
the 3D GPU), it still has to read the full framebuffer off DRAM every
frame, and it's an additional hardware block with its own power/thermal
footprint contending for shared memory bandwidth with a running foreground
app (e.g. a game). The user's call, based on direct experience/intuition
about this specific hardware's behavior under load — logged here as a
considered-and-rejected option, not as a settled technical fact. (Counter-
argument that was raised and acknowledged but didn't change the decision:
the current raw-CPU-mmap-and-stream approach *also* reads the same
~10MB/frame off DRAM, so the DRAM traffic itself isn't actually avoided
either way — the difference is what happens to the data afterward. Worth
revisiting with real measurements if the current approach turns out to
cause its own problems.)

Never implemented beyond the feasibility probe.

### 4.3 DRM writeback connector

**Idea:** some Qualcomm SoCs expose a dedicated fixed-function display
writeback path that can capture a composited frame directly to a linear
buffer — since the display controller already decompresses UBWC to scan
it out, a writeback connector would get us already-decompressed pixels for
free, no GPU shaders, no CPU decode.

**Investigated with `probe_writeback.c`.** Result: **not available on
this device.** Only two connectors exist on `/dev/dri/card0`: `VIRTUAL`
and `DSI`. No `card1` either. Dead end, confirmed empirically, don't
re-check without new hardware.

### 4.4 Hardware rotator / format-converter

**Idea:** look for a dedicated ASIC (rotator, C2D, etc.) that does
format/rotation conversion and might read UBWC and write linear as a side
effect.

**Investigated with `probe_v4l2.c`.** Result: `/dev/video0`/`1` are camera
ISP pipeline nodes. `/dev/video32`/`33` are `msm_vidc_decoder`/
`msm_vidc_encoder` — i.e. the Venus *video codec* hardware (§4.2), not a
rotator. No dedicated format-converter node found on this device.

**Conclusion of §4:** no near-zero-load hardware shortcut exists on this
specific phone. Staying with the original raw-CPU-capture design and
solving the compression in software on the PC is the only path consistent
with the stated constraints, however much harder that makes it.

---

## 5. Solved: tiling/addressing math (ported, not reverse-engineered)

This part is not reverse-engineered — it's a direct translation of public
Mesa/Freedreno source the user provided (`fd6_tiled_memcpy.cc`,
`freedreno_layout.h`, `freedreno_layout.c`):

- `get_pixel_offset(x, y)` — swizzle of a pixel's position within its
  16x4-pixel block.
- `block_x_xormask()` / `block_y_xormask()` — swizzle of a block's
  position within its macrotile, for both 4-channel and 8-channel
  macrotile bank-interleave modes.
- `fdl6_get_bank_mask()` / `fdl6_get_bank_shift()` — the DRAM-bank-conflict
  avoidance swizzle, parameterized by a per-SoC `highest_bank_bit` and
  `bank_swizzle_levels`.

All ported verbatim into `ubwc_tiling.py`, vectorized with numpy
(whole-frame decode in ~0.1s). **Confirmed correct**: once applied, a
captured screenshot's UI structure (icon positions, text line layout,
spacing) lines up correctly with the real screen — see the "before/after"
comparison in the chat history (naive tiling produced vertical-stripe
noise; real math produced a correctly-laid-out but still content-noisy
image).

### 5.1 Meta (flag) plane sizing — reconstructed, not ported

The buffer layout is `[UBWC meta plane][tiled color plane]`, not just raw
tiled color data. The function that computes the real meta plane size
(`fdl6_layout_image()`) was **not** in the provided sources — only its
prototype. `compute_meta_plane_size()` in `ubwc_tiling.py` is a
reconstruction from the alignment constants that *are* present
(`RGB_TILE_WIDTH_ALIGNMENT=64`, `RGB_TILE_HEIGHT_ALIGNMENT=16`,
`UBWC_PLANE_SIZE_ALIGNMENT=4096`) plus the per-cpp block-size table from
`get_block_size()`. For the device's `1088x2400`, `cpp=4` buffer this
comes out to **77,824 bytes**, which matches the probe dump boundary
exactly (repeating meta-style bytes out to that offset, real-looking pixel
bytes after). Treat this formula as "best available from what we have,"
not a byte-exact copy of vendor code.

### 5.2 Per-SoC bank config — found via brute force, not derived

`highest_bank_bit`, `bank_swizzle_levels`, and `macrotile_mode` (4ch vs.
8ch) are kernel/SoC-specific choices not recoverable from the DRM modifier
(`0x0500000000000001` is just the generic "this is UBWC" vendor tag, no
per-SoC info encoded). `autotune_bank_config()` brute-forces all
combinations (80 total) against a captured frame, scoring by
local-smoothness-relative-to-global-variance (a real decode should look
locally smoother than a wrong permutation, while still having real
content variance — this rules out picking a degenerate flat/blank result).

**Result for this device:** `highest_bank_bit=13`, `macrotile_mode=8ch`.
`bank_swizzle_levels` turned out not to matter at this `highest_bank_bit`
— the macrotile_stride for this buffer's pitch works out to **17** (odd),
and none of the three bank-swizzle alignment checks
(`stride & ((1<<shift)-1) == 0`) can pass when bit 0 of the stride is set,
so the bank-swizzle term is always zero regardless of which levels are
enabled. This is a real property of this framebuffer's pitch, not a
search artifact — confirmed by the autotune output showing five different
`bank_swizzle_levels` values tied at the identical score.

---

## 6. Practical bugs fixed along the way (not UBWC-related, but will bite you again if you don't know about them)

- **`fbstream6` only grabs its GEM handle once, at startup.** It does not
  re-check which buffer in the compositor's buffer pool is actually live.
  Leaving one instance running across multiple test captures caused every
  capture after the first to silently read a stale, frozen buffer — we
  lost a full round of test-pattern data to this before catching it (the
  tell: four captures of four different on-screen patterns produced
  byte-identical meta histograms). **Always restart `fbstream6` fresh,
  after the new content is already on screen, for every single capture.**
  This is a real design gap in `fbstream.c` worth fixing properly
  (periodically re-resolve the live FB instead of grabbing once) if this
  capture approach continues to be used — not yet done.
- `DRM_IOCTL_MODE_GETRESOURCES`'s two-pass ioctl convention requires
  allocating buffers for *all four* id lists (fb/crtc/encoder/connector)
  matching the counts pass 1 returns, even if you only care about one of
  them — a non-zero count with a null pointer is an instant `EFAULT`.
- `am start -d file:///...` silently does nothing when handing a local
  file to another app via a VIEW intent — Android blocks this
  (scoped-storage-adjacent restriction), and Chrome just ignores it with
  no visible error. Serving test pages over HTTP from the PC
  (`python -m http.server`) and opening `http://PC_IP:PORT/...` instead
  works normally.

---

## 7. The empirical reverse-engineering loop (compression codes)

Methodology: display known, controlled content on the phone (solid
colors, gradients, stripe patterns — see `patterns/*.html`), capture the
raw bytes (`decode_ubwc.py --save-raw`), and correlate the per-block meta
byte against what's actually on screen (`inspect_meta.py`,
`compare_captures.py`).

### 7.1 First (wrong) hypothesis: luma-driven code selection

Initial data (black, red, white, gray128, green, blue, gradient, checker)
showed:
- `0x05` only in solid black
- `0x0d` only in white/gradient/fine-stripes (all "white-involving")
- `0x11` shared by red, green, blue, *and* gray128

Red/green/blue are channel-extreme (one channel at 255) same as white, but
grouped with mid-value gray instead. Converting to standard luma
(`Y = 0.299R + 0.587G + 0.114B`) seemed to explain it: black=0, white=255
(the extremes get special codes), red≈76/green≈150/blue≈29/gray=128 are
all mid-range (share the generic code) — plausible, and consistent with a
Qualcomm patent lead (US20220254070A1) describing a YCoCg-R transform +
variable-bit-width packing per block, where luma is the first transformed
channel.

**This was wrong.** Targeted test colors chosen specifically by luma value
(not by how saturated they look) disproved it cleanly:
- `darkgray10` (#0a0a0a, Y≈10) → generic `0x11`, not `0x05`
- `lightgray245` (#f5f5f5, Y≈245) → generic `0x11`, not `0x0d`
- `yellow` (#ffff00, Y≈226, near-white luma despite a saturated hue) →
  generic `0x11`, not `0x0d`

Luma is not the driver. Recorded here specifically so this dead end isn't
re-derived.

### 7.2 Confirmed rule: exact all-zero / all-FF special cases

Across 11 test colors, the actual rule is much simpler and, unlike the
luma theory, **deterministic**:

| Meta byte | Condition | Confidence |
|---|---|---|
| `0x05` | Block is byte-for-byte all `0x00` | Confirmed — appeared *only* for literal solid black across all 11 test colors, including near-black grays |
| `0x0d` | Block is byte-for-byte all `0xFF` | Confirmed — appeared *only* for literal solid white across all 11 test colors, including near-white grays and near-white-luma yellow |
| `0x11` | Generic flat/simple block requiring an actual stored base value | Everything else that isn't one of the two trivial cases above — confirmed as "not special," meaning not yet decoded |
| `0x13`, `0x15`, `0x17` | Unknown, likely higher-complexity/multi-value blocks | Seen only in small counts (single digits to low hundreds) even in solid-color captures — almost certainly anti-aliased/boundary blocks where the fill meets adjacent chrome, or where the browser blends edges. **Not decoded.** |

This is not a fuzzy correlation — it's a bit-exact, directly implementable
rule, unlike the luma theory. **Implemented** in
`ubwc_tiling.ubwc_detile()`: when a `meta` grid is passed in, any block
tagged `META_ALL_ZERO` is forced to literal `0x00`, any block tagged
`META_ALL_FF` is forced to literal `0xFF`, on top of the existing
address-only decode for every other code. `decode_ubwc.py` wires this
through automatically.

**Verified working**: a live capture of a mostly-black real screen
(`first_frame_v3.png`) shows the black regions rendering cleanly, where
they were speckled before this fix.

### 7.3 What's still noisy, and why that's expected (not a regression)

A busier real screen (an app drawer with Android's blurred-wallpaper-
behind-a-translucent-scrim background) still shows speckle noise. This is
**expected, not a bug**: that background is never actually flat white —
it's a blur + scrim effect with real wallpaper color bleeding through, so
those blocks were never going to hit the `0x05`/`0x0d` special cases. They
fall through to the still-undecoded generic codes (`0x11`/`0x13`/`0x15`/
`0x17`), same as before this fix. Confirmed the noise pattern lines up
positionally with the real icon grid columns in the reference screenshot
— i.e. it's content-correlated noise from the unsolved generic codes, not
a new addressing bug.

---

## 8. Open questions / next steps

1. **Crack the generic `0x11` code.** This is the majority of real
   content and the highest-value remaining target. No data yet beyond
   "it's not one of the two trivial cases." Needs test patterns with
   controlled *non-flat* content at sub-block granularity (the
   `pattern_stripes_fine.html` 2px-stripe pattern was a start, but hasn't
   been analyzed in detail yet — do that next before designing further
   patterns).
2. **Characterize `0x13`/`0x15`/`0x17`.** Hypothesis: escalating
   "complexity tiers" (more distinct values / wider range within the
   block needing progressively more storage), but unconfirmed. Look at
   `--block` dumps for blocks tagged with these codes specifically at
   pattern edges/boundaries (e.g. where a fine-stripe pattern's block
   contains both a black run and a white run) to test this.
3. **Confirm the reddish-brown band** seen near the bottom of
   `first_frame_v3.png` — not yet investigated, unclear if it's a
   meta-plane edge case, a padding/alignment issue, or unrelated.
4. **`fbstream.c`'s one-shot buffer grab (§6)** is a real gap if this
   capture approach continues — worth fixing to periodically re-resolve
   the live FB rather than assuming the first grab stays valid forever,
   independent of the UBWC work.

---

## 9. Quick reference: how to run a test-pattern capture round

```
# One-time: serve patterns over HTTP from the PC (avoids the file:// block, §6)
cd patterns && python -m http.server 8000

# Per pattern (repeat this three-step sequence for every pattern):
adb shell am start -a android.intent.action.VIEW -d http://PC_IP:8000/pattern_NAME.html
#  ...then on the phone (root shell), kill any previous fbstream6 and restart it fresh:
/data/local/tmp/fbstream6 5005
#  ...then on the PC:
python decode_ubwc.py --host PHONE_IP --port 5005 --save-raw captures/NAME --save-raw-count 1
#  ...Ctrl+C the phone-side fbstream6 before moving to the next pattern.

# Analysis:
python inspect_meta.py captures/NAME_0000.bin --histogram
python inspect_meta.py captures/NAME_0000.bin --block X_BLOCK Y_BLOCK [--expect-rgba R G B A]
python compare_captures.py captures/a_0000.bin captures/b_0000.bin ...
```
