// Host versions of the epd_driver drawing primitives screen.h uses. They write
// the same 4-bit framebuffer format as the real driver.
#include <stdlib.h>
#include "epd_driver.h"

void epd_draw_pixel(int32_t x, int32_t y, uint8_t color, uint8_t *fb) {
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
    uint8_t *p = &fb[y * EPD_WIDTH / 2 + x / 2];
    if (x % 2) *p = (*p & 0x0F) | (color & 0xF0);
    else       *p = (*p & 0xF0) | (color >> 4);
}

void epd_draw_hline(int32_t x, int32_t y, int32_t len, uint8_t color, uint8_t *fb) {
    for (int32_t i = 0; i < len; i++) epd_draw_pixel(x + i, y, color, fb);
}

void epd_draw_vline(int32_t x, int32_t y, int32_t len, uint8_t color, uint8_t *fb) {
    for (int32_t i = 0; i < len; i++) epd_draw_pixel(x, y + i, color, fb);
}

void epd_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color, uint8_t *fb) {
    for (int32_t i = 0; i < h; i++) epd_draw_hline(x, y + i, w, color, fb);
}

void epd_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color, uint8_t *fb) {
    epd_draw_hline(x, y, w, color, fb);
    epd_draw_hline(x, y + h - 1, w, color, fb);
    epd_draw_vline(x, y, h, color, fb);
    epd_draw_vline(x + w - 1, y, h, color, fb);
}

// Same midpoint algorithm as the library, so previews match the panel pixel for pixel.
void epd_fill_circle(int32_t x0, int32_t y0, int32_t r, uint8_t color, uint8_t *fb) {
    epd_draw_vline(x0, y0 - r, 2 * r + 1, color, fb);
    int32_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r, px = x, py = y;
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        if (x < y + 1) {
            epd_draw_vline(x0 + x, y0 - y, 2 * y + 1, color, fb);
            epd_draw_vline(x0 - x, y0 - y, 2 * y + 1, color, fb);
        }
        if (y != py) {
            epd_draw_vline(x0 + py, y0 - px, 2 * px + 1, color, fb);
            epd_draw_vline(x0 - py, y0 - px, 2 * px + 1, color, fb);
            py = y;
        }
        px = x;
    }
}

void epd_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color, uint8_t *fb) {
    int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        epd_draw_pixel(x0, y0, color, fb);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Only used by write_mode() when drawing without a framebuffer.
void epd_draw_image(Rect_t area, uint8_t *data, DrawMode_t mode) {
    (void)area; (void)data; (void)mode;
}
