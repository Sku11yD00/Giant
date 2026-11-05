// Giant Editor: N0 shell. Opens a window, starts the graphics device and
// renders every frame. Panels, the viewport and the AI command layer build on
// this loop in the next steps.
//
//   giant_editor [--frames N] [--backend null|d3d12] [--software] [--hidden]
//                [--debug-layer] [--no-vsync]
#include "giant/core/log.h"
#include "giant/platform/window.h"
#include "giant/rhi/rhi.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

using namespace giant;

struct Options {
    u64 max_frames = 0; // 0 = run until the window closes
    rhi::Backend backend = rhi::default_backend();
    bool software = false;
    bool hidden = false;
    bool debug_layer = false;
    bool vsync = true;
};

bool parse_options(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            out.max_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--backend" && i + 1 < argc) {
            const std::string_view name = argv[++i];
            if (name == "null") {
                out.backend = rhi::Backend::null;
            } else if (name == "d3d12") {
                out.backend = rhi::Backend::d3d12;
            } else {
                log_error("editor", "unknown backend: " + std::string(name));
                return false;
            }
        } else if (arg == "--software") {
            out.software = true;
        } else if (arg == "--hidden") {
            out.hidden = true;
        } else if (arg == "--debug-layer") {
            out.debug_layer = true;
        } else if (arg == "--no-vsync") {
            out.vsync = false;
        } else {
            log_error("editor", "unknown option: " + std::string(arg));
            return false;
        }
    }
    return true;
}

int run(const Options& options) {
    platform::WindowDesc window_desc;
    window_desc.title = "Giant Editor";
    window_desc.hidden = options.hidden;
    auto window = platform::Window::create(window_desc);
    if (window == nullptr) {
        return 1;
    }

    rhi::DeviceDesc device_desc;
    device_desc.backend = options.backend;
    device_desc.software_adapter = options.software;
    device_desc.debug_layer = options.debug_layer;
    auto device = rhi::create_device(device_desc);
    if (device == nullptr) {
        return 1;
    }

    rhi::SwapChainDesc sc_desc;
    sc_desc.native_window = window->native().hwnd;
    sc_desc.width = window->pixel_width();
    sc_desc.height = window->pixel_height();
    sc_desc.vsync = options.vsync;
    auto swap_chain = device->create_swap_chain(sc_desc);
    if (swap_chain == nullptr) {
        return 1;
    }

    // A clear day-sky blue until the renderer draws something.
    const rhi::Color sky{0.42f, 0.62f, 0.86f, 1.0f};

    using clock = std::chrono::steady_clock;
    auto report_start = clock::now();
    u64 report_frames = 0;

    while (window->poll_events()) {
        if (window->take_resized()) {
            swap_chain->resize(window->pixel_width(), window->pixel_height());
        }
        device->begin_frame(*swap_chain);
        device->clear(*swap_chain, sky);
        device->end_frame(*swap_chain);

        ++report_frames;
        const auto now = clock::now();
        const double seconds = std::chrono::duration<double>(now - report_start).count();
        if (seconds >= 5.0) {
            const double ms = 1000.0 * seconds / static_cast<double>(report_frames);
            log_info("editor", "frame time " + std::to_string(ms) + " ms");
            report_start = now;
            report_frames = 0;
        }
        if (options.max_frames != 0 && device->frames_presented() >= options.max_frames) {
            break;
        }
    }

    device->wait_idle();
    log_info("editor", "rendered " + std::to_string(device->frames_presented()) + " frames");
    swap_chain.reset(); // swap chains go before their device
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }
    return run(options);
}
