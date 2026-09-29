from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src" / "main.c").read_text(encoding="utf-8")
apps_ui = (ROOT / "src" / "apps" / "apps_ui.c").read_text(encoding="utf-8")
installer = (ROOT / "src" / "apps" / "app_installer.c").read_text(
    encoding="utf-8"
)
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")

assert '#define PROGRAM_TITLE RBX_PROGRAM_TITLE' in main
assert 'RBX_PROGRAM_TITLE "RepairBox.pl PSX HDD + Apps Setup v1.3 "' in (
    ROOT / "include" / "build_profile.h"
).read_text(encoding="utf-8")
assert "REVISION_APPS_ONLY" in main
assert "PAD_TRIANGLE" in main
assert "apps_ui_run();" in main
assert "\\x1e  APPS" in main

# The Apps path is the standalone v1.0 workflow. It is selected explicitly
# and does not participate in either destructive HDD installation path.
psx1_branch = main.split("if (revision == REVISION_PSX1)", 1)[1]
assert "apps_ui_run();" not in psx1_branch.split(
    "else if (revision == REVISION_APPS_ONLY)", 1
)[0]
assert "storage_initialize_existing_system" in apps_ui
assert "system_version_detect" in apps_ui
assert "app_scan_catalog" in apps_ui
assert "app_install(" in apps_ui
assert ".stage" not in apps_ui
assert "stage marker" not in apps_ui.lower()
assert "installer_report_save" not in apps_ui
assert "Errors are skipped" not in apps_ui
assert "catalog->valid_count < catalog->count" in apps_ui

for required in [
    "write_direct_kelf_verified",
    "register_xmb_entry",
    "app_scan_managed_partitions",
    "app_uninstall_managed_partitions",
    'fileXioRename("pfs0:/EXECUTE.NEW", "pfs0:/EXECUTE.KELF")',
    'memcmp(readback_sector, "PS2ICON3D", 9)',
    'memcmp(readback_sector, "PS2X", 4)',
]:
    assert required in installer, required

assert "src/apps/apps_ui" in makefile
assert "src/apps/app_installer" in makefile
assert "$(BUILD_DIR)/default_cover_png.o" in makefile
print("standalone v1.0 Apps workflow integration: PASS")
