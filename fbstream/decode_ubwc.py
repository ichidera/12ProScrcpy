#!/usr/bin/env python3
"""
decode_ubwc.py — PC-side UBWC decoder + live viewer
Receives raw GPU memory frames from fbstream6, attempts to decode and display.

Strategy:
  1. Try rendering buffer as-is (linear) — if it looks right, done.
  2. If it looks like UBWC tiles, apply Qualcomm UBWC de-tiling.
  3. Use numpy for fast CPU decode, OpenCV for display.

UBWC Adreno 7xx tile layout for ARGB8888:
  - Macrotile size: 64 wide x 16 tall pixels
  - Tiles stored row-major
  - Within each tile: linear (no sub-tile swizzle on A7xx for RGBA)
  - Separate metadata buffer (not in our stream — handled by display HW)
"""
import socket, struct, sys, time, argparse
import numpy as np

try:
    import cv2
    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False
    print("OpenCV not available, install with: pip install opencv-python")

MAGIC = 0x46425354  # "FBST"
TILE_W = 64
TILE_H = 16

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

def ubwc_detile(raw: np.ndarray, W: int, H: int, pitch: int) -> np.ndarray:
    """
    De-tile UBWC ARGB8888 buffer.
    Input:  raw bytes, shaped as (H, pitch/4) uint32 array
    Output: linear (H, W, 4) uint8 array
    
    Each macrotile is TILE_W x TILE_H pixels stored contiguously.
    Macrotiles are arranged row-major across the image.
    """
    # Number of tiles
    tiles_x = (W + TILE_W - 1) // TILE_W
    tiles_y = (H + TILE_H - 1) // TILE_H

    # Reinterpret as bytes
    src = raw.view(np.uint8).reshape(-1)
    out = np.zeros((H, W, 4), dtype=np.uint8)

    tile_bytes = TILE_W * TILE_H * 4  # bytes per tile

    for ty in range(tiles_y):
        for tx in range(tiles_x):
            tile_idx = ty * tiles_x + tx
            tile_off = tile_idx * tile_bytes

            # Pixel bounds
            px = tx * TILE_W
            py = ty * TILE_H
            pw = min(TILE_W, W - px)
            ph = min(TILE_H, H - py)

            if tile_off + tile_bytes > len(src):
                break

            tile_data = src[tile_off:tile_off + tile_bytes]
            tile_2d = tile_data.reshape(TILE_H, TILE_W, 4)
            out[py:py+ph, px:px+pw] = tile_2d[:ph, :pw]

    return out

def try_linear(raw_bytes: bytes, W: int, H: int, pitch: int) -> np.ndarray:
    """Try rendering as linear BGRA/RGBA."""
    arr = np.frombuffer(raw_bytes, dtype=np.uint8)
    if len(arr) < pitch * H:
        return None
    frame = arr[:pitch*H].reshape(H, pitch//4, 4)
    return frame[:, :W, :]   # strip padding

def looks_valid(frame: np.ndarray) -> bool:
    """Heuristic: a valid frame has some color variation."""
    if frame is None: return False
    # Sample a grid of pixels
    sample = frame[::50, ::50, :3]
    variance = np.var(sample.astype(np.float32))
    return variance > 10.0  # threshold: some color variation expected

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='192.168.0.130')
    ap.add_argument('--port', type=int, default=5005)
    ap.add_argument('--width',  type=int, default=1280)
    ap.add_argument('--height', type=int, default=2576)
    ap.add_argument('--pitch',  type=int, default=5120)
    ap.add_argument('--mode', choices=['auto','linear','ubwc'], default='auto')
    ap.add_argument('--save', help='Save first frame to PNG')
    args = ap.parse_args()

    W, H, pitch = args.width, args.height, args.pitch
    frame_bytes = pitch * H
    print(f"Connecting to {args.host}:{args.port}")
    print(f"Frame: {W}x{H} pitch={pitch} = {frame_bytes/1e6:.1f} MB/frame")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((args.host, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print("Connected")

    frame_count = 0
    t0 = time.time()
    mode_detected = args.mode

    while True:
        # Read header
        hdr_raw = recvn(sock, 16)
        magic, fw, fh, fpitch = struct.unpack('<IIII', hdr_raw)
        if magic != MAGIC:
            print(f"Bad magic: 0x{magic:08x}")
            break

        # Read frame data
        fsz = fpitch * fh
        raw = recvn(sock, fsz)

        # Decode
        t_decode = time.time()

        if mode_detected == 'auto' or mode_detected == 'linear':
            frame = try_linear(raw, fw, fh, fpitch)
            if mode_detected == 'auto' and frame_count == 0:
                if looks_valid(frame):
                    mode_detected = 'linear'
                    print("Auto-detected: LINEAR buffer (not UBWC-tiled)")
                else:
                    mode_detected = 'ubwc'
                    print("Auto-detected: UBWC tiled — applying de-tiler")

        if mode_detected == 'ubwc':
            arr = np.frombuffer(raw, dtype=np.uint8)
            raw_arr = arr[:fpitch*fh].reshape(fh, fpitch//4, 4)
            frame = ubwc_detile(raw_arr, fw, fh, fpitch)
        
        decode_ms = (time.time() - t_decode) * 1000

        frame_count += 1
        elapsed = time.time() - t0
        fps = frame_count / elapsed

        if frame_count % 10 == 0:
            print(f"Frame {frame_count}: {fps:.1f} fps | decode {decode_ms:.1f}ms | mode={mode_detected}")

        if args.save and frame_count == 1:
            if HAS_CV2:
                # Convert RGBA→BGR for OpenCV save
                bgr = cv2.cvtColor(frame, cv2.COLOR_RGBA2BGR)
                cv2.imwrite(args.save, bgr)
                print(f"Saved first frame to {args.save}")
            else:
                # Raw save
                with open(args.save + '.raw', 'wb') as f:
                    f.write(frame.tobytes())
                print(f"Saved raw to {args.save}.raw")

        if HAS_CV2:
            # Scale down for display (phone is tall)
            display = cv2.cvtColor(frame, cv2.COLOR_RGBA2BGR)
            scale = 400.0 / fw
            dw, dh = int(fw*scale), int(fh*scale)
            display = cv2.resize(display, (dw, dh))
            cv2.imshow('fbstream', display)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break
        else:
            # No display — just benchmark
            if frame_count >= 30:
                print(f"\n30 frames: avg {fps:.1f} fps, avg decode {decode_ms:.1f}ms")
                break

    sock.close()
    if HAS_CV2: cv2.destroyAllWindows()

if __name__ == '__main__':
    main()
