#include "giant/platform/window.h"

#include "giant/core/log.h"

#include <SDL3/SDL.h>

#include <string>

namespace giant::platform {

struct Window::Impl {
    SDL_Window* window = nullptr;
};

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
} // namespace

bool Window::wait_events(i32 timeout_ms) {
    SDL_WaitEventTimeout(nullptr, timeout_ms); // wakes on any event, leaves it queued
    return poll_events();
}

bool Window::poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
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
