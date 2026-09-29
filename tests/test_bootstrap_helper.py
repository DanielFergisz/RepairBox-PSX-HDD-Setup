#!/usr/bin/env python3
"""Verify deterministic, prefix-only bootstrap package preparation."""

import hashlib
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HELPER_PATH = ROOT / "tools" / "prepare_psx2_bootstrap.py"
spec = importlib.util.spec_from_file_location("bootstrap_helper", HELPER_PATH)
assert spec is not None and spec.loader is not None
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)

assert helper.SECTOR_COUNT == 0x27E7
assert helper.BYTE_COUNT == 0x004FCE00

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    source = root / "source.bin"
    output = root / "RepairBox-PSX2-Bootstrap"
    pattern = bytes(range(256))
    with source.open("wb") as stream:
        remaining = helper.BYTE_COUNT + 4096
        while remaining:
            chunk = pattern[: min(len(pattern), remaining)]
            stream.write(chunk)
            remaining -= len(chunk)
    original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    digest = helper.prepare(source, output)
    produced = output / helper.OUTPUT_NAME
    expected = source.read_bytes()[: helper.BYTE_COUNT]
    assert produced.read_bytes() == expected
    assert produced.stat().st_size == helper.BYTE_COUNT
    assert digest == hashlib.sha256(expected).hexdigest().upper()
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original_hash
    sums = (output / "SHA256SUMS.txt").read_text(encoding="ascii")
    assert sums == f"{digest}  {helper.OUTPUT_NAME}\n"

print("bootstrap helper exact-range and source-read-only tests: PASS")
