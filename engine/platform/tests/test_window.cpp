#include "giant/platform/window.h"

#include "giant_test.h"

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
