#!/usr/bin/env python3
"""Production build must not contain USB report generation."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
psx1_pipeline = (ROOT / "src/psx1_pipeline.c").read_text(encoding="utf-8")
psx2_pipeline = (ROOT / "src/direct_ready40.c").read_text(encoding="utf-8")
psx2_multi = (ROOT / "src/direct_ready40_multi.c").read_text(encoding="utf-8")
assert "PAD_SQUARE" not in main
assert "psx1_report_save_usb(" not in main
assert "initializer_write_report(" not in main
assert "psx1_report_save_usb" not in psx1_pipeline
assert "initializer_write_report" not in psx2_pipeline
assert "initializer_write_report" not in psx2_multi
assert not (ROOT / "src/psx1_report.c").exists()
assert not (ROOT / "src/report.c").exists()
assert not (ROOT / "include/psx1_report.h").exists()
assert not (ROOT / "include/report.h").exists()

print("production USB report exclusion for PSX1 and PSX2: PASS")
