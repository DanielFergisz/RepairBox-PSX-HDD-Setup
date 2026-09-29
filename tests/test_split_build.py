from pathlib import Path

root = Path(__file__).resolve().parents[1]
makefile = (root / "Makefile").read_text()
profile = (root / "include/build_profile.h").read_text()
source = (root / "src/source_media.c").read_text()
storage = (root / "src/storage.c").read_text()

assert "FLAVOR ?= USB-MX4SIO" in makefile
assert "SOURCE_IRX = mmceman" in makefile
assert "SOURCE_IRX = bdm bdmfs_fatfs mx4sio_bd usbd usbmass_bd" in makefile
assert "-DRBX_BUILD_MMCE=$(BUILD_MMCE)" in makefile
assert "usbhdfsd" not in makefile
assert "RBX_FIRST_SOURCE_INDEX" in profile
assert "#if !RBX_BUILD_MMCE" in source
assert "#if RBX_BUILD_MMCE" in source
assert "Removable-media drivers are loaded exclusively" in storage
print("split MMCE and USB/MX4SIO build contracts: PASS")
