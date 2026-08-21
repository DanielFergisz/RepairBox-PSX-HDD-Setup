#include <debug.h>
#include <ee_regs.h>
#include <kernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <SDL/SDL_gfxPrimitives_font.h>

#include "repairbox_logo.h"
#include "ui.h"

#define UI_FRAME_WIDTH 640
#define UI_FRAME_HEIGHT 224
#define UI_GLYPH_WIDTH 8
#define UI_GLYPH_HEIGHT 8
#define UI_ADVANCE 10
#define UI_LINE_HEIGHT 12
#define UI_FORMAT_BUFFER_SIZE 2048
#define UI_GLYPH_QWORDS 16
#define UI_WHITE 0x00FFFFFFu
#define UI_BLACK 0x00000000u

typedef struct ui_setupchar {
    u64 dd0[4];
    u32 dw0[1];
    u16 x;
    u16 y;
    u64 dd1[1];
    u32 dw1[2];
    u64 dd2[5];
} ui_setupchar_t;

typedef struct ui_dma_buffers {
    ui_setupchar_t setup;
    u8 setup_padding[256 - sizeof(ui_setupchar_t)];
    u32 pixels[64];
} ui_dma_buffers_t;

_Static_assert(sizeof(ui_setupchar_t) == 96,
               "GIF setup packet size changed");
_Static_assert(sizeof(ui_dma_buffers_t) == 512,
               "GIF DMA buffer layout changed");

static ui_dma_buffers_t dma_buffers __attribute__((aligned(256)));

static const ui_setupchar_t setup_template = {
    {0x1000000000000004ULL, 0xE, 0xA000000000000ULL, 0x50},
    {0},
    100,
    100,
    {0x51},
    {8, 8},
    {0x52, 0, 0x53, 0x0800000000008010ULL, 0},
};

static int cursor_x = UI_SAFE_LEFT;
static int cursor_y = UI_SAFE_TOP;
static u32 active_background = UI_WHITE;
static u32 active_foreground = UI_BLACK;

static int is_dense_character(unsigned char character)
{
    return strchr("mMwW8BRO0aegs@%&", (int)character) != NULL;
}

static u8 render_row(unsigned char character, int row)
{
    const u8 *glyph =
        (const u8 *)&gfxPrimitivesFontdata[(unsigned int)character * 8u];
    u8 bits = glyph[row];

    if (is_dense_character(character))
        return bits;
    return (u8)(bits | (bits >> 1));
}

static void dma_wait(void)
{
    while ((*R_EE_D2_CHCR & 0x100) != 0) {
    }
}

static void dma_send(const void *address, int qwords)
{
    *R_EE_D2_QWC = (u32)qwords;
    *R_EE_D2_MADR = (u32)address;
    *R_EE_D2_CHCR = 0x101;
}

static void draw_character(int x, int y, u32 foreground, u32 background,
                           unsigned char character)
{
    ui_setupchar_t *setup =
        (ui_setupchar_t *)UNCACHED_SEG(&dma_buffers.setup);
    int row;
    int column;

    *setup = setup_template;
    setup->x = (u16)x;
    setup->y = (u16)y;

    for (row = 0; row < UI_GLYPH_HEIGHT; ++row) {
        u8 bits = render_row(character, row);

        for (column = 0; column < UI_GLYPH_WIDTH; ++column) {
            u32 pixel = (bits & (0x80u >> column))
                            ? foreground
                            : background;
            *(u32 *)UNCACHED_SEG(
                &dma_buffers.pixels[row * UI_GLYPH_WIDTH + column]) = pixel;
        }
    }

    dma_wait();
    dma_send(&dma_buffers.setup, 6);
    dma_wait();
    dma_send(dma_buffers.pixels, UI_GLYPH_QWORDS);
    dma_wait();
}

static void next_line(void)
{
    cursor_x = UI_SAFE_LEFT;
    cursor_y += UI_LINE_HEIGHT;
}

static void put_character(char character)
{
    if (cursor_x + UI_GLYPH_WIDTH > UI_FRAME_WIDTH - UI_SAFE_RIGHT)
        next_line();
    if (cursor_y + UI_GLYPH_HEIGHT > UI_FRAME_HEIGHT - UI_SAFE_BOTTOM)
        return;

    draw_character(cursor_x, cursor_y, active_foreground,
                   active_background, (unsigned char)character);
    cursor_x += UI_ADVANCE;
}

void ui_init(void)
{
    scr_setCursor(0);
    ui_begin();
}

