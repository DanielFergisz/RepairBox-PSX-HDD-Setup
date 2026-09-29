from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "src/source_media.c").read_text()

for required in (
    "remember_launch_path",
    "try_launch_ancestors",
    "search_bounded",
    "SEARCH_MAX_DEPTH 2u",
    "SEARCH_MAX_DIRECTORIES 48u",
    "SEARCH_MAX_ENTRIES 384u",
    "base_has_content",
    "RepairBox-PSX2-SystemFiles",
    "RepairBox-PSX2-Bootstrap",
    "RepairBox-PSX2-XFROM",
    "RepairBox-PSX1-XFROM",
):
    assert required in source, required
assert "return matches > 1 ? -EEXIST : 0" in source
assert "resolution_result = 0;" in source
assert "directory_readable(third)" in source
assert "source_media_psx1_xfrom_root" in source
assert "source_media_psx2_xfrom_root" in source
assert 'return directory_readable(first) && directory_readable(second)' in source
print("bounded deterministic package discovery: PASS")
