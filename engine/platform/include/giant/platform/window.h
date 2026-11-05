// An operating-system window with its event loop. SDL3 underneath; nothing
// outside engine/platform sees SDL.
#pragma once

#include "giant/core/types.h"

#include <memory>
#include <string>

namespace giant::platform {

struct WindowDesc {
    std::string title = "Giant";
    u32 width = 1600;
    u32 height = 900;
    bool resizable = true;
    bool hidden = false; // for tests and headless runs
};

// The native handle a graphics backend needs to create a swap chain.
// On Windows `hwnd` is the HWND; it is null on platforms without one.
struct NativeWindow {
    void* hwnd = nullptr;
};

class Window {
public:
    // Returns null (and logs why) if the window cannot be created.
    static std::unique_ptr<Window> create(const WindowDesc& desc);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Processes pending events. Returns false once the user asked to close.
    bool poll_events();

    // Size of the drawable area in pixels (not points), as last seen by
    // poll_events or at creation.
    u32 pixel_width() const { return m_width; }
    u32 pixel_height() const { return m_height; }

    // True once after the drawable size changed; the swap chain should resize.
    bool take_resized();

    // Minimised (or zero-sized): skip rendering and call wait_events instead.
    bool minimized() const { return m_minimized || m_width == 0 || m_height == 0; }

    // Sleeps until an event arrives or the timeout passes, then processes
    // events like poll_events. Use while minimised so the editor idles.
    bool wait_events(i32 timeout_ms);

    NativeWindow native() const;

private:
    struct Impl;
    explicit Window(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_resized = false;
    bool m_minimized = false;
    bool m_close_requested = false;
};

} // namespace giant::platform
