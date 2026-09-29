#!/usr/bin/env python3
"""Extract the externally supplied PSX MBR/OSD bootstrap prefix read-only."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


SECTOR_SIZE = 512
LAST_SECTOR = 0x27E6
SECTOR_COUNT = LAST_SECTOR + 1
BYTE_COUNT = SECTOR_COUNT * SECTOR_SIZE
OUTPUT_NAME = "mbr_bootstrap_prefix.bin"


def prepare(source: Path, output_dir: Path) -> str:
    if not source.is_file():
        raise ValueError(f"source does not exist: {source}")
    if source.stat().st_size < BYTE_COUNT:
        raise ValueError(
            f"source is too small: {source.stat().st_size} < {BYTE_COUNT}"
        )
    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / OUTPUT_NAME
    digest = hashlib.sha256()
    remaining = BYTE_COUNT
    with source.open("rb") as source_file, output.open("wb") as output_file:
        while remaining:
            chunk = source_file.read(min(1024 * 1024, remaining))
            if not chunk:
                raise OSError("unexpected end of source")
            output_file.write(chunk)
            digest.update(chunk)
            remaining -= len(chunk)
    if output.stat().st_size != BYTE_COUNT:
        raise OSError("output size verification failed")
    digest_hex = digest.hexdigest().upper()
    (output_dir / "SHA256SUMS.txt").write_text(
        f"{digest_hex}  {OUTPUT_NAME}\n", encoding="ascii", newline="\n"
    )
    (output_dir / "README.txt").write_text(
        "RepairBox PSX2 external bootstrap package\n"
        "\n"
        f"Source: {source}\n"
        "This package contains user-supplied proprietary PSX data.\n"
        "It is not embedded in the RepairBox ELF or source code.\n"
        "\n"
        "Logical sector range: 0x00000000..0x000027E6 inclusive\n"
        f"Sector count: 0x{SECTOR_COUNT:08X} ({SECTOR_COUNT})\n"
        f"Byte count: 0x{BYTE_COUNT:08X} ({BYTE_COUNT})\n"
        f"SHA-256: {digest_hex}\n",
        encoding="utf-8",
        newline="\n",
    )
    return digest_hex


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        default=Path(r"C:\RepairBox-PSX2-Initializer\HDDTOOL_PACK\__mbr.211"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("RepairBox-PSX2-Bootstrap"),
    )
    args = parser.parse_args()
    digest = prepare(args.source.resolve(), args.output.resolve())
    print(f"created {args.output / OUTPUT_NAME}")
    print(f"bytes={BYTE_COUNT} sectors=0x{SECTOR_COUNT:X} sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
