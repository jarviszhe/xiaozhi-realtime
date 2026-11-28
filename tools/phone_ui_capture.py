"""Operate the real board UI and verify/export its serial RGB565 snapshots.

No reset, NVS mutation, credentials, or synthetic UI state. pyserial and Pillow
are required for the CLI. Outputs include raw serial evidence and decoded PNGs.
"""
import argparse
import base64
import json
from pathlib import Path
import re
import time


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


class Snapshot:
    def __init__(self, header):
        match = re.fullmatch(
            r"UI_SNAPSHOT_BEGIN width=(\d+) height=(\d+) stride=(\d+) bytes=(\d+) fnv=([0-9a-f]{8})",
            header,
        )
        if not match:
            raise ValueError("invalid snapshot header")
        self.width, self.height, self.stride, size = map(int, match.groups()[:4])
        self.checksum = int(match[5], 16)
        if not (0 < self.width <= 1024 and 0 < self.height <= 1024
                and self.width * 2 <= self.stride <= 4096
                and size == self.stride * self.height):
            raise ValueError("invalid snapshot dimensions")
        self.data = bytearray()

    def chunk(self, line):
        _, offset, data = line.split()
        if int(offset) != len(self.data):
            raise ValueError("snapshot offset gap or duplicate")
        decoded = base64.b64decode(data, validate=True)
        if not decoded or len(decoded) > 192 or len(self.data) + len(decoded) > self.stride * self.height:
            raise ValueError("snapshot chunk overflow")
        self.data.extend(decoded)

    def finish(self, footer):
        if (footer != f"UI_SNAPSHOT_END fnv={self.checksum:08x}"
                or len(self.data) != self.stride * self.height
                or fnv1a(self.data) != self.checksum):
            raise ValueError("snapshot missing/truncated/corrupt")
        return bytes(self.data)

    def save(self, path):
        from PIL import Image
        # Native LVGL RGB565, little endian; no panel flush byte swapping.
        rgb = bytearray(self.width * self.height * 3)
        for y in range(self.height):
            for x in range(self.width):
                p = y * self.stride + x * 2
                value = self.data[p] | self.data[p + 1] << 8
                q = (y * self.width + x) * 3
                r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
                rgb[q:q + 3] = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        Image.frombytes("RGB", (self.width, self.height), bytes(rgb)).save(path)


class Console:
    def __init__(self, port, directory):
        import serial
        self.port = serial.Serial()
        self.port.port, self.port.baudrate, self.port.timeout = port, 115200, 0.1
        self.port.dtr = self.port.rts = False
        self.port.open()
        if hasattr(self.port, "set_buffer_size"):
            self.port.set_buffer_size(rx_size=262144, tx_size=8192)
        self.log = (directory / f"serial-{port}.log").open("a", encoding="utf8")
        self.timeline = (directory / f"{port}-timeline.jsonl").open("a", encoding="utf8")
        self.pending = bytearray()

    def close(self):
        self.port.close()
        self.log.close()
        self.timeline.close()

    def send(self, command):
        self.timeline.write(json.dumps({"time": time.time(), "command": command}) + "\n")
        self.timeline.flush()
        self.port.write((command + "\n").encode())

    def line(self):
        while b"\n" not in self.pending:
            data = self.port.read(max(1, self.port.in_waiting))
            if not data:
                return None
            self.pending.extend(data)
        line, _, self.pending = self.pending.partition(b"\n")
        line = line.decode("utf8", errors="replace").strip("\r")
        self.log.write(line + "\n")
        self.log.flush()
        if "UI_PIXELS" not in line:
            self.timeline.write(json.dumps({"time": time.time(), "line": line}, ensure_ascii=False) + "\n")
            self.timeline.flush()
        return line

    def collect(self, seconds, verbose=True):
        lines = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            line = self.line()
            if line is not None:
                lines.append(line)
                if any(fault in line for fault in ("stack overflow", "Guru Meditation", "assert failed", "panic_abort")):
                    raise RuntimeError(line)
                if verbose and any(tag in line for tag in ("UI_", "GATEWAY", "phone_ui", "panic", "assert", "Malloc")):
                    print(line, flush=True)
        return lines

    def screenshot(self, path):
        self.send("ui screenshot")
        deadline = time.monotonic() + 45
        shot = None
        while time.monotonic() < deadline:
            line = self.line()
            if line is None:
                continue
            # ESP console logs may surround the framed data; only anchored frames count.
            if line.startswith("UI_SNAPSHOT_BEGIN"):
                shot = Snapshot(line)
            elif line.startswith("UI_PIXELS") and shot:
                shot.chunk(line)
            elif line.startswith("UI_SNAPSHOT_END") and shot:
                shot.finish(line)
                shot.save(path)
                print(f"SNAPSHOT_PASS {path} {shot.width}x{shot.height} fnv={shot.checksum:08x}", flush=True)
                return shot
            elif line.startswith("UI_SNAPSHOT_ERROR") or any(fault in line for fault in ("panic", "assert failed", "stack overflow", "Guru Meditation")):
                raise RuntimeError(line)
        raise TimeoutError("real device snapshot timed out")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--wait", type=float, default=2)
    parser.add_argument("--screenshot")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    console = Console(args.port, args.output)
    try:
        console.collect(0.5, False)
        for command in args.command:
            console.send(command)
            console.collect(args.wait)
        if args.screenshot:
            console.screenshot(args.output / args.screenshot)
        console.send("ui status" if args.port == "COM3" else "gateway status")
        console.collect(1)
    finally:
        console.close()


if __name__ == "__main__":
    main()
