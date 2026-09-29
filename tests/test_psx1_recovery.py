#!/usr/bin/env python3
"""Guard the opt-in PSX1 recovery path and unchanged default fast path."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
pipeline = (root / "src/psx1_pipeline.c").read_text(encoding="utf-8")
main = (root / "src/main.c").read_text(encoding="utf-8")

assert pipeline.count('fileXioFormat("hdd0:", NULL, NULL, 0)') == 1
assert "APA_POST_FORMAT_SCAN_LIMIT 4u" in pipeline
assert "APA_POST_FORMAT_RETRY_US 500000u" in pipeline
assert 'fileXioDevctl("hdd0:", HDIOC_FLUSH' in pipeline
assert "validate_apa_after_format(result)" in pipeline

# Recovery matches the standard APA sparse-header invalidation geometry. It
# never touches LBA 0 and verifies every two-sector zero write before retrying.
assert "APA_HEADER_FIRST_LBA 0x00002000u" in pipeline
assert "APA_HEADER_LBA_STEP 0x00040000u" in pipeline
assert "APA_HEADER_SECTORS 2u" in pipeline
assert '"hdd0:", HDIOC_WRITESECTOR' in pipeline
assert '"hdd0:", HDIOC_READSECTOR' in pipeline
assert "memcmp(transfer->data, recovery_readback_buffer" in pipeline
assert "recovery_cleanup_verified_slots" in pipeline
assert "recovery_cleanup_failure_lba" in pipeline
assert "psx1_execute_internal(result, 0)" in pipeline
assert "psx1_execute_internal(result, 1)" in pipeline

# It is offered only after a successful fast-format call whose new layout did
# not become readable, and requires a second L1+R1+X confirmation in the UI.
gate = pipeline[pipeline.index("int psx1_recovery_available("):]
assert "result->format.apa.return_value >= 0" in gate
assert "result->format.failed_step == 1" in gate
assert "psx1_recovery_available(&result)" in main
assert "confirmation_chord(current, pressed)" in main
assert "APA cleanup + format retry" in main

print("opt-in sparse APA cleanup and verified retry: PASS")
