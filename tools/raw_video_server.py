#!/usr/bin/env python3
"""Low-latency playback server for the OBS raw-tcp-udp-output plugin (Windows).

Listens on a single port for both TCP and UDP; whichever protocol delivers
data first claims the session. When the stream ends (TCP disconnect or UDP
idle timeout) the server returns to waiting, so OBS can stop and restart
freely.

Wire format (must match src/raw-tcp-udp-output.c):

    [4 bytes payload size, big-endian]
    [1 byte  frame type: 1 = keyframe]
    [8 bytes DTS, big-endian]
    [payload: encoded video bitstream, e.g. H.264 Annex B]

TCP carries these records back to back. UDP carries the same byte stream
split into <=1400-byte datagrams, reassembled here in arrival order.

Payloads are piped into ffplay as a raw elementary stream. ffplay is run
with every buffering stage disabled (-fflags nobuffer, -probesize 32,
-analyzeduration 0) and frames are displayed as soon as they decode
(setpts=0), which keeps glass-to-glass latency near the encoder's own
delay.

Usage:
    python raw_video_server.py [--port 9999] [--codec h264]

then point OBS at  udp://<this-host>:9999  or  tcp://<this-host>:9999.

Requires ffplay.exe on PATH (ships with ffmpeg builds), or pass --player.

Troubleshooting: if the ffplay window opens but never shows video, the
encoder is probably not repeating SPS/PPS headers in the stream. For the
x264 encoder add  repeat-headers=1  to its custom settings in OBS.
"""

import argparse
import selectors
import socket
import struct
import subprocess
import sys
import time

# status lines must not sit in the block buffer when output is redirected
sys.stdout.reconfigure(line_buffering=True)

HEADER = struct.Struct(">IBQ")  # payload size, keyframe flag, DTS

UDP_DRAIN_CHUNK = 65536
STATS_INTERVAL = 2.0


class Player:
    """ffplay child process fed through stdin."""

    def __init__(self, args):
        self.args = args
        self.proc = None

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def start(self):
        self.stop()
        cmd = [
            self.args.player,
            "-hide_banner", "-loglevel", "warning",
            # kill every buffering stage between the pipe and the screen
            "-fflags", "nobuffer",
            "-flags", "low_delay",
            "-probesize", "32",
            "-analyzeduration", "0",
            "-framedrop",
            # raw elementary streams carry no usable timestamps; render
            # each frame the moment it is decoded
            "-vf", "setpts=0",
            "-window_title", f"OBS raw stream ({self.args.codec})",
            "-f", self.args.codec,
            "-",
        ]
        try:
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, bufsize=0)
        except FileNotFoundError:
            sys.exit(f"error: '{self.args.player}' not found - install ffmpeg "
                     "(which includes ffplay) or pass --player")

    def write(self, data):
        """Returns False if the player is gone (e.g. window closed)."""
        if not self.running():
            return False
        try:
            self.proc.stdin.write(data)
            return True
        except OSError:
            self.stop()
            return False

    def stop(self):
        if self.proc is None:
            return
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.kill()
            self.proc.wait(timeout=2)
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.proc = None


class Session:
    """Reassembles the byte stream into frames and feeds the player."""

    def __init__(self, player, args):
        self.player = player
        self.args = args
        self.buf = bytearray()
        self.wait_keyframe = True
        self.frames = 0
        self.bytes = 0
        self._stats_t = time.monotonic()
        self._stats_frames = 0
        self._stats_bytes = 0
        print("  waiting for first keyframe...")

    def feed(self, data):
        """Returns False on desync; the caller should end the session."""
        self.buf += data
        while True:
            if len(self.buf) < HEADER.size:
                return True
            size, keyframe, _dts = HEADER.unpack_from(self.buf)
            if size == 0 or size > self.args.max_frame:
                print(f"\n  desync detected (frame size {size}), "
                      "resetting session")
                return False
            if len(self.buf) < HEADER.size + size:
                return True
            payload = bytes(self.buf[HEADER.size:HEADER.size + size])
            del self.buf[:HEADER.size + size]
            self._deliver(payload, keyframe)

    def _deliver(self, payload, keyframe):
        if self.wait_keyframe:
            if not keyframe:
                return
            self.wait_keyframe = False
            if not self.player.running():
                self.player.start()
        if not self.player.write(payload):
            # player window was closed; restart cleanly at the next keyframe
            self.wait_keyframe = True
            return

        self.frames += 1
        self.bytes += len(payload)
        now = time.monotonic()
        elapsed = now - self._stats_t
        if elapsed >= STATS_INTERVAL:
            fps = (self.frames - self._stats_frames) / elapsed
            mbps = (self.bytes - self._stats_bytes) * 8 / 1e6 / elapsed
            print(f"\r  {fps:5.1f} fps   {mbps:7.2f} Mb/s   "
                  f"{self.frames} frames", end="", flush=True)
            self._stats_t = now
            self._stats_frames = self.frames
            self._stats_bytes = self.bytes


