#!/usr/bin/env python3
"""Lock the public wLaunchELF DVR PFS slot mapping and EBUSY policy."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
pipeline = (ROOT / "src/direct_ready40.c").read_text(encoding="utf-8")
installer = (ROOT / "src/installer.c").read_text(encoding="utf-8")

# Public wLaunchELF convention: xcontents is slot 0, xdata is slot 1.
assert ('{"__xdata", "dvr_hdd0:__xdata", "dvr_pfs1:", '
        '"dvr_pfs1:/", 8192u}') in pipeline
assert ('{"__xcontents", "dvr_hdd0:__xcontents", "dvr_pfs0:",') in pipeline
assert ('{"__xdata", "dvr_hdd0:__xdata", "dvr_pfs1:", '
        '"dvr_pfs1:", 1}') in installer
assert ('{"__xcontents", "dvr_hdd0:__xcontents", "dvr_pfs0:", '
        '"dvr_pfs0:", 1}') in installer

# EBUSY is accepted only on DVR canonical slots, which must then be probed.
assert 'allow_preexisting && pfs->mount_result == -EBUSY' in pipeline
assert 'allow_preexisting_mount &&' in installer
assert 'probe_preexisting_mount' in installer
assert 'partition->mount_probe_result = fileXioDopen(root_path)' in installer

# A pre-existing mount is never unmounted by this process.
assert 'if (pfs->mount_owned) {' in pipeline
assert 'if (partition->mount_owned) {' in installer
assert 'pfs_failure_return(&result->dvr_pfs[index])' in pipeline

print("DVR PFS canonical mapping and mount-ownership policy: PASS")
