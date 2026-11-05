#include "giant/platform/window.h"

#include "giant_test.h"

#include <SDL3/SDL.h>

#include <memory>

using namespace giant;
using namespace giant::platform;

GIANT_TEST("platform: a hidden window opens with the requested size") {
    WindowDesc desc;
    desc.title = "giant test";
    desc.width = 320;
    desc.height = 200;
    desc.hidden = true;
    auto window = Window::create(desc);
    GIANT_CHECK(window != nullptr);
    if (window == nullptr) {
        return;
    }
    GIANT_CHECK(window->pixel_width() == 320);
    GIANT_CHECK(window->pixel_height() == 200);
    GIANT_CHECK(window->poll_events());
    GIANT_CHECK(!window->take_resized());
}

GIANT_TEST("platform: windows can be opened and closed repeatedly") {
    for (int i = 0; i < 3; ++i) {
        WindowDesc desc;
        desc.hidden = true;
        auto window = Window::create(desc);
        GIANT_CHECK(window != nullptr);
    }
}

namespace {
// SDL is visible to this test only, to post the events an OS would send.
void push_window_event(Window& window, SDL_EventType type) {
    SDL_Event event{};
    event.type = static_cast<Uint32>(type);
    event.window.windowID = window.sdl_window_id_for_tests();
    SDL_PushEvent(&event);
}

std::unique_ptr<Window> hidden_window(u32 w, u32 h) {
    WindowDesc desc;
    desc.width = w;
    desc.height = h;
    desc.hidden = true;
    return Window::create(desc);
}
} // namespace

GIANT_TEST("platform: a size change is reported once") {
    auto window = hidden_window(320, 200);
    GIANT_CHECK(window != nullptr);
    if (window == nullptr) {
        return;
    }
    window->request_size(400, 300);
    GIANT_CHECK(window->poll_events());
    GIANT_CHECK(window->take_resized());
    GIANT_CHECK(!window->take_resized());
    GIANT_CHECK(window->pixel_width() == 400);
    GIANT_CHECK(window->pixel_height() == 300);
    // The dummy driver has no high-DPI scaling, so points equal pixels.
    GIANT_CHECK(window->point_width() == 400);
    GIANT_CHECK(window->point_height() == 300);
}

GIANT_TEST("platform: minimise and restore are tracked") {
    auto window = hidden_window(320, 200);
    GIANT_CHECK(window != nullptr);
    if (window == nullptr) {
        return;
    }
    // The dummy video driver has no minimise, so post the events the OS
    // would send; this tests our handling of them.
    GIANT_CHECK(!window->minimized());
    push_window_event(*window, SDL_EVENT_WINDOW_MINIMIZED);
    GIANT_CHECK(window->poll_events());
    GIANT_CHECK(window->minimized());
    push_window_event(*window, SDL_EVENT_WINDOW_RESTORED);
    GIANT_CHECK(window->poll_events());
    GIANT_CHECK(!window->minimized());
}

GIANT_TEST("platform: a close request ends the loop") {
    auto window = hidden_window(320, 200);
    GIANT_CHECK(window != nullptr);
    if (window == nullptr) {
        return;
    }
    push_window_event(*window, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
    GIANT_CHECK(!window->poll_events());
}

GIANT_TEST("platform: events for another window are ignored") {
    auto a = hidden_window(320, 200);
    auto b = hidden_window(320, 200);
    GIANT_CHECK(a != nullptr && b != nullptr);
    if (a == nullptr || b == nullptr) {
        return;
    }
    // Both windows share SDL's queue; whichever polls must ignore the other's.
    push_window_event(*b, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
    push_window_event(*b, SDL_EVENT_WINDOW_MINIMIZED);
    b->request_size(500, 400);
    GIANT_CHECK(a->poll_events());
    GIANT_CHECK(!a->take_resized());
    GIANT_CHECK(!a->minimized());
    GIANT_CHECK(a->pixel_width() == 320);
}
