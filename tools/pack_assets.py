#!/usr/bin/env python3
"""Pack PNG/MP3 assets into two runtime packages without conversion."""

import argparse
import hashlib
from pathlib import Path
import re
import struct
import zlib


MAGIC = b"MHASSETS"
SAFE_PART = re.compile(r"[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*")
# Windows device names stay invalid as file stems with any extension.
RESERVED_STEMS = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)), *(f"lpt{i}" for i in range(1, 10))}


def manifest_text(manifest):
    rows = ["MHMANIFEST 1", f"tables {manifest['tables']['path']}", f"images {len(manifest['images'])}"]
    for image in manifest["images"]:
        atlas = image["index_atlas"]
        key = image["color_key"] if image["color_key"] is not None else "none"
        rows.append(f"image {image['name']} {image['palette']} {key} {atlas['path']} {atlas['bytes']} {len(image['frames'])}")
        rows.extend("frame " + " ".join(map(str, rectangle)) for rectangle in image["frames"])
    rows.append(f"audio {len(manifest['audio'])}")
    fields = ("name", "path", "bytes", "original_sample_frames", "original_sample_rate", "decoded_48000hz_samples", "codec", "channels", "source_duration_seconds")
    rows.extend("sound " + " ".join(str(sound[field]) for field in fields) for sound in manifest["audio"])
    rows.append(f"lookups {len(manifest['lookup_tables'])}")
    rows.extend(f"lookup {item['path']} {item['bytes']}" for item in manifest["lookup_tables"])
    rows.append("end")
    return "\n".join(rows) + "\n"


def read_manifest(path):
    rows = iter(path.read_text(encoding="ascii").splitlines())

    def row(tag, length):
        value = next(rows).split()
        if len(value) != length or value[0] != tag:
            raise ValueError(f"Expected {tag} record")
        return value[1:]

    try:
        if row("MHMANIFEST", 2) != ["1"]:
            raise ValueError("Unsupported manifest version")
        result = {"tables": {"path": row("tables", 2)[0]}, "images": [], "audio": [], "lookup_tables": []}
        for _ in range(int(row("images", 2)[0])):
            name, palette, key, atlas, size, count = row("image", 7)
            result["images"].append({"name": name, "palette": palette, "color_key": None if key == "none" else int(key),
                "index_atlas": {"path": atlas, "bytes": int(size)},
                "frames": [[int(value) for value in row("frame", 5)] for _ in range(int(count))]})
        for _ in range(int(row("audio", 2)[0])):
            name, file, size, original, rate, decoded, codec, channels, duration = row("sound", 10)
            result["audio"].append({"name": name, "path": file, "bytes": int(size), "original_sample_frames": int(original),
                "original_sample_rate": int(rate), "decoded_48000hz_samples": int(decoded), "codec": codec,
                "channels": int(channels), "source_duration_seconds": float(duration)})
        for _ in range(int(row("lookups", 2)[0])):
            file, size = row("lookup", 3)
            result["lookup_tables"].append({"path": file, "bytes": int(size)})
        row("end", 1)
        if next(rows, None) is not None:
            raise ValueError("Unexpected manifest data")
        return result
    except StopIteration as error:
        raise ValueError("Truncated manifest") from error


def collect_files(manifest_path):
    manifest_path = manifest_path.resolve(strict=True)
    root = manifest_path.parent
    manifest_bytes = manifest_path.read_bytes()
    manifest = read_manifest(manifest_path)
    groups = {
        "images": {"manifest.txt": manifest_bytes},
        "audio": {},
    }
    for group, records in (
        (
            "images",
            [
                (manifest["tables"], "tables.txt"),
                *[(image["index_atlas"], f"indices/{image['name']}.png") for image in manifest["images"]],
                *[(lookup, lookup["path"]) for lookup in manifest["lookup_tables"]],
            ],
        ),
        ("audio", [(sound, f"audio/{sound['name']}.mp3") for sound in manifest["audio"]]),
    ):
        for record, expected in records:
            relative = record["path"]
            if not isinstance(relative, str) or len(relative) > 240:
                raise ValueError("Invalid asset path")
            for part in relative.split("/"):
                if not SAFE_PART.fullmatch(part) or part.split(".", 1)[0].lower() in RESERVED_STEMS:
                    raise ValueError(f"Unsafe asset path: {relative}")
            if relative != expected:
                raise ValueError(f"expected {group} path {expected}: {relative}")
            if relative in groups[group]:
                raise ValueError(f"Duplicate asset path: {relative}")
            path = (root / relative).resolve(strict=True)
            if not path.is_relative_to(root) or not path.is_file():
                raise ValueError(f"Asset path escapes root or is not a file: {relative}")
            data = path.read_bytes()
            if "bytes" in record and len(data) != record["bytes"]:
                raise ValueError(f"Byte count mismatch: {relative}")
            groups[group][relative] = data
    return {name: sorted(files.items()) for name, files in groups.items()}


def pack_files(files):
    """MHASSETS + count + index bytes; index entries are name, offset, size, CRC32."""
    index_size = sum(22 + len(name.encode("ascii")) for name, _ in files)
    offset = 16 + index_size
    index = bytearray()
    payload = bytearray()
    previous = ""
    for name, data in files:
        encoded = name.encode("ascii")
        if not name or (previous and previous >= name) or len(encoded) > 240:
            raise ValueError("Asset names must be sorted and unique")
        previous = name
        index.extend(struct.pack("<HQQI", len(encoded), offset, len(data), zlib.crc32(data)))
        index.extend(encoded)
        payload.extend(data)
        offset += len(data)
    return MAGIC + struct.pack("<II", len(files), index_size) + index + payload


def write_package(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == data:
        return
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    groups = collect_files(args.manifest)
    for name, files in groups.items():
        data = pack_files(files)
        path = args.output_dir / f"{name}.pak"
        write_package(path, data)
        print(f"{path}: {len(files)} files, {len(data)} bytes, SHA-256 {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
