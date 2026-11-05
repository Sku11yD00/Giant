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

namespace {
void push(SDL_Event event) { SDL_PushEvent(&event); }
} // namespace

GIANT_TEST("platform: mouse, keyboard and text input arrive as input events") {
    auto window = hidden_window(320, 200);
    auto other = hidden_window(320, 200);
    GIANT_CHECK(window != nullptr && other != nullptr);
    if (window == nullptr || other == nullptr) {
        return;
    }
    const SDL_WindowID id = window->sdl_window_id_for_tests();

    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.x = 12.5f;
    e.motion.y = 40.0f;
    push(e);

    e = {};
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = id;
    e.button.button = SDL_BUTTON_RIGHT;
    e.button.down = true;
    push(e);

    e = {};
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = id;
    e.wheel.y = 2.0f;
    e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    push(e);

    e = {};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = id;
    e.key.key = SDLK_Z;
    e.key.mod = SDL_KMOD_LCTRL;
    e.key.down = true;
    push(e);

    char text[] = "h\xC3\xA9";
    e = {};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = id;
    e.text.text = text;
    push(e);

    // Another window's input is not ours.
    e = {};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = other->sdl_window_id_for_tests();
    e.key.key = SDLK_A;
    push(e);

    GIANT_CHECK(window->poll_events());
    const auto events = window->take_input_events();
    GIANT_CHECK(events.size() == 5);
    if (events.size() == 5) {
        using T = InputEvent::Type;
        GIANT_CHECK(events[0].type == T::mouse_move && events[0].x == 12.5f && events[0].y == 40.0f);
        GIANT_CHECK(events[1].type == T::mouse_button && events[1].button == MouseButton::right &&
                    events[1].down);
        GIANT_CHECK(events[2].type == T::mouse_wheel && events[2].y == -2.0f); // flipped
        GIANT_CHECK(events[3].type == T::key && events[3].key == Key::z && events[3].down &&
                    events[3].modifiers == modifier::ctrl);
        GIANT_CHECK(events[4].type == T::text && events[4].text == "h\xC3\xA9");
    }
    GIANT_CHECK(window->take_input_events().empty());
    // Input survives several polls until it is taken.
    e = {};
    e.type = SDL_EVENT_KEY_UP;
    e.key.windowID = id;
    e.key.key = SDLK_F5;
    push(e);
    GIANT_CHECK(window->poll_events());
    GIANT_CHECK(window->poll_events());
    const auto later = window->take_input_events();
    GIANT_CHECK(later.size() == 1 && later[0].key == Key::f5 && !later[0].down);
}

GIANT_TEST("platform: text input and cursors can be switched") {
    auto window = hidden_window(320, 200);
    GIANT_CHECK(window != nullptr);
    if (window == nullptr) {
        return;
    }
    window->set_text_input(true);
    window->set_text_input(false);
    window->set_cursor(Window::Cursor::text);
    window->set_cursor(Window::Cursor::hidden);
    window->set_cursor(Window::Cursor::arrow);
    GIANT_CHECK(window->poll_events());
}
