#include <string.h>
#include "framebuffer.h"

static uint8_t framebuffer[FRAMEBUFFER_SIZE];

void framebuffer_clear(void)
{
    memset(framebuffer, 0xFF, sizeof(framebuffer));
}

void framebuffer_set_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= EPAPER_WIDTH || y < 0 || y >= EPAPER_HEIGHT) {
        return;
    }
    uint8_t *byte = &framebuffer[y * EPAPER_ROW_BYTES + x / 8];
    uint8_t  mask = 0x80 >> (x & 7);
    *byte = black ? *byte & ~mask : *byte | mask;
}

void framebuffer_fill_rect(int x, int y, int width, int height, bool black)
{
    for (int row = 0; row < height; row++) {
        for (int column = 0; column < width; column++) {
            framebuffer_set_pixel(x + column, y + row, black);
        }
    }
}

void framebuffer_blit(int x, int y, int width, int height, const uint8_t *bits, bool black)
{
    int stride = (width + 7) / 8;
    for (int row = 0; row < height; row++) {
        for (int column = 0; column < width; column++) {
            if (bits[row * stride + column / 8] & (0x80 >> (column & 7))) {
                framebuffer_set_pixel(x + column, y + row, black);
            }
        }
    }
}

const uint8_t *framebuffer_data(void)
{
    return framebuffer;
}
