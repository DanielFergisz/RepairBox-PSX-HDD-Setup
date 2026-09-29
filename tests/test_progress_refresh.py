#!/usr/bin/env python3
"""Guard incremental refresh on long-running installer progress screens."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
psx1 = (ROOT / "src/psx1_pipeline.c").read_text(encoding="utf-8")
installer = (ROOT / "src/installer.c").read_text(encoding="utf-8")
apps = (ROOT / "src/apps/apps_ui.c").read_text(encoding="utf-8")


def function_body(source: str, signature: str, next_signature: str) -> str:
    end = source.index(next_signature)
    start = source.rindex(signature, 0, end)
    return source[start:end]


xfrom = function_body(
    main, "static void xfrom_progress_callback(",
    "static void draw_psx2_result("
)
assert "if (!state->screen_ready)" in xfrom
assert xfrom.count("ui_begin();") == 1
assert "ui_progress_line(148, line);" in xfrom

psx2 = function_body(
    main, "static void psx2_progress_callback(",
    "static void draw_activation_progress("
)
assert "state->stage != stage" in psx2
assert psx2.count("ui_begin();") == 1
assert "ui_progress_line(156" in psx2

activation = function_body(
    main, "static void draw_activation_progress(",
    "static const char *xfrom_progress_label("
)
assert "phase == DR40_PROGRESS_BEGIN" in activation
assert activation.count("ui_begin();") == 1

operation = function_body(
    psx1, "static void draw_operation(", "static void format_worker("
)
assert "int changed =" in operation
assert operation.count("ui_begin();") == 1
assert 'ui_printf("%-54.54s"' in operation

recovery = function_body(
    psx1, "static void draw_recovery_cleanup(",
    "static int cleanup_old_apa_headers("
)
assert "if (!*screen_ready)" in recovery
assert recovery.count("ui_begin();") == 1

# Existing copy progress implementations already use one clear followed by
# fixed-position line updates; keep that known-good behavior intact.
copy_progress = function_body(
    installer, "static void show_progress(",
    "static int hash_open_file("
)
assert "if (!progress_screen_ready)" in copy_progress
assert copy_progress.count("ui_begin();") == 1

app_progress = function_body(
    apps, "static void draw_progress(", "static void draw_result("
)
assert "if (!state->screen_ready)" in app_progress
assert app_progress.count("ui_begin();") == 1

assert "draw_xfrom_progress" not in main
print("incremental progress refresh and no-flicker guards: PASS")
