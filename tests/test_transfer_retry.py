#!/usr/bin/env python3
"""Static policy checks for precise transfer errors and bounded retries."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
installer = (root / "src" / "installer.c").read_text(encoding="utf-8")
header = (root / "include" / "installer.h").read_text(encoding="utf-8")

assert "#define FILE_IO_MAX_ATTEMPTS 2u" in installer
assert "copy_file_once" in installer
assert "return_value != -EIO || attempt == FILE_IO_MAX_ATTEMPTS" in installer
assert "++result->copy_retry_count;" in installer
assert "result->failure_offset = failure_offset;" in installer
assert "result->failure_attempt = attempt;" in installer

for operation in (
    "source_open",
    "source_read",
    "source_close",
    "source_size_mismatch",
    "destination_open",
    "destination_write",
    "destination_close",
    "destination_verify_open",
    "destination_verify_read",
    "destination_verify_close",
    "destination_verify_mismatch",
):
    assert f'"{operation}"' in installer

assert '"copy_or_verify_file"' not in installer
assert "validate_source_files" not in installer
assert "source_content_check" not in installer
assert "u64 failure_offset;" in header
assert "u32 failure_attempt;" in header

print("Bounded retry, precise transfer errors and fast preflight policy: PASS")