void ui_begin(void)
{
    active_background = UI_WHITE;
    active_foreground = UI_BLACK;
    scr_setbgcolor(active_background);
    scr_setfontcolor(active_foreground);
    scr_setcursorcolor(active_foreground);
    scr_clear();
    cursor_x = UI_SAFE_LEFT;
    cursor_y = UI_SAFE_TOP;
}

void ui_printf(const char *format, ...)
{
    char buffer[UI_FORMAT_BUFFER_SIZE];
    va_list arguments;
    int length;
    int index;

    va_start(arguments, format);
    length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length < 0)
        return;
    if (length >= (int)sizeof(buffer))
        length = (int)sizeof(buffer) - 1;

    for (index = 0; index < length; ++index) {
        if (buffer[index] == '\n')
            next_line();
        else if (buffer[index] == '\r')
            cursor_x = UI_SAFE_LEFT;
        else
            put_character(buffer[index]);
    }
}

void ui_set_position(int x, int y)
{
    if (x < UI_SAFE_LEFT)
        x = UI_SAFE_LEFT;
    if (x > UI_FRAME_WIDTH - UI_SAFE_RIGHT - UI_GLYPH_WIDTH)
        x = UI_FRAME_WIDTH - UI_SAFE_RIGHT - UI_GLYPH_WIDTH;
    if (y < UI_SAFE_TOP)
        y = UI_SAFE_TOP;
    if (y > UI_FRAME_HEIGHT - UI_SAFE_BOTTOM - UI_GLYPH_HEIGHT)
        y = UI_FRAME_HEIGHT - UI_SAFE_BOTTOM - UI_GLYPH_HEIGHT;
    cursor_x = x;
    cursor_y = y;
}

void ui_inverse_status(const char *text)
{
    int panel_x;
    int text_width = (int)strlen(text) * UI_ADVANCE;
    int text_x = UI_SAFE_LEFT +
                 (UI_FRAME_WIDTH - UI_SAFE_LEFT - UI_SAFE_RIGHT -
                  text_width) / 2;

    for (panel_x = UI_SAFE_LEFT;
         panel_x + UI_GLYPH_WIDTH <= UI_FRAME_WIDTH - UI_SAFE_RIGHT;
         panel_x += UI_GLYPH_WIDTH)
        draw_character(panel_x, cursor_y, UI_WHITE, UI_BLACK, ' ');

    if (text_x < UI_SAFE_LEFT)
        text_x = UI_SAFE_LEFT;
    while (*text != '\0' &&
           text_x + UI_GLYPH_WIDTH <= UI_FRAME_WIDTH - UI_SAFE_RIGHT) {
        draw_character(text_x, cursor_y, UI_WHITE, UI_BLACK,
                       (unsigned char)*text++);
        text_x += UI_ADVANCE;
    }
    next_line();
}

void ui_draw_repairbox_logo(int x, int y)
{
    ui_setupchar_t *setup =
        (ui_setupchar_t *)UNCACHED_SEG(&dma_buffers.setup);
    int tile_x;
    int tile_y;
    int row;
    int column;

    if (x < UI_SAFE_LEFT || y < UI_SAFE_TOP ||
        x + REPAIRBOX_LOGO_WIDTH > UI_FRAME_WIDTH - UI_SAFE_RIGHT ||
        y + REPAIRBOX_LOGO_HEIGHT > UI_FRAME_HEIGHT - UI_SAFE_BOTTOM)
        return;

    for (tile_y = 0; tile_y < REPAIRBOX_LOGO_HEIGHT; tile_y += 8) {
        for (tile_x = 0; tile_x < REPAIRBOX_LOGO_WIDTH; tile_x += 8) {
            *setup = setup_template;
            setup->x = (u16)(x + tile_x);
            setup->y = (u16)(y + tile_y);
            for (row = 0; row < 8; ++row) {
                u8 bits = repairbox_logo_bits[
                    (tile_y + row) * REPAIRBOX_LOGO_STRIDE + tile_x / 8];

                for (column = 0; column < 8; ++column) {
                    u32 pixel = (bits & (0x80u >> column))
                                    ? UI_BLACK
                                    : UI_WHITE;
                    *(u32 *)UNCACHED_SEG(
                        &dma_buffers.pixels[row * 8 + column]) = pixel;
                }
            }
            dma_wait();
            dma_send(&dma_buffers.setup, 6);
            dma_wait();
            dma_send(dma_buffers.pixels, UI_GLYPH_QWORDS);
            dma_wait();
        }
    }
}

void ui_sync(void)
{
    dma_wait();
    __asm__ volatile("sync.l" ::: "memory");
}
