#!/usr/bin/env python3
"""Generate the property-test and differential-test corpora.

The valid files are deterministic 4-channel ProTracker MOD files.  The
property corpus mutates those files and the four in-tree OpenMPT test modules
in four categories: truncation, invalid pattern data, huge sample spans and
cyclic order references.  Every malformed case has a JSON sidecar containing
the byte offset that was changed/removed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILTIN_SOURCES = [
    ROOT / "test/test.mod",
    ROOT / "test/test.xm",
    ROOT / "test/test.s3m",
    ROOT / "test/test.mptm",
]

# One octave of ProTracker periods (C-2 .. B-2) followed by C-3.
PERIODS = [428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 239, 226, 214]


def cell(sample: int = 0, period: int = 0, effect: int = 0, param: int = 0) -> bytes:
    sample &= 0x1F
    period &= 0x0FFF
    return bytes(
        (
            (sample & 0xF0) | ((period >> 8) & 0x0F),
            period & 0xFF,
            ((sample & 0x0F) << 4) | (effect & 0x0F),
            param & 0xFF,
        )
    )


def make_wave(kind: int, length: int, seed: int) -> bytes:
    rng = random.Random(seed)
    out = bytearray(length)
    for i in range(length):
        phase = (i * 8) // max(1, length)
        if kind == 0:
            value = 96 if (i < length // 2) else -96
        elif kind == 1:
            value = -96 + (192 * i) // max(1, length - 1)
        elif kind == 2:
            value = 96 - abs((192 * i) // max(1, length - 1) - 96)
        else:
            value = rng.randrange(-96, 97)
        out[i] = value & 0xFF
    return bytes(out)


def make_valid_mod(index: int) -> tuple[bytes, dict]:
    rng = random.Random(0x4D4F4400 + index)
    channels = 4
    title = f"property-valid-{index:03d}".encode("ascii")[:20].ljust(20, b" ")

    sample_words = [
        64 + ((index * 31 + 0) % 24) * 16,
        64 + ((index * 31 + 7) % 24) * 16,
        64 + ((index * 31 + 13) % 24) * 16,
        64 + ((index * 31 + 19) % 24) * 16,
    ]
    sample_data = []
    sample_headers = []
    for smp in range(31):
        if smp < len(sample_words):
            words = sample_words[smp]
            data = make_wave(smp, words * 2, index * 100 + smp)
            name = f"wave{smp + 1}".encode("ascii")[:22].ljust(22, b"\0")
            sample_headers.append(
                name
                + struct.pack(">HBBHH", words, 0, 64, 0, words)
            )
            sample_data.append(data)
        else:
            sample_headers.append(b"\0" * 30)

    patterns = 1 + (index % 2)
    order_list = list(range(patterns))[:128] + [0] * (128 - patterns)
    header = (
        title
        + b"".join(sample_headers)
        + bytes((1 + patterns - 1, 0))
        + bytes(order_list)
        + b"M.K."
    )

    pattern_blobs = []
    for pat in range(patterns):
        rows = []
        for row in range(64):
            row_cells = []
            for chn in range(channels):
                if row % (4 + pat) == chn and row >= 4:
                    note = (row // 2 + chn + pat * 3) % len(PERIODS)
                    smp = 1 + ((row // 8 + chn + pat) % 4)
                    effect = 0
                    param = 0
                    effect_selector = (index + row + chn + pat) % 7
                    if effect_selector == 0:
                        effect, param = 0x0, ((row + index) & 0x0F)
                    elif effect_selector == 1:
                        effect, param = 0x4, 0x47
                    elif effect_selector == 2:
                        effect, param = 0x6, 0x83
                    elif effect_selector == 3:
                        effect, param = 0x7, 0x37
                    elif effect_selector == 4:
                        effect, param = 0xA, 0x10
                    elif effect_selector == 5:
                        effect, param = 0xE, 0x9A
                    row_cells.append(cell(smp, PERIODS[note], effect, param))
                else:
                    row_cells.append(cell())
            rows.append(b"".join(row_cells))
        pattern_blobs.append(b"".join(rows))

    payload = header + b"".join(pattern_blobs) + b"".join(sample_data)
    meta = {
        "valid_id": f"valid-{index:03d}",
        "format": "mod",
        "channels": channels,
        "patterns": patterns,
        "samples": len(sample_words),
        "rows": 64,
        "sha256": hashlib.sha256(payload).hexdigest(),
        "size": len(payload),
    }
    return payload, meta


def detect_format(data: bytes) -> str:
    if data.startswith(b"Extended Module: "):
        return "xm"
    if data.startswith(b"IMPM"):
        return "it"
    if len(data) >= 48 and data[44:48] == b"SCRM":
        return "s3m"
    if len(data) >= 1084 and data[1080:1084] in {
        b"M.K.",
        b"M!K!",
        b"FLT4",
        b"FLT8",
        b"4CHN",
        b"8CHN",
    }:
        return "mod"
    return "unknown"


def put_be16(data: bytearray, offset: int, value: int) -> None:
    if 0 <= offset <= len(data) - 2:
        data[offset : offset + 2] = struct.pack(">H", value & 0xFFFF)


def put_le16(data: bytearray, offset: int, value: int) -> None:
    if 0 <= offset <= len(data) - 2:
        data[offset : offset + 2] = struct.pack("<H", value & 0xFFFF)


def mutate_truncation(data: bytes, index: int, fmt: str) -> tuple[bytes, int, str]:
    candidates = {
        "mod": [0, 1, 20, 42, 1080, 1084, 1085, 1084 + 512, 950, 952, len(data) // 2, len(data) - 1],
        "xm": [0, 17, 37, 60, 80, 335, 336, len(data) // 2, len(data) - 1],
        "s3m": [0, 44, 48, 96, 128, 192, len(data) // 2, len(data) - 1],
        "it": [0, 4, 32, 38, 192, 256, len(data) // 2, len(data) - 1],
        "unknown": [0, 1, 16, len(data) // 2, len(data) - 1],
    }[fmt]
    candidates = [max(0, min(len(data) - 1, c)) for c in candidates]
    cut = candidates[(index // 4 * 5 + index) % len(candidates)]
    return data[:cut], cut, f"truncate before byte {cut}"


def mutate_invalid_pattern(data: bytes, index: int, fmt: str) -> tuple[bytes, int, str]:
    out = bytearray(data)
    if fmt == "mod" and len(out) >= 1085:
        pattern_count = max(1, min(128, (len(out) - 1084) // 1024))
        pat = (index // 4) % pattern_count
        block = min(1084 + pat * 1024, len(out) - 1)
        for j in range(block, min(block + 64, len(out))):
            out[j] = 0xFF
        if len(out) > 952:
            out[950] = 2
            out[952] = max(0, pattern_count - 1)
            out[953] = 0x7F
        return bytes(out), block, f"invalid MOD pattern cells in pattern {pat}"

    if fmt == "xm" and len(out) >= 72:
        put_le16(out, 70, 0xFFFF)
        if len(out) > 80:
            out[80] = 0xFE
        return bytes(out), 70, "XM pattern count and order reference set invalid"

    if fmt == "s3m" and len(out) >= 38:
        put_le16(out, 36, 0xFFFF)
        if len(out) > 96:
            out[96] = 0xFE
        return bytes(out), 36, "S3M pattern count and order reference set invalid"

    if fmt == "it" and len(out) >= 40:
        put_le16(out, 38, 0xFFFF)
        if len(out) > 192:
            out[192] = 0xFE
        return bytes(out), 38, "IT/MPTM pattern count and order reference set invalid"

    offset = min(len(out) - 1, max(0, len(out) // 2))
    out[offset] ^= 0xFF
    return bytes(out), offset, "generic invalid pattern byte"


def mutate_huge_samples(data: bytes, index: int, fmt: str) -> tuple[bytes, int, str]:
    out = bytearray(data)
    if fmt == "mod" and len(out) >= 1080:
        for smp in range(31):
            put_be16(out, 20 + smp * 30 + 22, 0xFFFF)
        return bytes(out), 42, "all MOD sample lengths set to 65535 words"

    if fmt == "xm" and len(out) >= 74:
        put_le16(out, 72, 0xFFFF)
        return bytes(out), 72, "XM instrument/sample count set to 65535"

    if fmt == "s3m" and len(out) >= 36:
        put_le16(out, 34, 0xFFFF)
        return bytes(out), 34, "S3M sample count set to 65535"

    if fmt == "it" and len(out) >= 40:
        put_le16(out, 36, 0xFFFF)
        return bytes(out), 36, "IT/MPTM sample count set to 65535"

    offset = min(len(out) - 1, max(0, len(out) // 3))
    if offset + 1 < len(out):
        out[offset] = 0xFF
        out[offset + 1] = 0xFF
    return bytes(out), offset, "generic huge span field"


def mutate_cycle(data: bytes, index: int, fmt: str) -> tuple[bytes, int, str]:
    out = bytearray(data)
    if fmt == "mod" and len(out) >= 1084 + 64 * 16:
        out[950] = 2
        out[951] = 0
        out[952] = 0
        out[953] = 0
        cell_offset = 1084 + 63 * 16
        out[cell_offset + 2] = 0x0B
        out[cell_offset + 3] = 0x00
        return bytes(out), cell_offset + 2, "MOD order list and B00 pattern jump form a cycle"

    if fmt == "xm" and len(out) >= 336:
        put_le16(out, 64, 2)
        put_le16(out, 66, 0)
        out[80] = 0
        out[81] = 0
        return bytes(out), 64, "XM order list repeats pattern 0 and restarts at order 0"

    if fmt == "s3m" and len(out) >= 98:
        put_le16(out, 32, 2)
        out[96] = 0
        out[97] = 0
        return bytes(out), 32, "S3M order list repeats order 0"

    if fmt == "it" and len(out) >= 194:
        put_le16(out, 32, 2)
        out[192] = 0
        out[193] = 0
        return bytes(out), 32, "IT/MPTM order list repeats order 0"

    offset = min(len(out) - 1, max(0, len(out) // 2))
    out[offset] ^= 0xFF
    return bytes(out), offset, "generic cyclic reference byte"


MUTATORS = [
    ("truncation", mutate_truncation),
    ("invalid_pattern", mutate_invalid_pattern),
    ("huge_sample_count", mutate_huge_samples),
    ("cycle_reference", mutate_cycle),
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path(__file__).with_name("corpus"))
    args = parser.parse_args()

    out = args.out.resolve()
    valid_dir = out / "valid"
    prop_dir = out / "cases"
    valid_dir.mkdir(parents=True, exist_ok=True)
    prop_dir.mkdir(parents=True, exist_ok=True)

    valid_manifest = []
    for i in range(50):
        payload, meta = make_valid_mod(i)
        path = valid_dir / f"valid-{i:03d}.mod"
        path.write_bytes(payload)
        meta["path"] = str(path.relative_to(ROOT))
        valid_manifest.append(meta)
    (out / "valid_manifest.json").write_text(
        json.dumps(valid_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    # Cases 0..15 exercise every in-tree format in every category.  The
    # remaining 184 cases each use their own generated player-style MOD, so
    # the run covers 200 distinct seed instances rather than repeatedly
    # mutating a handful of files.
    seed_dir = out / "seeds"
    seed_dir.mkdir(parents=True, exist_ok=True)
    seed_paths = []
    seed_manifest = []
    for i in range(50, 234):
        payload, meta = make_valid_mod(i)
        path = seed_dir / f"seed-{i:03d}.mod"
        path.write_bytes(payload)
        meta["path"] = str(path.relative_to(ROOT))
        seed_manifest.append(meta)
        seed_paths.append(path)
    (out / "seeds_manifest.json").write_text(
        json.dumps(seed_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    cases_manifest = []
    for i in range(200):
        category, mutator = MUTATORS[i % len(MUTATORS)]
        if i < 16:
            source = BUILTIN_SOURCES[i // len(MUTATORS)]
        else:
            source = seed_paths[i - 16]
        original = source.read_bytes()
        fmt = detect_format(original)
        mutated, offset, description = mutator(original, i, fmt)
        case_id = f"case-{i:03d}-{category}"
        path = prop_dir / f"{case_id}.bin"
        path.write_bytes(mutated)
        meta = {
            "id": case_id,
            "category": category,
            "source": str(source.relative_to(ROOT)),
            "format": fmt,
            "mutation_offset": offset,
            "description": description,
            "source_size": len(original),
            "size": len(mutated),
            "sha256": hashlib.sha256(mutated).hexdigest(),
        }
        (prop_dir / f"{case_id}.json").write_text(
            json.dumps(meta, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        cases_manifest.append(meta)

    (out / "cases_manifest.json").write_text(
        json.dumps(cases_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"wrote 50 valid MOD files and 200 property cases to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
