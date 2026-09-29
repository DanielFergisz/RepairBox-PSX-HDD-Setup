#!/usr/bin/env python3
"""Enforce the explicitly authorized destructive scope."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
sources = {
    path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8")
    for folder in (ROOT / "src", ROOT / "include")
    for path in sorted(folder.glob("*.[ch]"))
}
all_source = "\n".join(sources.values())
pipeline = sources["src/direct_ready40.c"]
bootstrap = sources["src/bootstrap.c"]
bootflag = sources["src/bootflag_ro.c"]
activation = sources["src/activation.c"]
xfrom_repair = sources["src/xfrom_repair.c"]
psx1_pipeline = sources["src/psx1_pipeline.c"]
multi_pipeline = sources["src/direct_ready40_multi.c"]

for forbidden in (
    "HDIOC_PRESETMAXLBA28", "HDIOC_POSTSETMAXLBA28",
    "sceAtaExecCmd", "SifIopReset", "HDIOC_SETOSDMBR",
    "u32 value = 0x00400000",
):
    assert forbidden not in all_source, forbidden

# One verified SETMAX call in each mutually exclusive capacity executor, with
# the same hardware-verified 40 GB value only.
assert len(re.findall(r"fileXioDevctl\s*\([^;]*HDIOC_SETMAXLBA28",
                      pipeline, re.DOTALL)) == 1
assert len(re.findall(r"fileXioDevctl\s*\([^;]*HDIOC_SETMAXLBA28",
                      multi_pipeline, re.DOTALL)) == 1
assert "u32 value = DR40_VISIBLE_SECTORS;" in pipeline
assert "u32 value = DR40_VISIBLE_SECTORS;" in multi_pipeline
assert '"dvr_hdd0:", HDIOC_SETMAXLBA28' in pipeline
assert '"dvr_hdd0:", HDIOC_SETMAXLBA28' in multi_pipeline

# Exact formatter scope and no fallback/retry format variants.
assert pipeline.count('fileXioFormat("hdd0:", NULL, NULL, 0)') == 1
assert pipeline.count('fileXioFormat("dvr_hdd0:", NULL, NULL, 0)') == 1
assert pipeline.count("fileXioFormat(") == 3
assert '"pfs:"' in pipeline and '"dvr_pfs:"' in pipeline
assert multi_pipeline.count('fileXioFormat("hdd0:", NULL, NULL, 0)') == 1
assert multi_pipeline.count(
    'fileXioFormat("dvr_hdd0:", NULL, NULL, 0)') == 1
assert multi_pipeline.count("fileXioFormat(") == 3

# Raw writes exist only in bootstrap.c and the explicitly confirmed PSX1 APA
# recovery path. Both target logical hdd0; no physical SD path may appear.
assert all_source.count("HDIOC_WRITESECTOR") == 2
assert '"hdd0:", HDIOC_WRITESECTOR' in bootstrap
assert '"hdd0:", HDIOC_WRITESECTOR' in psx1_pipeline
assert "BOOTSTRAP_SECTOR_COUNT 0x27E7u" in sources["include/bootstrap.h"]
assert "BOOTSTRAP_BYTE_COUNT 0x004FCE00u" in sources["include/bootstrap.h"]
for physical in ("mass0:", "sd0:", "ata0:", "\\\\.\\PhysicalDrive"):
    assert physical not in bootstrap

# XFROM is strictly read-only: no writable flags or mutating API tokens.
assert "BOOTFLAG_RO_XFROM_PATH" in bootflag
assert "FIO_O_RDONLY" in bootflag
for forbidden in (
    "FIO_O_WRONLY", "FIO_O_RDWR", "FIO_O_CREAT", "FIO_O_TRUNC",
    "fileXioWrite", "xfromWrite", "xfromErase", "xfromFormat",
):
    assert forbidden not in bootflag, forbidden

# XFROM mutation is limited to the fixed revision-specific restore, creation
# of the known BIEXEC-SYSTEM directory when missing, removal of unexpected
# regular files inside that directory, and one complete post-storage
# activation write. No directory is removed or formatted.
assert all_source.count("fileXioWrite(") == 3
assert activation.count("fileXioWrite(") == 1
assert xfrom_repair.count("fileXioWrite(") == 1
assert 'fileXioOpen(BOOTFLAG_XFROM_PATH,' in activation
assert "FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC" in activation
assert "if (!storage_valid || !session_confirmed" in activation
assert "activation_repair_current_digest" not in activation
assert "!result->current_accessible" not in activation[
    activation.index("int activation_arm_pending("):
]
assert "result->replacement_required" in activation
assert "bootflag_original.bin" not in all_source
assert "xfromFormat" not in xfrom_repair
assert "xfromErase" not in xfrom_repair
assert "xfromWrite" not in xfrom_repair
assert xfrom_repair.count("fileXioRemove(") == 1
assert "fileXioRemove(plan.paths[index])" in xfrom_repair
assert "fileXioRmdir" not in xfrom_repair
assert xfrom_repair.count("fileXioMkdir(") == 1
assert "fileXioMkdir(XFROM_SYSTEM_ROOT, 0777)" in xfrom_repair
assert 'XFROM_DEVICE_ROOT "xfrom0:/"' in xfrom_repair
assert 'XFROM_SYSTEM_ROOT "xfrom0:/BIEXEC-SYSTEM"' in xfrom_repair
assert "deferred_activation" in xfrom_repair

# Production has no USB report writer or report hotkey.
assert "mass:/RepairBox_PSX2" not in all_source
assert "dr40_write_report(" not in sources["src/main.c"]
assert "_Report.txt" not in all_source
assert "activation_verify_existing_pending" in activation
assert activation.count("fileXioWrite(") == 1
assert "bootflag_ro_allow_pending_reinstall" in sources["src/main.c"]
assert "initializer_write_report(" not in sources["src/main.c"]
assert "psx1_report_save_usb(" not in sources["src/main.c"]
assert "PAD_SQUARE" not in sources["src/main.c"]
for forbidden in ("HDIOC_SETMAXLBA28", "dvr_hdd0:", "dvr_pfs:",
                  "BOOTFLAG_XFROM_PATH", "bootstrap_write_and_verify"):
    assert forbidden not in psx1_pipeline, forbidden

# File-copy destinations are confined to the selected mounted PFS root.
installer = sources["src/installer.c"]
assert "safe_component" in installer
assert "root_prefix" in installer
assert "FIO_MT_RDWR" in installer
assert all_source.count("fileXioRemove(") == 1
assert "fileXioRemove(plan.paths[index])" in xfrom_repair
assert "fileXioRmdir" not in all_source

# The proprietary bootstrap source is host-only and cannot enter EE sources.
assert "__mbr.211" not in all_source
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
assert "mbr_bootstrap_prefix.bin" not in makefile

print("source safety audit: authorized multi-capacity READY_40GB scope: PASS")
