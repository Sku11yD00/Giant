#include "giant/platform/window.h"

#include "giant/core/log.h"

#include <SDL3/SDL.h>

#include <array>
#include <string>

namespace giant::platform {

struct Window::Impl {
    SDL_Window* window = nullptr;
    // System cursors, created on first use and owned here.
    std::array<SDL_Cursor*, SDL_SYSTEM_CURSOR_COUNT> cursors{};
};

std::filesystem::path executable_dir() {
    const char* base = SDL_GetBasePath(); // owned by SDL, cached
    if (base == nullptr) {
        return std::filesystem::current_path();
    }
    const std::string utf8 = base;
    std::filesystem::path dir(std::u8string(utf8.begin(), utf8.end()));
    if (!dir.has_filename()) {
        dir = dir.parent_path(); // SDL ends the path with a separator
    }
    return dir;
}

Window::Window(ConstructKey, std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

std::unique_ptr<Window> Window::create(const WindowDesc& desc) {
    // Reference-counted by SDL, released in ~Window.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        log_error("platform", std::string("could not start video: ") + SDL_GetError());
        return nullptr;
    }

    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (desc.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    auto impl = std::make_unique<Impl>();
    impl->window = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width),
                                    static_cast<int>(desc.height), flags);
    if (impl->window == nullptr) {
        log_error("platform", std::string("could not create window: ") + SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return nullptr;
    }

    auto window = std::make_unique<Window>(ConstructKey{}, std::move(impl));
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window->m_impl->window, &w, &h);
    window->m_width = static_cast<u32>(w > 0 ? w : 0);
    window->m_height = static_cast<u32>(h > 0 ? h : 0);
    log_info("platform", "window created (" + std::to_string(w) + "x" + std::to_string(h) +
                             " pixels, video driver " + SDL_GetCurrentVideoDriver() + ")");
    return window;
}

