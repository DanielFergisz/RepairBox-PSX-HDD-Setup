#!/usr/bin/env python3
"""Check hardware-verified 256 GB, 512 GB and 1 TB contracts."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include/capacity_profile.h").read_text(encoding="utf-8")
PROFILE = (ROOT / "src/capacity_profile.c").read_text(encoding="utf-8")
MULTI = (ROOT / "src/direct_ready40_multi.c").read_text(encoding="utf-8")
MAIN = (ROOT / "src/main.c").read_text(encoding="utf-8")

VISIBLE = 0x04A817C8
XCONTENTS_START = 0x04CC17C8
TAIL = 0x800

profiles = {
    "256_measured": 0x1DD80000,
    "512_verified": 0x3BB00000,
    "1TB_reference": 0x74706DB0,
}

assert profiles["512_verified"] * 512 == 512_711_720_960
assert profiles["1TB_reference"] * 512 == 1_000_204_886_016
assert 0x0EE78000 <= 0x10000000  # hardware-rejected 128 GB card
assert 0x10000001 > 0x10000000   # first generically accepted LBA48 size

assert profiles["512_verified"] - TAIL == 0x3BAFF800
assert profiles["512_verified"] - TAIL - XCONTENTS_START == 0x36E3E038
assert profiles["1TB_reference"] - TAIL == 0x747065B0
assert profiles["1TB_reference"] - TAIL - XCONTENTS_START == 0x6FA44DE8

assert "#define CAPACITY_NATIVE_512_VERIFIED 0x3BB00000u" in HEADER
assert "#define CAPACITY_512_MIN_BYTES 450000000000ULL" in HEADER
assert "#define CAPACITY_512_MAX_BYTES 550000000000ULL" in HEADER
assert "#define CAPACITY_1TB_REFERENCE 0x74706DB0u" in HEADER
assert "#define CAPACITY_LBA28_SECTOR_LIMIT 0x10000000u" in HEADER
assert "bytes >= 900000000000ULL" in PROFILE
assert "bytes <= 1099000000000ULL" in PROFILE
assert "native_sectors <= CAPACITY_LBA28_SECTOR_LIMIT" in PROFILE
assert "return CAPACITY_PROFILE_LBA48_UNTESTED" in PROFILE
assert "capacity_profile_is_supported" in PROFILE
assert "return CAPACITY_PROFILE_512_VERIFIED" in PROFILE
assert 'return "512 GB"' in PROFILE
assert "return CAPACITY_PROFILE_1TB_VERIFIED" in PROFILE
assert 'return "1 TB"' in PROFILE
assert "CAPACITY_PROFILE_1TB_EXPERIMENTAL" not in HEADER + PROFILE
assert "expected_end = (u64)native_max - 0x800u" in MULTI
assert "expected_size = expected_end - 0x04CC17C8u" in MULTI
assert "calculated_end == expected_end" in MULTI
assert "Capacity profile: UNTESTED" not in MAIN
assert "PAD_SQUARE" not in MAIN

# The 256 GB route remains the frozen, hardware-verified executor. Larger
# profiles use a separate implementation and cannot alter that source file.
assert "profile == CAPACITY_PROFILE_256_VERIFIED" in MAIN
assert "dr40_execute(&result" in MAIN
assert "dr40_multi_execute(&result" in MAIN
assert VISIBLE == 78_125_000

print("256/512/1TB capacity profiles and dynamic DVR arithmetic: PASS")
