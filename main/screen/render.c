#include "screen/framebuffer.h"
#include "screen/debug_screen.h"
#include "screen/render.h"
#include "screen/status_screen.h"

const uint8_t *render_frame(const render_content_t *content)
{
    framebuffer_clear();
    if (RENDER_DEBUG_SCREEN) {
        debug_screen_draw(content);
    } else {
        status_screen_draw(content);
    }
    return framebuffer_data();
}
