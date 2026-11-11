#include "app/screen_content.h"
#include "screen/debug_screen.h"
#include "screen/framebuffer.h"
#include "screen/render.h"
#include "screen/status_screen.h"

const uint8_t *render_frame(const app_state_t *state)
{
    framebuffer_clear();
    if (RENDER_DEBUG_SCREEN) {
        debug_screen_draw(state);
    } else {
        screen_content_t content;
        screen_content_of(state, &content);
        status_screen_draw(&content);
    }
    return framebuffer_data();
}

bool render_is_outdated(const app_state_t *drawn, const app_state_t *now)
{
    if (RENDER_DEBUG_SCREEN) {
        return debug_screen_is_outdated(drawn, now);
    }
    screen_content_t drawn_content, now_content;
    screen_content_of(drawn, &drawn_content);
    screen_content_of(now, &now_content);
    return !screen_content_equal(&drawn_content, &now_content);
}
