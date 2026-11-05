// Giant Editor: N0 shell. Opens a window, starts the graphics device and
// renders every frame. Panels, the viewport and the AI command layer build on
// this loop in the next steps.
//
//   giant_editor [--frames N] [--backend null|d3d12] [--software] [--hidden]
//                [--debug-layer] [--require-validation] [--no-vsync]
//
// --require-validation fails the run if the GPU validation layer is not
// actually running (CI uses it so "no validation errors" means something).
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
    bool require_validation = false;
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
        } else if (arg == "--require-validation") {
            out.debug_layer = true;
            out.require_validation = true;
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
    if (options.require_validation && !device->validation_enabled()) {
        log_error("editor", "--require-validation: the GPU validation layer is not running");
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

    // A clear day-sky blue until the renderer draws something. Linear colour:
    // the back buffer view encodes it to sRGB for the screen.
    const rhi::Color sky{0.15f, 0.34f, 0.71f, 1.0f};

    using clock = std::chrono::steady_clock;
    auto report_start = clock::now();
    u64 report_frames = 0;
    // With --frames, give up if frames keep being skipped, so a broken swap
    // chain fails the run instead of hanging it.
    const u64 max_loops = options.max_frames == 0 ? 0 : options.max_frames + 1000;
    u64 loops = 0;

    while (window->poll_events()) {
        if (max_loops != 0 && ++loops > max_loops) {
            log_error("editor", "--frames: too many frames were skipped; giving up");
            return 1;
        }
        if (window->minimized()) {
            // Nothing to show: idle instead of spinning a CPU core.
            window->wait_events(100);
            continue;
        }
        if (window->take_resized()) {
            swap_chain->resize(window->pixel_width(), window->pixel_height());
        }
        if (!device->begin_frame(*swap_chain)) {
            if (device->device_lost()) {
                log_error("editor", "the GPU device was lost; closing");
                return 1;
            }
            // The swap chain is unusable after a failed resize: try again at
            // the current size, and wait a little so a lasting failure idles.
            window->wait_events(16);
            swap_chain->resize(window->pixel_width(), window->pixel_height());
            continue;
        }
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
    const u32 validation_errors = device->validation_errors();
    swap_chain.reset(); // swap chains go before their device
    if (validation_errors > 0) {
        log_error("editor", std::to_string(validation_errors) + " GPU validation errors");
        return 1;
    }
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
