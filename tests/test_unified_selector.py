#!/usr/bin/env python3
"""Selector must choose one isolated storage stack before any HDD IRX load."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
storage = (ROOT / "src/storage.c").read_text(encoding="utf-8")
inspector = (ROOT / "src/psx1_inspector.c").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")

assert "PSX1 - First Revision" in main
assert "PSX2 - Second Revision" in main
assert '"[ L1 ]  PSX1"' in main
assert '"[ R1 ]  PSX2"' in main
for model in ("DESR-5000", "DESR-5100", "DESR-7000", "DESR-7100",
              "DESR-5500", "DESR-5700", "DESR-7500", "DESR-7700"):
    assert model in main
assert "pressed & PAD_L1" in main
assert "pressed & PAD_R1" in main
assert "selected_revision_t" in main
selector = main[main.index("static selected_revision_t select_revision"):]
selector = selector[:selector.index("static void draw_psx1_preflight")]
assert "return REVISION_PSX1;" in selector
assert "return REVISION_PSX2;" in selector
assert "confirmed" not in selector
assert "PAD_CROSS" not in selector
assert "This selection is locked" not in main
assert "revision = select_revision(&pad);" in main
main_body = main[main.index("int main("):]
assert main_body.index("select_revision(&pad)") < main_body.index("storage_initialize")
assert main_body.index("select_revision(&pad)") < main_body.index("run_psx1_ui")
assert "if (revision == REVISION_PSX1)" in main_body
assert "else if (revision == REVISION_PSX2)" in main_body
assert main_body.count("storage_initialize(&psx2_diagnostics)") == 1
assert "ps2hdd_psx1_irx" in inspector
assert "ps2hdd_irx" in storage
assert "ps2hdd_psx1_irx.c" in makefile
assert "ps2hdd-sparse-skip.irx" in makefile
assert "SifIopReset" not in main

print("explicit revision selector and mutually exclusive module loaders: PASS")
