#!/usr/bin/env python3
"""Guard production UI, embedded logo and deterministic GIF DMA storage."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
ui = (ROOT / "src/ui.c").read_text(encoding="utf-8")
logo = (ROOT / "include/repairbox_logo.h").read_text(encoding="utf-8")

assert "static ui_dma_buffers_t dma_buffers" in ui
assert "__attribute__((aligned(256)))" in ui
assert "sizeof(ui_dma_buffers_t) == 512" in ui
assert "dma_send(&dma_buffers.setup, 6)" in ui
assert "dma_send(dma_buffers.pixels, UI_GLYPH_QWORDS)" in ui
assert "#define REPAIRBOX_LOGO_WIDTH 192" in logo
assert "#define REPAIRBOX_LOGO_HEIGHT 16" in logo
assert "#define REPAIRBOX_LOGO_STRIDE 24" in logo
assert logo.count("0x") == 384
assert main.count("ui_draw_repairbox_logo(408, 168);") == 2
assert main.count('ui_inverse_status("FULL POWER OFF REQUIRED");') == 2
assert main.count('ui_printf("Power off the PSX.\\n");') == 2
assert main.count('ui_printf("Disconnect AC power before restarting.\\n");') == 2
assert main.count('ui_printf("Reconnect and start the PSX normally.\\n");') == 2
assert "void ui_draw_repairbox_logo(int x, int y)" in ui
assert "malloc(" not in ui and "calloc(" not in ui

# Production contains no report writer, report hotkey or report UI. There is
# still no automatic checkpoint or original-bootflag backup.
all_source = "\n".join(
    path.read_text(encoding="utf-8")
    for folder in (ROOT / "src", ROOT / "include")
    for path in folder.glob("*.[ch]")
)
assert "initializer_write_report(" not in main
assert "psx1_report_save_usb(" not in main
assert "PAD_SQUARE" not in main
assert "SQUARE Save report" not in main
assert "bootflag_original.bin" not in all_source
assert "mass:/RepairBox_PSX2" not in all_source
assert "_Report.txt" not in all_source
assert not (ROOT / "src/report.c").exists()
assert not (ROOT / "src/psx1_report.c").exists()
assert "Disconnect AC power before restarting." in all_source
assert "Reconnect and start the PSX normally." in all_source
assert "System package NOT REQUIRED THIS PASS" in main

installer = (ROOT / "src/installer.c").read_text(encoding="utf-8")
assert '"File %u/%u Try %u/%u  Name: %s"' in installer
assert '"Current file: %llu%%   Size: %s"' in installer
for unit in ('"%llu B"', '"KiB"', '"MiB"', '"GiB"'):
    assert unit in installer

print("production UI, embedded logo and GIF DMA workspace policy: PASS")
