#!/usr/bin/env python3
"""
Stream a video file to AgentCube via ACSP (atomic present).

Protocol: TCP :81, FRAME 80×80 RGB565 LE → cube scales 3× to 240×240.
Old frame stays on panel until the new one is fully received, then SPI-burst.

Requires: ffmpeg (brew install ffmpeg)

Usage:
  python3 scripts/stream_video_to_cube.py \\
      --host 192.168.1.97 \\
      --video /path/to/clip.mp4 \\
      --fps 12

  # loop forever
  python3 scripts/stream_video_to_cube.py -H 192.168.1.97 -i clip.mp4 --loop

  # solid test pattern
  python3 scripts/stream_video_to_cube.py -H 192.168.1.97 --test
"""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import time

MAGIC = 0x50534341  # 'ACSP'
VER = 1
CMD_FRAME = 1
CMD_CLEAR = 2
CMD_END = 4
W = 80
H = 80
FRAME_BYTES = W * H * 2


def pack_hdr(cmd: int, seq: int, x: int, y: int, w: int, h: int, plen: int) -> bytes:
    return struct.pack("<IBBHhhHHI", MAGIC, VER, cmd, seq & 0xFFFF, x, y, w, h, plen)


def connect(host: str, port: int) -> socket.socket:
    s = socket.create_connection((host, port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(10)
    return s


def wait_ack(s: socket.socket) -> None:
    while True:
        b = s.recv(1)
        if not b:
            raise ConnectionError("connection closed waiting ACK")
        if b == b"\x06":
            return


def send_frame(s: socket.socket, seq: int, rgb565: bytes) -> None:
    assert len(rgb565) == FRAME_BYTES
    s.sendall(pack_hdr(CMD_FRAME, seq, 0, 0, W, H, FRAME_BYTES))
    s.sendall(rgb565)
    # Firmware ACKs after atomic present
    wait_ack(s)


def send_clear(s: socket.socket, seq: int, color: int = 0) -> None:
    s.sendall(pack_hdr(CMD_CLEAR, seq, 0, 0, 0, 0, 2) + struct.pack("<H", color))
    wait_ack(s)


def test_pattern(seq: int, t: float) -> bytes:
    """Moving color bars — proves overlay/atomic present without a file."""
    out = bytearray()
    ox = int(t * 40) % W
    for y in range(H):
        for x in range(W):
            xx = (x + ox) % W
            r = (xx * 255) // (W - 1)
            g = (y * 255) // (H - 1)
            b = int(80 + 40 * (1 if (xx // 16 + y // 16) % 2 == 0 else 0))
            c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out += struct.pack("<H", c)
    return bytes(out)


def open_ffmpeg(path: str, fps: float) -> subprocess.Popen:
    # scale+pad to exact 80x80, rgb565le raw frames
    vf = f"fps={fps},scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2"
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-re",  # realtime pacing
        "-i",
        path,
        "-vf",
        vf,
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb565le",
        "-an",
        "pipe:1",
    ]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main() -> int:
    ap = argparse.ArgumentParser(description="Stream video to AgentCube (ACSP 80×80)")
    ap.add_argument("-H", "--host", default="192.168.1.97", help="cube IP")
    ap.add_argument("-p", "--port", type=int, default=81, help="ACSP TCP port")
    ap.add_argument("-i", "--video", default=None, help="video file (mp4/mkv/…)")
    ap.add_argument("--fps", type=float, default=12.0, help="target stream FPS (ffmpeg)")
    ap.add_argument("--loop", action="store_true", help="replay video forever")
    ap.add_argument("--test", action="store_true", help="moving test pattern, no file")
    ap.add_argument("--no-clear", action="store_true", help="do not black screen at start")
    args = ap.parse_args()

    if not args.test and not args.video:
        ap.error("provide --video PATH or --test")

    print(f"Connecting {args.host}:{args.port} …")
    s = connect(args.host, args.port)
    seq = 0
    if not args.no_clear:
        send_clear(s, seq)
        seq += 1
        print("cleared")

    if args.test:
        print("Test pattern — Ctrl+C to stop")
        n = 0
        t0 = time.perf_counter()
        try:
            while True:
                frame = test_pattern(seq, time.perf_counter() - t0)
                send_frame(s, seq, frame)
                seq += 1
                n += 1
                if n % 30 == 0:
                    dt = time.perf_counter() - t0
                    print(f"  ~{n / dt:.1f} FPS effective ({n} frames)")
        except KeyboardInterrupt:
            print("\nstop")
        s.close()
        return 0

    while True:
        print(f"ffmpeg → {args.video} @ {args.fps} fps, {W}x{H} rgb565")
        proc = open_ffmpeg(args.video, args.fps)
        assert proc.stdout is not None
        n = 0
        t0 = time.perf_counter()
        try:
            while True:
                raw = proc.stdout.read(FRAME_BYTES)
                if not raw or len(raw) < FRAME_BYTES:
                    break
                send_frame(s, seq, raw)
                seq += 1
                n += 1
                if n % 30 == 0:
                    dt = time.perf_counter() - t0
                    print(f"  ~{n / dt:.1f} FPS effective ({n} frames)")
        except (BrokenPipeError, ConnectionError) as e:
            print("stream error:", e)
            proc.kill()
            s.close()
            return 1
        finally:
            proc.kill()
            try:
                proc.wait(timeout=2)
            except Exception:
                pass

        print(f"done {n} frames")
        if not args.loop:
            break
        print("loop…")

    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
