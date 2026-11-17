#!/usr/bin/env python3
#
# gen_bench_mixer_modules.py
# --------------------------
# Purpose: Generate deterministic XM tracks for mixer benchmarking.
# Notes  : Produces low/mid/high polyphony variants with looped samples and
#          frequent pattern changes. No third-party modules are required.
#

import argparse
import math
import struct
from pathlib import Path


def sample_values(count, salt, bits):
    limit = 32767 if bits == 16 else 127
    scale = 30000 if bits == 16 else 120
    result = []
    for i in range(count):
        value = (
            0.45 * math.sin(2.0 * math.pi * i / (73.0 + salt * 7.0))
            + 0.25 * math.sin(2.0 * math.pi * i / (191.0 + salt * 11.0))
            + 0.10 * math.sin(2.0 * math.pi * i / (17.0 + salt * 3.0))
        )
        value += ((i * 1103515245 + salt * 12345) % 2001 - 1000) / 1000.0 * 0.08
        result.append(max(-limit, min(limit, int(value * scale))))
    return result


def delta_encode(values, bits):
    mask = 0xFFFF if bits == 16 else 0xFF
    encoded = []
    previous = 0
    for value in values:
        encoded.append((value - previous) & mask)
        previous = value
    return encoded


def build_xm(channels, bits=16, stereo=False, rows=8, orders=64, sample_frames=8192):
    left = sample_values(sample_frames, 0, bits)
    right = sample_values(sample_frames, 3, bits)
    channels_data = [left, right] if stereo else [left]
    if bits == 16:
        sample = b"".join(struct.pack("<%dH" % sample_frames, *delta_encode(channel, bits)) for channel in channels_data)
        loop_bytes = sample_frames * 2
        sample_flags = 0x01 | 0x10 | (0x20 if stereo else 0)
    else:
        sample = b"".join(bytes(delta_encode(channel, bits)) for channel in channels_data)
        loop_bytes = sample_frames
        sample_flags = 0x01 | (0x20 if stereo else 0)

    name = ("Bench %dch %db%s" % (channels, bits, " stereo" if stereo else "")).encode("ascii").ljust(20, b"\0")
    tracker = b"Bench".ljust(20, b"\0")
    header = b"Extended Module: " + name + b"\x1a" + tracker + struct.pack("<H", 0x0104)
    song_header = (
        struct.pack("<I", 276)
        + struct.pack("<H", orders)
        + struct.pack("<H", 0)
        + struct.pack("<H", channels)
        + struct.pack("<H", 1)
        + struct.pack("<H", 1)
        + struct.pack("<H", 0)
        + struct.pack("<H", 6)
        + struct.pack("<H", 125)
        + bytes(256)
    )

    packed_pattern = bytearray()
    notes = [37, 41, 44, 48, 51, 55, 58, 61, 64, 68, 71, 75]
    for row in range(rows):
        for channel in range(channels):
            packed_pattern += bytes([0x80, notes[(row + channel) % len(notes)], 1, 0, 0, 0])
    pattern = (
        struct.pack("<I", 9)
        + bytes([0])
        + struct.pack("<H", rows)
        + struct.pack("<H", len(packed_pattern))
        + packed_pattern
    )

    instrument = (
        struct.pack("<I", 263)
        + b"Bench Instrument".ljust(22, b"\0")
        + bytes([0])
        + struct.pack("<H", 1)
        + struct.pack("<I", 40)
        + bytes(96)
        + bytes(96)
        + bytes(14)
        + struct.pack("<H", 0)
        + bytes([0, 0])
        + struct.pack("<H", 0)
        + struct.pack("<H", 0)
        + bytes([0])
        + bytes(15)
    )
    assert len(instrument) == 263

    sample_header = (
        struct.pack("<I", loop_bytes)
        + struct.pack("<I", 0)
        + struct.pack("<I", loop_bytes)
        + bytes([64, 0, sample_flags, 128, 0, 0])
        + b"Bench Sample".ljust(22, b"\0")
    )
    assert len(sample_header) == 40
    return header + song_header + pattern + instrument + sample_header + sample


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--bits", type=int, choices=(8, 16), default=16)
    parser.add_argument("--stereo", action="store_true")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for name, channels in (("low", 12), ("mid", 20), ("high", 32)):
        path = args.output_dir / ("bench_%s.xm" % name)
        path.write_bytes(build_xm(channels, bits=args.bits, stereo=args.stereo))
        print(path)


if __name__ == "__main__":
    main()
