#!/usr/bin/env python3
"""Host policy checks for the exact Direct READY_40GB experiment."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "direct_ready40.c").read_text(encoding="utf-8")
HEADER = (ROOT / "include" / "direct_ready40.h").read_text(encoding="utf-8")
INSTALLER = (ROOT / "src" / "installer.c").read_text(encoding="utf-8")

SYSTEM = {
    "__mbr": (0x00000000, 0x00040000),
    "__net": (0x00040000, 0x00040000),
    "__system": (0x00080000, 0x00080000),
    "__sysconf": (0x00100000, 0x00100000),
    "__common": (0x00200000, 0x00200000),
}
DVR = {
    "__extend": (0x04A817C8, 0x00040000),
    "__xdata": (0x04AC17C8, 0x00200000),
    "__xcontents": (0x04CC17C8, 0x190BE038),
}

assert SYSTEM["__common"][0] + SYSTEM["__common"][1] == 0x00400000
assert DVR["__xcontents"][0] + DVR["__xcontents"][1] == 0x1DD7F800
assert 0x1DD80000 - 0x1DD7F800 == 0x800
assert "#define DR40_VISIBLE_SECTORS 0x04A817C8u" in HEADER
assert "#define DR40_NATIVE_MAX 0x1DD80000u" in HEADER
for name, (start, size) in {**SYSTEM, **DVR}.items():
    assert f'"{name}", 0x{start:08X}u, 0x{size:08X}u' in SOURCE

positions = [
    SOURCE.index('fileXioFormat("hdd0:", NULL, NULL, 0)'),
    SOURCE.index("format_and_validate_pfs(&result->normal_pfs[index]"),
    SOURCE.index('fileXioFormat("dvr_hdd0:", NULL, NULL, 0)'),
    SOURCE.index("format_and_validate_pfs(&result->dvr_pfs[index]"),
    SOURCE.index("bootstrap_write_and_verify"),
    SOURCE.index("installer_execute"),
]
assert positions == sorted(positions)
assert ('{"__xdata", "dvr_hdd0:__xdata", "dvr_pfs1:", '
        '"dvr_pfs1:/", 8192u}') in SOURCE
assert ('{"__xcontents", "dvr_hdd0:__xcontents", "dvr_pfs0:",') in SOURCE
assert '"dvr_pfs0:/", 0x00100000u}' in SOURCE

for partition in ("__net", "__system", "__sysconf", "__common",
                  "__xdata", "__xcontents"):
    assert f'{{"{partition}",' in INSTALLER
assert "manifest" not in INSTALLER.lower()
assert "source_size != destination_size" in INSTALLER
assert "memcmp(source_digest, destination_digest" in INSTALLER
assert "unsafe_source_component" in INSTALLER
assert "unknown_top_level_names" in INSTALLER

print("Direct READY_40GB geometry, ordering, zones, and package policy: PASS")
