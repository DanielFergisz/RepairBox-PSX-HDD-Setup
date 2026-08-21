#ifndef REPAIRBOX_SYSTEM_FILES_DUMPER_UI_H
#define REPAIRBOX_SYSTEM_FILES_DUMPER_UI_H

#define UI_SAFE_LEFT 40
#define UI_SAFE_RIGHT 40
#define UI_SAFE_TOP 16
#define UI_SAFE_BOTTOM 16

void ui_init(void);
void ui_begin(void);
void ui_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
void ui_set_position(int x, int y);
void ui_inverse_status(const char *text);
void ui_draw_repairbox_logo(int x, int y);
void ui_sync(void);

#endif
