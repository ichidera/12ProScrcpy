#!/usr/bin/env python3
"""
decode_ubwc.py — PC-side UBWC decoder + live viewer
Receives raw GPU memory frames from fbstream6, decodes with the real
Adreno UBWC swizzle (see ubwc_tiling.py), and displays them.

v2 changes (see CHANGELOG.md):
  * The buffer is now treated as [UBWC meta/flag plane][tiled color plane],
    matching what the probe dump actually shows (small repeating meta
    bytes for the first ~76KB, real-looking pixel bytes only after that).
    The meta-plane size is computed from freedreno_layout.h's alignment
    constants instead of being ignored.
  * The old naive "64x16 contiguous tile, row-major" de-tiler is gone.
    It's replaced by ubwc_tiling.ubwc_detile(), a direct numpy port of the
    real per-pixel/per-block/per-macrotile xor-swizzle in fd6_tiled_memcpy.cc
    (get_pixel_offset / block_x_xormask / block_y_xormask / bank masking).
  * Added --cpp, --highest-bank-bit, --bank-swizzle-levels, --macrotile-mode
    since those aren't recoverable from the DRM modifier alone (0x...01 is
    just the generic "QCOM_COMPRESSED" tag — the actual per-SoC bank wiring
    isn't encoded in it).
  * Added --autotune, which grabs one frame, brute-forces the bank config
    against it, and prints the best guess (see ubwc_tiling.autotune_bank_config).
"""
import socket, struct, sys, time, argparse, os
import numpy as np

from ubwc_tiling import compute_meta_plane_size, ubwc_detile, autotune_bank_config, \
    MACROTILE_4_CHANNEL, MACROTILE_8_CHANNEL

try:
    import cv2
    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False
    print("OpenCV not available, install with: pip install opencv-python")

MAGIC = 0x46425354  # "FBST"


def recvn(sock, n):
    buf = bytearray(n)
    view = memoryview(buf)
    pos = 0
    while pos < n:
        r = sock.recv_into(view[pos:], n - pos)
        if r == 0:
            raise ConnectionError("Disconnected")
        pos += r
    return bytes(buf)


