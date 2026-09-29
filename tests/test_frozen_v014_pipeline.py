#!/usr/bin/env python3
"""Guard the golden storage sequence plus authorized recovery stages."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIPELINE = (ROOT / "src/direct_ready40.c").read_text(encoding="utf-8")
MULTI = (ROOT / "src/direct_ready40_multi.c").read_text(encoding="utf-8")
MAIN = (ROOT / "src" / "main.c").read_text(encoding="utf-8")
ACTIVATION = (ROOT / "src" / "activation.c").read_text(encoding="utf-8")

for source in (PIPELINE, MULTI):
    # RC12 adds only best-effort stale DVR unmounting before the first
    # destructive formatter. The verified geometry and ordered stages remain.
    assert source.count("cleanup_stale_dvr_mounts(result);") == 1
    assert source.index("cleanup_stale_dvr_mounts(result);") < source.index(
        'fileXioFormat("hdd0:", NULL, NULL, 0)'
    )
    assert source.count(
        'dvr_pfs0_cleanup_return = fileXioUmount("dvr_pfs0:")'
    ) == 1
    assert source.count(
        'dvr_pfs1_cleanup_return = fileXioUmount("dvr_pfs1:")'
    ) == 1
    assert source.count('fileXioFormat("hdd0:", NULL, NULL, 0)') == 1
    assert source.count('fileXioFormat("dvr_hdd0:", NULL, NULL, 0)') == 1
    assert "activation_arm_pending" not in source

assert MAIN.count("activation_arm_pending") == 1
assert MAIN.index("xfrom_repair_execute") < MAIN.index("dr40_execute")
assert MAIN.index("dr40_execute") < MAIN.index("activation_arm_pending")
assert "result->already_armed =" in ACTIVATION
assert '"PENDING - PRESERVE"' in MAIN
assert '"PSX2 READY TO REINSTALL"' in MAIN
assert '"VERIFY EXISTING XFROM 40/1"' in MAIN
assert "activation_verify_existing_pending" in MAIN
assert "activation.generation_valid || activation.already_armed" in MAIN
assert "ACTIVATE XFROM 40/1" in MAIN
assert "if (result.direct_ready40_storage_valid &&" in MAIN
assert "bootflag_original.bin" not in ACTIVATION

print("golden PSX2 sequence plus authorized RC12 DVR cleanup: PASS")
