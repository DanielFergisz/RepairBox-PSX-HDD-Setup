#!/usr/bin/env python3
"""Guard the hardware-verified PSX1 v0.5 formatter and unified installer."""

from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
pipeline = (ROOT / "src/psx1_pipeline.c").read_text(encoding="utf-8")
policy = (ROOT / "src/psx1_format_test.c").read_text(encoding="utf-8")
installer = (ROOT / "src/installer.c").read_text(encoding="utf-8")

assert sha256((ROOT / "irx/psx1/ps2hdd-sparse-skip.irx").read_bytes()).hexdigest().upper() == (
    "14B7A3A258C23534E092D277CF41EFF758398B7C92969BB9A1984E80E27DC8DF"
)
assert pipeline.count('fileXioFormat("hdd0:", NULL, NULL, 0)') == 1
assert 'static const int pfs_format_args[1] = {8192};' in pipeline
assert pipeline.count("FORMAT_JOB_PFS") >= 2
assert '"__net", 0x00040000u, 0x00040000u' in policy
assert '"__system", 0x00080000u, 0x00080000u' in policy
assert '"__sysconf", 0x00100000u, 0x00100000u' in policy
assert '"__common", 0x00200000u, 0x00200000u' in policy
assert "scan->mbr_checksum_valid" in policy
assert "scan->mbr_osd_start == 0" in policy
assert "pfs->zone_size == PFS_ZONE_SIZE" in policy
assert "pfs->journal_class == PFS_JOURNAL_CLEAN" in policy
assert "installer_initialize_psx1" in pipeline
assert "installer_execute(&result->installer)" in pipeline
assert "inspector_scan(&result->final_layout)" in pipeline
assert "result->final_raw_pfs_valid" in pipeline
assert "static int installed_pfs_valid" in pipeline
assert "pfs->root_directory_entries >= 2" in pipeline
final_validation = pipeline[pipeline.index('"FINAL PSX1 VALIDATION"'):]
assert "installed_pfs_valid(entry)" in final_validation
assert "format_test_strict_pfs_valid(entry)" not in final_validation
assert "hash_open_file(destination" in installer
assert "source_size != destination_size" in installer
assert "memcmp(source_digest, destination_digest" in installer
assert "current_file_bytes_processed" in installer
assert "source_total_bytes * 2u" in installer
assert "total_completed_file_bytes * 2u" in installer
assert "file_total_work = result->current_file_size * 2u" in installer
assert '"INSTALLING  %s"' in installer
assert '7u, 8u, "COPY AND VERIFY PACKAGE"' in installer
assert '11u, 13u, "COPY SYSTEM/DVR FILES"' in installer
assert '"Step %u / %u   %s"' in installer
assert "Every file is copied" not in installer
assert "result->format.capacity_class == MEDIA_CAPACITY_64_GB" in pipeline
assert "result->format.capacity_class = MEDIA_CAPACITY_SETMAX_HIDDEN" in pipeline
assert "78125000ULL * 9u / 10u" in pipeline
assert "78125000ULL * 11u / 10u" in pipeline
assert 'capacity == MEDIA_CAPACITY_SETMAX_HIDDEN' in pipeline
assert '"PSX1 MEDIA"' in pipeline
verified_start = pipeline.index("result->hardware_verified_profile =")
verified_end = pipeline.index(";", verified_start)
assert "MEDIA_CAPACITY_SETMAX_HIDDEN" not in pipeline[verified_start:verified_end]
assert 'psx1_capacity_name(result->format.capacity_class)' in (ROOT / "src/main.c").read_text(encoding="utf-8")
assert 'NATIVE SIZE HIDDEN' not in (ROOT / "src/main.c").read_text(encoding="utf-8")
assert 'Native size: HIDDEN BY SETMAX' not in (ROOT / "src/main.c").read_text(encoding="utf-8")
assert "#define PROGRESS_REFRESH_MS 500u" in installer
assert 'ui_printf("%-*.*s"' in installer
show_start = installer.index("static void show_progress")
show_end = installer.index("static int hash_open_file", show_start)
show = installer[show_start:show_end]
assert show.count("ui_begin();") == 1
assert "if (!progress_screen_ready)" in show
assert "Overall: %llu%%" in show
assert '"%s  %s", phase' not in show

# Model the exact v1.3 COPY + VERIFY work accounting across several files.
sizes = [65536, 308112, 17]
total = sum(sizes)
completed = 0
observed = []
for size in sizes:
    for processed in (0, size // 2, size):
        observed.append((completed * 2 + processed) * 100 // (total * 2))
    for processed in (0, size // 2, size):
        observed.append(
            (completed * 2 + size + processed) * 100 // (total * 2)
        )
    completed += size
assert observed == sorted(observed)
assert observed[-1] == 100

# Per-file percentage is one continuous COPY + VERIFY interval (0..50..100).
for size in sizes:
    per_file = []
    for processed in (0, size // 2, size):
        per_file.append(processed * 100 // (size * 2))
    for processed in (0, size // 2, size):
        per_file.append((size + processed) * 100 // (size * 2))
    assert per_file == sorted(per_file)
    assert per_file[-1] == 100

print("PSX1 v0.5 fast APA, raw validation, copy and verification: PASS")