def try_linear(raw_bytes: bytes, W: int, H: int, pitch: int) -> np.ndarray:
    """Try rendering as linear BGRA/RGBA (kept as a fallback / sanity check)."""
    arr = np.frombuffer(raw_bytes, dtype=np.uint8)
    if len(arr) < pitch * H:
        return None
    frame = arr[:pitch * H].reshape(H, pitch // 4, 4)
    return frame[:, :W, :]


def looks_valid(frame: np.ndarray) -> bool:
    """Heuristic: a valid frame has some color variation."""
    if frame is None:
        return False
    sample = frame[::50, ::50, :3]
    variance = np.var(sample.astype(np.float32))
    return variance > 10.0


def decode_frame(raw: bytes, W: int, H: int, pitch: int, args) -> np.ndarray:
    """Slice off the UBWC meta plane, then run the real detile on the rest."""
    meta_size, meta_pitch, meta_height = compute_meta_plane_size(W, H, args.cpp)
    color_bytes = np.frombuffer(raw, dtype=np.uint8)
    if len(color_bytes) <= meta_size:
        raise ValueError(
            f"frame ({len(color_bytes)} bytes) is smaller than the computed "
            f"meta-plane size ({meta_size} bytes) -- capture is truncated"
        )
    color_plane = color_bytes[meta_size:]
    return ubwc_detile(
        color_plane, W, H, pitch, cpp=args.cpp,
        highest_bank_bit=args.highest_bank_bit,
        bank_swizzle_levels=args.bank_swizzle_levels,
        macrotile_mode=args.macrotile_mode,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='192.168.0.130')
    ap.add_argument('--port', type=int, default=5005)
    ap.add_argument('--width', type=int, default=1088)
    ap.add_argument('--height', type=int, default=2400)
    ap.add_argument('--pitch', type=int, default=4352)
    ap.add_argument('--cpp', type=int, default=4, help='bytes per pixel (4 for ARGB/RGBA8888)')
    ap.add_argument('--mode', choices=['auto', 'linear', 'ubwc'], default='ubwc')
    ap.add_argument('--highest-bank-bit', type=int, default=15,
                     help='per-SoC UBWC bank config; not derivable from the modifier, tune with --autotune')
    ap.add_argument('--bank-swizzle-levels', type=int, default=0x7,
                     help='bitmask of enabled bank-swizzle levels (bits 0,1,2), default all on')
    ap.add_argument('--macrotile-mode', choices=[MACROTILE_4_CHANNEL, MACROTILE_8_CHANNEL],
                     default=MACROTILE_8_CHANNEL,
                     help='4ch (older Adreno) or 8ch (a7xx-class) macrotile bank interleave')
    ap.add_argument('--autotune', action='store_true',
                     help='grab one frame, brute-force bank config against it, print the best guess, then exit')
    ap.add_argument('--save', help='Save first frame to PNG')
    ap.add_argument('--save-raw', help='Save raw (undecoded) frame bytes to PREFIX_NNNN.bin, '
                                        'for offline UBWC compression reverse-engineering')
    ap.add_argument('--save-raw-count', type=int, default=1,
                     help='how many frames to dump with --save-raw (default 1)')
    args = ap.parse_args()

    W, H, pitch = args.width, args.height, args.pitch
    frame_bytes = pitch * H
    print(f"Connecting to {args.host}:{args.port}")
    print(f"Frame: {W}x{H} pitch={pitch} = {frame_bytes/1e6:.1f} MB/frame")

    if args.save_raw:
        raw_dir = os.path.dirname(args.save_raw)
        if raw_dir:
            os.makedirs(raw_dir, exist_ok=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((args.host, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print("Connected")

    if args.autotune:
        hdr_raw = recvn(sock, 16)
        magic, fw, fh, fpitch = struct.unpack('<IIII', hdr_raw)
        if magic != MAGIC:
            print(f"Bad magic: 0x{magic:08x}")
            sys.exit(1)
        raw = recvn(sock, fpitch * fh)
        meta_size, _, _ = compute_meta_plane_size(fw, fh, args.cpp)
        color_bytes = np.frombuffer(raw, dtype=np.uint8)[meta_size:]
        print(f"Autotuning bank config against one {fw}x{fh} frame "
              f"(meta plane = {meta_size} bytes)...")
        best, results = autotune_bank_config(color_bytes, fw, fh, fpitch, cpp=args.cpp)
        print("\nTop 5 candidates (lower score = smoother/more plausible):")
        for r in results[:5]:
            print(f"  {r}")
        print(f"\nBest guess: --highest-bank-bit {best['highest_bank_bit']} "
              f"--bank-swizzle-levels {best['bank_swizzle_levels']} "
              f"--macrotile-mode {best['macrotile_mode']}")
        sock.close()
        return

    frame_count = 0
    t0 = time.time()
    mode_detected = args.mode

    while True:
        hdr_raw = recvn(sock, 16)
        magic, fw, fh, fpitch = struct.unpack('<IIII', hdr_raw)
        if magic != MAGIC:
            print(f"Bad magic: 0x{magic:08x}")
            break

        fsz = fpitch * fh
        raw = recvn(sock, fsz)

        if args.save_raw and frame_count < args.save_raw_count:
            raw_path = f"{args.save_raw}_{frame_count:04d}.bin"
            with open(raw_path, 'wb') as f:
                # Header first (magic/w/h/pitch), then the exact raw payload,
                # so a saved file is fully self-describing for later analysis.
                f.write(hdr_raw)
                f.write(raw)
            print(f"Saved raw frame {frame_count} ({len(raw)} bytes payload) to {raw_path}")
            if frame_count + 1 >= args.save_raw_count:
                print("--save-raw-count reached, closing connection (skip decode/display).")
                sock.close()
                return

        t_decode = time.time()

        if mode_detected == 'auto' or mode_detected == 'linear':
            frame = try_linear(raw, fw, fh, fpitch)
            if mode_detected == 'auto' and frame_count == 0:
                if looks_valid(frame):
                    mode_detected = 'linear'
                    print("Auto-detected: LINEAR buffer (not UBWC-tiled)")
                else:
                    mode_detected = 'ubwc'
                    print("Auto-detected: UBWC tiled — applying real de-tiler")

        if mode_detected == 'ubwc':
            frame = decode_frame(raw, fw, fh, fpitch, args)

        decode_ms = (time.time() - t_decode) * 1000

        frame_count += 1
        elapsed = time.time() - t0
        fps = frame_count / elapsed

        if frame_count % 10 == 0:
            print(f"Frame {frame_count}: {fps:.1f} fps | decode {decode_ms:.1f}ms | mode={mode_detected}")

        if args.save and frame_count == 1:
            if HAS_CV2:
                bgr = cv2.cvtColor(frame, cv2.COLOR_RGBA2BGR)
                cv2.imwrite(args.save, bgr)
                print(f"Saved first frame to {args.save}")
            else:
                with open(args.save + '.raw', 'wb') as f:
                    f.write(frame.tobytes())
                print(f"Saved raw to {args.save}.raw")

        if HAS_CV2:
            display = cv2.cvtColor(frame, cv2.COLOR_RGBA2BGR)
            scale = 400.0 / fw
            dw, dh = int(fw * scale), int(fh * scale)
            display = cv2.resize(display, (dw, dh))
            cv2.imshow('fbstream', display)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break
        else:
            if frame_count >= 30:
                print(f"\n30 frames: avg {fps:.1f} fps, avg decode {decode_ms:.1f}ms")
                break

    sock.close()
    if HAS_CV2:
        cv2.destroyAllWindows()


if __name__ == '__main__':
    main()
