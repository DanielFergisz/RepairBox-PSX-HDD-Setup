#!/usr/bin/env python3
"""Audit fixed, revision-specific XFROM restoration without package metadata."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/xfrom_repair.c").read_text(encoding="utf-8")
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
media = (ROOT / "src/source_media.c").read_text(encoding="utf-8")

for required in (
    'XFROM_DEVICE_ROOT "xfrom0:/"',
    'XFROM_SYSTEM_ROOT "xfrom0:/BIEXEC-SYSTEM"',
    "XFROM_REPAIR_PSX1",
    "XFROM_REPAIR_PSX2",
    "psx1_files[]",
    "psx2_files[]",
    '"osdmain.dat", 6144000u',
    '"osdmain.mod", 2048u',
    '"xosdmain.elf", 423760u',
    '"xosdmain.elf", 478688u',
    '"bootflag.bak", 512u',
    '"bootflag.txt", 512u',
    "deferred_activation",
    "build_source_path",
    "entry->source_nested",
    "hash_path(path, 0",
    "load_verified_source",
    "ensure_system_root",
    "fileXioMkdir(XFROM_SYSTEM_ROOT, 0777)",
    "build_cleanup_plan",
    "known_destination_name",
    "safe_cleanup_name",
    "unterminated_xfrom_entry",
    "cleanup_system_root",
    "fileXioRemove(plan.paths[index])",
    'fail(result, "unexpected_xfrom_directory"',
    "FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC",
    "XFROM_IO_ATTEMPTS 2u",
    "XFROM_IO_SIZE (16u * 1024u)",
    "xfrom_progress_callback_t progress",
    "XFROM_PROGRESS_VALIDATE_BACKUP",
    "XFROM_PROGRESS_RELOAD_BACKUP",
    "XFROM_PROGRESS_WRITE_TARGET",
    "XFROM_PROGRESS_VERIFY_TARGET",
    "XFROM_PROGRESS_SCAN_CLEANUP",
    "XFROM_PROGRESS_REMOVE_EXTRAS",
    "XFROM_PROGRESS_VERIFY_CLEANUP",
    "report_progress(progress, progress_context",
):
    assert required in source, required

for forbidden in (
    "SHA256SUMS.txt", "PACKAGE-STATE.txt", "XFROM_MANIFEST_LIMIT",
    "xfromFormat", "xfromErase", "xfromWrite", "fileXioRmdir",
):
    assert forbidden not in source, forbidden

assert "RepairBox-PSX1-XFROM" in media
assert "RepairBox-PSX2-XFROM" in media
assert "source_media_psx1_xfrom_root" in media
assert "source_media_psx2_xfrom_root" in media

psx1_execute = main.index(
    "xfrom_return = xfrom_repair_execute("
)
psx1_hdd = main.index("psx1_execute(&result);", psx1_execute)
assert psx1_execute < psx1_hdd

psx2_execute = main.index(
    "xfrom_return = xfrom_repair_execute(", psx1_execute + 1
)
psx2_hdd = main.index("dr40_execute(&result", psx2_execute)
activation = main.index("activation_arm_pending(", psx2_hdd)
assert psx2_execute < psx2_hdd < activation
assert "HDD installation was not started." in main
assert "xfrom.source_valid && xfrom.xfrom_root_accessible" in main
assert "xfrom.source_valid && xfrom.xfrom_partition_accessible" not in main
assert source.count("fileXioRemove(") == 1
assert source.index("cleanup_system_root(result, progress, progress_context)") < source.index(
    "for (index = 0; index < result->file_count; ++index)",
    source.index("int xfrom_repair_execute("),
)

# Files observed after PSX1 game-area operations must never enter the
# revision allowlist. The generic cleanup path consequently removes them,
# including make_game_area.opt when that variant is present.
for stale_name in (
    "contents.opt",
    "delete_game_area.opt",
    "make_game_area.opt",
    "repartition.opt",
):
    assert f'{{"{stale_name}"' not in source, stale_name

# Keep one request aligned with fileXio's default 16 KiB IOP buffer.  The
# change is deliberately limited to batching: all source and destination
# hashes, post-write verification and EIO retry paths remain required above.
assert "#define XFROM_IO_SIZE 4096u" not in source

# The UI must expose progress for both the read-only preflight and the
# write/verify repair without rendering every 16 KiB callback.
for required in (
    "xfrom_progress_begin",
    "xfrom_progress_callback",
    "CHECKING SOURCE PACKAGE - READ ONLY",
    "REPAIRING XFROM - DO NOT POWER OFF",
    "CHECKING SOURCE PACKAGE",
    "OVERWRITING XFROM FROM SOURCE",
    "VERIFYING XFROM",
    "display_total = total * 2u",
    "display_completed = total + completed",
    "progress_group = 100",
    "percent < state->last_percent + 5u",
    'snprintf(line, sizeof(line), "[%s] %u%%", bar, percent)',
):
    assert required in main, required

for forbidden_label in (
    "VALIDATING BACKUP FILE",
    "READING AND RECHECKING BACKUP",
    "WRITING XFROM FILE",
    "VERIFYING WRITTEN XFROM FILE",
    "CHECKING XFROM BACKUP - READ ONLY",
):
    assert forbidden_label not in main, forbidden_label

assert main.count("xfrom_repair_validate_source(") == 4
assert main.count("xfrom_return = xfrom_repair_execute(") == 2
assert main.count("&xfrom_progress);") == 6

print("revision-specific pre-HDD XFROM restore and fixed-file validation: PASS")
