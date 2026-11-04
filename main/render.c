#include "debug_screen.h"
#include "framebuffer.h"
#include "render.h"
#include "status_screen.h"

const uint8_t *render_status(const render_view_t *v)
{
    framebuffer_clear();
    if (RENDER_DEBUG_SCREEN) {
        render_debug(v);
    } else {
        screen_draw(v);
    }
    return framebuffer_data();
}
