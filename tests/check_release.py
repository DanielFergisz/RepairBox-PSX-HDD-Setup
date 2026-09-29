from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: check_release.py FLAVOR FILE.elf")
flavor, name = sys.argv[1:]
root = Path(__file__).resolve().parents[1]
elf = Path(name)
raw = elf.read_bytes()
assert b"PSX HDD + Apps Setup v1.3" in raw
assert b"PSX HDD + Apps Setup v1.3 RC" not in raw
assert b"SifIopReset" not in raw
assert b"xfromWrite" not in raw
assert b"repair_bootflag_digest" not in raw
assert b"RepairBox-PSX2-SystemFiles" in raw
assert b"RepairBox-PSX2-Bootstrap" in raw
assert b"RepairBox-PSX2-XFROM" in raw
assert b"RepairBox-PSX1-XFROM" in raw

if flavor == "MMCE":
    assert b"MMCE" in raw
    for forbidden in ("bdm", "bdmfs_fatfs", "mx4sio_bd", "usbd",
                      "usbmass_bd"):
        assert (root / "irx/pinned" / (forbidden + ".irx")).read_bytes() not in raw, forbidden
    assert (root / "irx/mmceman.irx").read_bytes() in raw
elif flavor == "USB-MX4SIO":
    assert b"USB/MX4SIO" in raw
    assert (root / "irx/mmceman.irx").read_bytes() not in raw
    for required in ("bdm_irx", "bdmfs_fatfs_irx", "mx4sio_bd_irx",
                     "usbd_irx", "usbmass_bd_irx"):
        asset = required.removesuffix("_irx") + ".irx"
        assert (root / "irx/pinned" / asset).read_bytes() in raw, required
else:
    raise AssertionError(flavor)
print(f"{flavor} release module isolation and safety audit: PASS")
