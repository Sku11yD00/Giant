// An operating-system window with its event loop. SDL3 underneath; nothing
// outside engine/platform sees SDL.
#pragma once

#include "giant/core/types.h"
#include "giant/platform/input.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace giant::platform {

// The folder holding the running program (for files shipped next to it).
std::filesystem::path executable_dir();

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
    // Keyboard and mouse input is collected for take_input_events().
    bool poll_events();

    // Input received since the last call, oldest first.
    std::vector<InputEvent> take_input_events();

    // Text input (on-screen keyboards, IME composition) is only delivered
    // while enabled. Turn it on while a text field has focus.
    void set_text_input(bool enabled);

    // Relative mouse (looking around in play): the cursor hidden and held
    // in the window, motion reported as mouse_move's dx, dy.
    void set_relative_mouse(bool enabled);
    bool relative_mouse() const { return m_relative_mouse; }

    enum class Cursor : u8 { arrow, text, resize_all, resize_ns, resize_ew, resize_nesw,
                             resize_nwse, hand, not_allowed, hidden };
    void set_cursor(Cursor cursor);

    // Size of the drawable area in pixels (not points), as last seen by
    // poll_events or at creation.
    u32 pixel_width() const { return m_width; }
    u32 pixel_height() const { return m_height; }

    // Size in screen points, the unit request_size takes. Equal to the pixel
    // size unless the screen is high-DPI.
    u32 point_width() const;
    u32 point_height() const;

    // True once after the drawable size changed; the swap chain should resize.
    bool take_resized();

    // Minimised (or zero-sized): skip rendering and call wait_events instead.
    bool minimized() const { return m_minimized || m_width == 0 || m_height == 0; }

    // Sleeps until an event arrives or the timeout passes, then processes
    // events like poll_events. Use while minimised so the editor idles.
    bool wait_events(i32 timeout_ms);

    NativeWindow native() const;

    // Asks the OS to resize (in points) or minimise the window. The change arrives as an
    // event in the next poll_events (used by tests and the editor's layouts).
    void request_size(u32 width, u32 height);
    void request_minimize();
    void request_restore();

    // The SDL window ID; tests use it to post synthetic OS events.
    u32 sdl_window_id_for_tests() const;

    struct Impl;
    struct ConstructKey {
    private:
        friend class Window;
        ConstructKey() = default;
    };
    // Use Window::create; the key keeps this constructor private in practice.
    Window(ConstructKey, std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> m_impl;
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_resized = false;
    bool m_minimized = false;
    bool m_close_requested = false;
    bool m_text_input = false;
    bool m_relative_mouse = false;
    Cursor m_cursor = Cursor::arrow;
    std::vector<InputEvent> m_input;
};

} // namespace giant::platform
