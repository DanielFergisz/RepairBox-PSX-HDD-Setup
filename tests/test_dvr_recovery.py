#!/usr/bin/env python3
"""Guard restart recovery from stale automatic DVR mounts."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include/direct_ready40.h").read_text(encoding="utf-8")

for name in ("direct_ready40.c", "direct_ready40_multi.c"):
    source = (ROOT / "src" / name).read_text(encoding="utf-8")
    call = source.index("cleanup_stale_dvr_mounts(result);")
    mode_gate = source.index("if (result->mode != DR40_MODE_INITIALIZE)")
    apa_format = source.index('fileXioFormat("hdd0:", NULL, NULL, 0)')
    dvr_format = source.index('fileXioFormat("dvr_hdd0:", NULL, NULL, 0)')

    assert mode_gate < call < apa_format < dvr_format
    assert source.count(
        'dvr_pfs0_cleanup_return = fileXioUmount("dvr_pfs0:")'
    ) == 1
    assert source.count(
        'dvr_pfs1_cleanup_return = fileXioUmount("dvr_pfs1:")'
    ) == 1

for field in (
    "dvr_mount_cleanup_attempted",
    "dvr_pfs0_cleanup_return",
    "dvr_pfs1_cleanup_return",
):
    assert field in HEADER

print("stale DVR mount restart recovery: PASS")