def run_tcp_session(conn, addr, player, args):
    print(f"\n[tcp] {addr[0]}:{addr[1]} connected")
    session = Session(player, args)
    with conn:
        while True:
            try:
                data = conn.recv(65536)
            except OSError:
                break
            if not data:
                break
            if not session.feed(data):
                break
    player.stop()
    print("\n[tcp] stream ended, waiting...")


def run_udp_session(sock, player, args):
    session = None
    peer = None
    sock.settimeout(args.idle_timeout)
    try:
        while True:
            try:
                data, addr = sock.recvfrom(UDP_DRAIN_CHUNK)
            except socket.timeout:
                break
            except ConnectionResetError:
                # Windows surfaces ICMP port-unreachable as a recv error on
                # UDP sockets; harmless here
                continue
            if peer is None:
                peer = addr
                print(f"\n[udp] receiving from {addr[0]}:{addr[1]}")
                session = Session(player, args)
            elif addr != peer:
                continue  # ignore other senders for the session's duration
            if not session.feed(data):
                break
    finally:
        sock.settimeout(None)
        player.stop()
    print("\n[udp] stream idle, waiting...")


def drain_udp(sock):
    """Discard datagrams queued while another session was running."""
    sock.setblocking(False)
    try:
        while True:
            sock.recvfrom(UDP_DRAIN_CHUNK)
    except (BlockingIOError, ConnectionResetError):
        pass
    finally:
        sock.setblocking(True)


def main():
    parser = argparse.ArgumentParser(
        description="Play the OBS raw TCP/UDP video stream with low latency")
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--port", type=int, default=9999, help="bind port")
    parser.add_argument("--codec", default="h264",
                        choices=["h264", "hevc", "av1"],
                        help="codec produced by the OBS encoder")
    parser.add_argument("--player", default="ffplay",
                        help="path to ffplay.exe")
    parser.add_argument("--idle-timeout", type=float, default=5.0,
                        help="seconds of UDP silence that end a session")
    parser.add_argument("--max-frame", type=int, default=32 * 1024 * 1024,
                        help="frame size sanity limit (desync guard)")
    args = parser.parse_args()

    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp.bind((args.host, args.port))
    tcp.listen(1)

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # large kernel buffer so bursts (keyframes) survive while ffplay starts
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    udp.bind((args.host, args.port))

    sel = selectors.DefaultSelector()
    sel.register(tcp, selectors.EVENT_READ)
    sel.register(udp, selectors.EVENT_READ)

    player = Player(args)
    print(f"listening on {args.host}:{args.port} (tcp + udp), "
          f"codec={args.codec}")
    print(f"point OBS at  tcp://<host>:{args.port}  or  "
          f"udp://<host>:{args.port},  Ctrl+C to quit")

    try:
        while True:
            for key, _ in sel.select():
                if key.fileobj is tcp:
                    conn, addr = tcp.accept()
                    run_tcp_session(conn, addr, player, args)
                else:
                    run_udp_session(udp, player, args)
                # discard anything that piled up during the session so the
                # next one starts aligned on a frame header
                drain_udp(udp)
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        player.stop()
        tcp.close()
        udp.close()


if __name__ == "__main__":
    main()