Window::~Window() {
    for (SDL_Cursor* cursor : m_impl->cursors) {
        if (cursor != nullptr) {
            SDL_DestroyCursor(cursor);
        }
    }
    if (m_impl->window != nullptr) {
        SDL_DestroyWindow(m_impl->window);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (SDL_WasInit(0) == 0) {
        SDL_Quit(); // last user gone: release SDL's global state too
    }
}

void Window::request_size(u32 width, u32 height) {
    SDL_SetWindowSize(m_impl->window, static_cast<int>(width), static_cast<int>(height));
    SDL_SyncWindow(m_impl->window);
}

u32 Window::point_width() const {
    int w = 0;
    SDL_GetWindowSize(m_impl->window, &w, nullptr);
    return static_cast<u32>(w > 0 ? w : 0);
}

u32 Window::point_height() const {
    int h = 0;
    SDL_GetWindowSize(m_impl->window, nullptr, &h);
    return static_cast<u32>(h > 0 ? h : 0);
}

u32 Window::sdl_window_id_for_tests() const { return SDL_GetWindowID(m_impl->window); }

void Window::request_minimize() {
    SDL_MinimizeWindow(m_impl->window);
    SDL_SyncWindow(m_impl->window);
}

void Window::request_restore() {
    SDL_RestoreWindow(m_impl->window);
    SDL_SyncWindow(m_impl->window);
}

namespace {
bool is_ours(const SDL_Event& event, SDL_Window* window) {
    return event.window.windowID == SDL_GetWindowID(window);
}

bool is_ours(SDL_WindowID id, SDL_Window* window) { return id == SDL_GetWindowID(window); }

Key to_key(SDL_Keycode key) {
    if (key >= SDLK_A && key <= SDLK_Z) {
        return static_cast<Key>(static_cast<u16>(Key::a) + (key - SDLK_A));
    }
    if (key >= SDLK_0 && key <= SDLK_9) {
        return static_cast<Key>(static_cast<u16>(Key::n0) + (key - SDLK_0));
    }
    if (key >= SDLK_F1 && key <= SDLK_F12) {
        return static_cast<Key>(static_cast<u16>(Key::f1) + (key - SDLK_F1));
    }
    switch (key) {
    case SDLK_TAB: return Key::tab;
    case SDLK_LEFT: return Key::left;
    case SDLK_RIGHT: return Key::right;
    case SDLK_UP: return Key::up;
    case SDLK_DOWN: return Key::down;
    case SDLK_PAGEUP: return Key::page_up;
    case SDLK_PAGEDOWN: return Key::page_down;
    case SDLK_HOME: return Key::home;
    case SDLK_END: return Key::end;
    case SDLK_INSERT: return Key::insert;
    case SDLK_DELETE: return Key::del;
    case SDLK_BACKSPACE: return Key::backspace;
    case SDLK_SPACE: return Key::space;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return Key::enter;
    case SDLK_ESCAPE: return Key::escape;
    case SDLK_APOSTROPHE: return Key::apostrophe;
    case SDLK_COMMA: return Key::comma;
    case SDLK_MINUS: return Key::minus;
    case SDLK_PERIOD: return Key::period;
    case SDLK_SLASH: return Key::slash;
    case SDLK_SEMICOLON: return Key::semicolon;
    case SDLK_EQUALS: return Key::equals;
    case SDLK_LEFTBRACKET: return Key::left_bracket;
    case SDLK_BACKSLASH: return Key::backslash;
    case SDLK_RIGHTBRACKET: return Key::right_bracket;
    case SDLK_GRAVE: return Key::grave;
    case SDLK_LCTRL: return Key::left_ctrl;
    case SDLK_LSHIFT: return Key::left_shift;
    case SDLK_LALT: return Key::left_alt;
    case SDLK_LGUI: return Key::left_super;
    case SDLK_RCTRL: return Key::right_ctrl;
    case SDLK_RSHIFT: return Key::right_shift;
    case SDLK_RALT: return Key::right_alt;
    case SDLK_RGUI: return Key::right_super;
    default: return Key::unknown;
    }
}

u8 to_modifiers(SDL_Keymod mod) {
    u8 out = 0;
    if ((mod & SDL_KMOD_CTRL) != 0) out |= modifier::ctrl;
    if ((mod & SDL_KMOD_SHIFT) != 0) out |= modifier::shift;
    if ((mod & SDL_KMOD_ALT) != 0) out |= modifier::alt;
    if ((mod & SDL_KMOD_GUI) != 0) out |= modifier::super;
    return out;
}

bool to_button(Uint8 sdl_button, MouseButton& out) {
    switch (sdl_button) {
    case SDL_BUTTON_LEFT: out = MouseButton::left; return true;
    case SDL_BUTTON_RIGHT: out = MouseButton::right; return true;
    case SDL_BUTTON_MIDDLE: out = MouseButton::middle; return true;
    case SDL_BUTTON_X1: out = MouseButton::x1; return true;
    case SDL_BUTTON_X2: out = MouseButton::x2; return true;
    default: return false;
    }
}
} // namespace

void Window::set_text_input(bool enabled) {
    if (enabled == m_text_input) {
        return;
    }
    m_text_input = enabled;
    if (enabled) {
        SDL_StartTextInput(m_impl->window);
    } else {
        SDL_StopTextInput(m_impl->window);
    }
}

void Window::set_relative_mouse(bool enabled) {
    if (enabled == m_relative_mouse) {
        return;
    }
    if (SDL_SetWindowRelativeMouseMode(m_impl->window, enabled)) {
        m_relative_mouse = enabled;
    }
}

void Window::set_cursor(Cursor cursor) {
    if (cursor == m_cursor) {
        return;
    }
    m_cursor = cursor;
    if (cursor == Cursor::hidden) {
        SDL_HideCursor();
        return;
    }
    SDL_SystemCursor id = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (cursor) {
    case Cursor::text: id = SDL_SYSTEM_CURSOR_TEXT; break;
    case Cursor::resize_all: id = SDL_SYSTEM_CURSOR_MOVE; break;
    case Cursor::resize_ns: id = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
    case Cursor::resize_ew: id = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
    case Cursor::resize_nesw: id = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
    case Cursor::resize_nwse: id = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
    case Cursor::hand: id = SDL_SYSTEM_CURSOR_POINTER; break;
    case Cursor::not_allowed: id = SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
    default: break;
    }
    SDL_Cursor*& slot = m_impl->cursors[static_cast<usize>(id)];
    if (slot == nullptr) {
        slot = SDL_CreateSystemCursor(id);
    }
    if (slot != nullptr) {
        SDL_SetCursor(slot);
    }
    SDL_ShowCursor();
}

bool Window::wait_events(i32 timeout_ms) {
    SDL_WaitEventTimeout(nullptr, timeout_ms); // wakes on any event, leaves it queued
    return poll_events();
}

std::vector<InputEvent> Window::take_input_events() {
    std::vector<InputEvent> out;
    out.swap(m_input);
    return out;
}

bool Window::poll_events() {
    SDL_Window* const window = m_impl->window;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (is_ours(event.motion.windowID, window)) {
                InputEvent input;
                input.type = InputEvent::Type::mouse_move;
                input.x = event.motion.x;
                input.y = event.motion.y;
                input.dx = event.motion.xrel;
                input.dy = event.motion.yrel;
                m_input.push_back(std::move(input));
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (is_ours(event.button.windowID, window)) {
                InputEvent input;
                input.type = InputEvent::Type::mouse_button;
                if (to_button(event.button.button, input.button)) {
                    input.x = event.button.x;
                    input.y = event.button.y;
                    input.down = event.button.down;
                    m_input.push_back(std::move(input));
                }
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (is_ours(event.wheel.windowID, window)) {
                const float flip = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
                InputEvent input;
                input.type = InputEvent::Type::mouse_wheel;
                input.x = event.wheel.x * flip;
                input.y = event.wheel.y * flip;
                m_input.push_back(std::move(input));
            }
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (is_ours(event.key.windowID, window)) {
                InputEvent input;
                input.type = InputEvent::Type::key;
                input.key = to_key(event.key.key);
                input.modifiers = to_modifiers(event.key.mod);
                input.down = event.key.down;
                m_input.push_back(std::move(input));
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (is_ours(event.text.windowID, window) && event.text.text != nullptr) {
                InputEvent input;
                input.type = InputEvent::Type::text;
                input.text = event.text.text;
                m_input.push_back(std::move(input));
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (is_ours(event, window)) {
                InputEvent input;
                input.type = InputEvent::Type::focus;
                input.down = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
                m_input.push_back(std::move(input));
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (is_ours(event, window)) {
                InputEvent input;
                input.type = InputEvent::Type::mouse_left;
                m_input.push_back(std::move(input));
            }
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            if (is_ours(event, m_impl->window)) {
                m_minimized = true;
            }
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            if (is_ours(event, m_impl->window)) {
                m_minimized = false;
            }
            break;
        case SDL_EVENT_QUIT:
            m_close_requested = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (is_ours(event, m_impl->window)) {
                m_close_requested = true;
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (is_ours(event, m_impl->window)) {
                const u32 w = static_cast<u32>(event.window.data1 > 0 ? event.window.data1 : 0);
                const u32 h = static_cast<u32>(event.window.data2 > 0 ? event.window.data2 : 0);
                if (w != m_width || h != m_height) {
                    m_width = w;
                    m_height = h;
                    m_resized = true;
                }
            }
            break;
        default:
            break;
        }
    }
    return !m_close_requested;
}

bool Window::take_resized() {
    const bool resized = m_resized;
    m_resized = false;
    return resized;
}

NativeWindow Window::native() const {
    NativeWindow native;
#if defined(_WIN32)
    native.hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(m_impl->window),
                                         SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
    return native;
}

} // namespace giant::platform
