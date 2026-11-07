// Giant Editor. Opens a window, starts the graphics device and renders the
// scene and panels every frame. Everything it can do is a command (editor_commands.h),
// and --mcp lets Claude Code or another MCP client run those commands.
//
//   giant_editor [--frames N] [--backend null|d3d12] [--software] [--hidden]
//                [--debug-layer] [--require-validation] [--no-vsync] [--mcp]
//                [--scripts path/to/Scripts.dll]
//
// --require-validation fails the run if the GPU validation layer is not
// actually running (CI uses it so "no validation errors" means something).
// --mcp speaks MCP over stdin/stdout (logs go to stderr); the editor closes
// when the client disconnects. --scripts loads C# scripts at start-up.
#include "editor_commands.h"
#include "mesh_library.h"
#include "scene_render.h"
#include "ui/imgui_layer.h"
#include "viewport_camera.h"
#include "far_ground.h"
#include "scatter_placer.h"
#include "physics_state.h"
#include "play_state.h"
#include "weather_state.h"
#include "terrain_brush_tool.h"
#include "terrain_meshes.h"
#include "world_streaming.h"
#include "ui/panels.h"

#include "giant/ai/mcp_server.h"
#include "giant/core/log.h"
#include "giant/core/log_history.h"
#include "giant/core/thread_time.h"
#include "giant/platform/line_reader.h"
#include "giant/platform/window.h"
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant/script/script_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h> // before shellapi.h, which needs its types
#include <shellapi.h>
// clang-format on
#endif

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
    bool mcp = false;
    std::string scripts; // C# script assembly to load at start-up
};

// The command line as UTF-8. On Windows, main's argv is in the ANSI code
// page, which garbles paths outside it; the wide command line does not.
std::vector<std::string> utf8_args(int argc, char** argv) {
    std::vector<std::string> args;
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide != nullptr) {
        for (int i = 0; i < count; ++i) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string text(bytes > 0 ? static_cast<usize>(bytes) : 0, '\0');
            if (bytes > 0) {
                WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), bytes, nullptr, nullptr);
                text.pop_back(); // the terminator
            }
            args.push_back(std::move(text));
        }
        LocalFree(wide);
        return args;
    }
#endif
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

bool parse_options(const std::vector<std::string>& argv, Options& out) {
    const usize argc = argv.size();
    for (usize i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            out.max_frames = std::strtoull(argv[++i].c_str(), nullptr, 10);
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
        } else if (arg == "--mcp") {
            out.mcp = true;
        } else if (arg == "--scripts" && i + 1 < argc) {
            out.scripts = argv[++i];
        } else {
            log_error("editor", "unknown option: " + std::string(arg));
            return false;
        }
    }
    return true;
}

// Answers every MCP message that arrived since the last frame. Returns false
// once the client can no longer receive replies. Messages after a quit
// request are not run.
//
// Replies are blocking writes: a client that stops reading entirely stalls
// the editor once the pipe fills. MCP clients always read, so this is
// accepted for now.
bool pump_mcp(platform::LineReader& reader, ai::McpServer& server, const bool& quit_requested) {
    for (const std::string& line : reader.take_lines()) {
        if (quit_requested) {
            break;
        }
        if (line.empty()) {
            continue;
        }
        if (auto reply = server.handle(line)) {
            std::cout << *reply << '\n';
        }
    }
    std::cout.flush();
    return static_cast<bool>(std::cout);
}

// Where Giant.Scripting.dll is: next to the editor in a download, one level
// up in a build tree.
std::filesystem::path find_managed_dir() {
    const std::filesystem::path exe = platform::executable_dir();
    std::error_code error;
    for (const auto& dir : {exe / "managed", exe.parent_path() / "managed"}) {
        if (std::filesystem::exists(dir / "Giant.Scripting.dll", error)) {
            return dir;
        }
    }
    return exe / "managed";
}

// C# scripts: starts .NET when first needed, applies load/unload requests
// from commands, and runs a frame of the scripts.
class ScriptRunner {
public:
    ScriptRunner(ai::CommandRegistry& commands, editor::EditorState& state)
        : m_commands(commands), m_state(state) {}

    void update(float delta_seconds) {
        if (m_state.scripts_unload_requested) {
            m_state.scripts_unload_requested = false;
            if (m_host != nullptr) {
                m_host->unload_scripts();
            }
            m_state.scripts_loaded.clear();
        }
        if (!m_state.scripts_to_load.empty()) {
            const std::string path = std::move(m_state.scripts_to_load);
            m_state.scripts_to_load.clear();
            if (start()) {
                const std::filesystem::path file(std::u8string(path.begin(), path.end()));
                m_state.scripts_loaded = m_host->load_scripts(file) ? path : std::string();
            }
        }
        if (m_host != nullptr && !m_state.scripts_loaded.empty()) {
            m_host->update(delta_seconds);
        }
        m_state.scripts_running = m_host != nullptr ? m_host->running_scripts() : 0;
    }

private:
    bool start() {
        if (m_host != nullptr) {
            return true;
        }
        if (!m_state.dotnet_available) {
            return false;
        }
        script::ScriptHostDesc desc;
        desc.managed_dir = find_managed_dir();
        // Scripts are game logic: their changes skip the editor's undo history.
        desc.commands = [this](std::string_view name, std::string_view args) {
            const auto parsed = ai::parse_json(args);
            if (!parsed) {
                return script::CommandReply{false, "arguments are not valid JSON"};
            }
            const ai::CommandResult result = m_commands.execute(name, *parsed, {0, false});
            return result.ok ? script::CommandReply{true, ai::to_json_text(result.value)}
                             : script::CommandReply{false, result.error};
        };
        m_host = script::ScriptHost::create(desc);
        m_state.dotnet_available = m_host != nullptr;
        return m_host != nullptr;
    }

    ai::CommandRegistry& m_commands;
    editor::EditorState& m_state;
    std::unique_ptr<script::ScriptHost> m_host;
};

int run(const Options& options) {
    // Before anything logs, so the AI can read start-up messages too.
    const LogHistory log_history;

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

    auto renderer = render::Renderer::create(*device);
    if (renderer == nullptr) {
        return 1;
    }

    editor::MeshLibrary meshes(renderer.get()); // after the renderer: released first
    editor::EditorState state;
    state.meshes = &meshes;
    editor::WorldStreaming world_streaming;
    state.world = &world_streaming;
    editor::TerrainMeshes terrain_meshes(renderer.get()); // after the renderer: released first
    state.terrain_meshes = &terrain_meshes;
    terrain_meshes.skirt_depth = 8.0f; // hides the gaps where the far ground's chunks meet the tiles
    editor::FarGround far_ground(renderer.get()); // after the renderer: released first
    state.far_ground = &far_ground;
    editor::ScatterPlacer scatter;
    state.scatter = &scatter;
    editor::WeatherState weather;
    state.weather = &weather;
    editor::PhysicsState physics;
    state.physics = &physics;
    editor::PlayState play(physics);
    state.play = &play;
    state.window = window.get();
    state.device = device.get();
    state.log = &log_history;
    state.probe_voxels = [&renderer](const DVec3& p) { return renderer->probe_voxels(p); };
    ai::CommandRegistry commands;
    editor::register_editor_commands(commands, state);

    std::unique_ptr<platform::LineReader> mcp_reader;
    std::unique_ptr<ai::McpServer> mcp_server;
    if (options.mcp) {
#if defined(_WIN32)
        // Messages end in \n only; text mode would turn that into \r\n.
        _setmode(_fileno(stdout), _O_BINARY);
#else
        // A client that closes its end must not kill the editor mid-write;
        // the failed write is noticed below and the editor closes cleanly.
        std::signal(SIGPIPE, SIG_IGN);
#endif
        mcp_reader = platform::LineReader::from_stdin();
        mcp_server = std::make_unique<ai::McpServer>(
            commands, ai::McpServerInfo{"giant-editor", "0.1.0", editor::mcp_instructions()});
        log_info("editor", "MCP server listening on stdin/stdout");
    }
    // Hidden runs (tests, CI) keep the panel layout in memory only.
    auto ui = editor::ImGuiLayer::create(*window, *device,
                                         options.hidden ? nullptr : "giant_editor_layout.ini");
    if (ui == nullptr) {
        return 1;
    }
    // Before the scripts: their OnStop may run commands (a raycast reads it).
    editor::SceneFrame scene_frame; // reused every frame
    physics.take_frame_changes = [&scene_frame] { return scene_frame.take_for_followers(); };
    editor::EditorPanels panels(commands, state, log_history);
    // After the registry, so scripts' OnStop can still run commands.
    ScriptRunner scripts(commands, state);
    state.scripts_to_load = options.scripts;
    editor::TerrainBrushTool terrain_brush;
    // Records the scene and the panels into `target` (an sRGB image).
    // Where the main thread's time goes each frame (get_engine_info's
    // phases_since): mark(p) gives p the time since the last mark.
    using clock = std::chrono::steady_clock;
    // A screenshot (taken by a command) is the command's time.
    editor::FramePhaseMs phase_ms{};
    auto phase_mark = clock::now();
    bool in_screenshot = false;
    const auto mark = [&](editor::FramePhase phase) {
        const auto now = clock::now();
        const editor::FramePhase to = in_screenshot ? editor::FramePhase::mcp : phase;
        phase_ms[static_cast<usize>(to)] += std::chrono::duration<float, std::milli>(now - phase_mark).count();
        phase_mark = now;
    };
    const auto record_frame = [&](rhi::CommandList& cmd, rhi::TextureHandle target) {
        mark(editor::FramePhase::other);
        terrain_meshes.update(state.scene.terrain);
        mark(editor::FramePhase::terrain);
        far_ground.update(state.camera.position, world_streaming, state.scene_generation, terrain_meshes,
                          state.scene.terrain);
        mark(editor::FramePhase::far_ground);
        scatter.camera = state.camera.position;
        scatter.update(state.scene, terrain_meshes.last_changes());
        mark(editor::FramePhase::scatter);
        const std::vector<world::EntityId> redraw = physics.take_redraw();
        editor::scene_frame(state.scene, *renderer, meshes, scene_frame, &terrain_meshes, &far_ground, &scatter,
                            &physics.poses(), redraw);
        mark(editor::FramePhase::scene);
        render::FrameInput input;
        input.view = editor::to_view(state.camera);
        input.lighting = editor::weather_lighting(state.scene.lighting, weather.shown());
        input.sky = editor::weather_sky(state.scene.lighting, weather.shown(), weather.cloud_offset(), weather.ground(),
                                        weather.time());
        input.settings = state.render_settings;
        input.instances = scene_frame.instances;
        input.cells = scene_frame.cells;
        input.materials = scene_frame.materials;
        input.decals = scene_frame.decals;
        renderer->render(cmd, target, input);
        mark(editor::FramePhase::render);
        if (!in_screenshot) {
            for (usize i = 0; i < render::k_cpu_section_names.size(); ++i) {
                phase_ms[static_cast<usize>(editor::FramePhase::count) + i] += renderer->last_cpu_ms()[i];
            }
        }
        state.scene_stats = renderer->last_stats();
        cmd.begin_timing("UI");
        cmd.draw_ui(target, ui->frame());
        cmd.end_timing();
        mark(editor::FramePhase::ui_draw);
    };
    state.render_screenshot = [&]() -> std::vector<u8> {
        // The same frame, offscreen: what the screen shows, sRGB-encoded.
        rhi::TextureDesc desc;
        desc.width = window->pixel_width();
        desc.height = window->pixel_height();
        desc.format = rhi::Format::rgba8_srgb;
        desc.usage = rhi::TextureUsage::render_target;
        desc.name = "screenshot";
        const rhi::TextureHandle target = device->create_texture(desc);
        if (!target.valid()) {
            return {};
        }
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        if (cmd == nullptr) {
            device->destroy(target);
            return {};
        }
        in_screenshot = true;
        record_frame(*cmd, target);
        in_screenshot = false;
        device->end_frame();
        std::vector<u8> pixels = device->read_texture(target);
        device->destroy(target);
        return pixels;
    };
    editor::ViewportCamera viewport_camera;
    bool was_playing = false;
    editor::FrameInfo frame_info;
    auto last_frame = std::chrono::steady_clock::now();

    // The null backend has no display to wait for, so pace it like vsync
    // instead of spinning a CPU core.
    const bool pace_null = device->backend() == rhi::Backend::null && options.vsync;

    auto report_start = clock::now();
    u64 report_frames = 0;
    // With --frames, give up after this many frames in a row could not be
    // drawn, so a broken swap chain fails the run instead of hanging it.
    // Minimised time does not count.
    constexpr u64 k_max_skipped_frames = 1000;
    u64 skipped_in_a_row = 0;

    double cpu_mark = thread_cpu_ms(); // (on Windows its first call calibrates: 5 ms)
    phase_mark = clock::now();         // setup is no frame's
    last_frame = phase_mark;
    while (window->poll_events() && !state.quit_requested) {
        mark(editor::FramePhase::events);
        if (mcp_reader != nullptr) {
            if (!pump_mcp(*mcp_reader, *mcp_server, state.quit_requested) ||
                mcp_reader->finished()) {
                log_info("editor", "MCP client disconnected; closing");
                break;
            }
            if (state.quit_requested) {
                break;
            }
        }
        mark(editor::FramePhase::mcp);
        const auto input = window->take_input_events();
        if (window->minimized()) {
            // Nothing to show: idle instead of spinning a CPU core.
            window->wait_events(100);
            last_frame = std::chrono::steady_clock::now();
            phase_ms = {}; // minimised time is no frame's
            phase_mark = last_frame;
            cpu_mark = thread_cpu_ms();
            continue;
        }
        const auto frame_start = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(frame_start - last_frame).count();
        last_frame = frame_start;
        frame_info.frame_ms = 1000.0f * delta;
        // The phases since the last frame began: the same span as delta.
        mark(editor::FramePhase::other);
        const double cpu_now = thread_cpu_ms();
        state.frame_times.add(frame_info.frame_ms, phase_ms, static_cast<float>(cpu_now - cpu_mark));
        cpu_mark = cpu_now;
        phase_ms = {};

        scripts.update(delta);
        mark(editor::FramePhase::scripts);

        // Playing: the mouse and keys are the player's (the panels get
        // only releases, so nothing they hold sticks).
        if (play.playing() != was_playing) {
            was_playing = play.playing();
            ui->set_captured(was_playing);
            viewport_camera.reset(); // keys held when play started or stopped never arrive
        }
        // Build the UI first: what it changes shows in this frame.
        ui->begin_frame(delta, input);
        if (play.playing()) {
            // The player's keys and mouse, not the editor camera's.
            play.handle_input(input, ui->wants_keyboard());
        } else {
            viewport_camera.handle_input(input, ui->wants_mouse());
            terrain_brush.update(input, ui->wants_mouse(), static_cast<float>(window->point_width()),
                                 static_cast<float>(window->point_height()), delta, state, commands);
            viewport_camera.update(state.camera, delta);
        }
        panels.draw(frame_info);
        ui->end_frame();
        mark(editor::FramePhase::ui);
        // The player's wanted motion (or play stopped: Esc, a new scene).
        const bool playing = play.before_step(state.scene, state.camera, state.scene_generation, delta);
        window->set_relative_mouse(playing); // mouse look, the cursor held in the window
        weather.update(state.scene.weather, delta, state.camera.position, state.scene_generation); // after the panels' changes
        // What the last frame took from the scene; dynamic entities move
        // while simulating (drawn there this frame).
        mark(editor::FramePhase::weather);
        physics.update(state.scene, scene_frame.take_for_followers(), delta);
        if (playing) play.after_step(state.camera, delta); // the camera at its eyes
        mark(editor::FramePhase::physics);
        if (window->take_resized()) {
            swap_chain->resize(window->pixel_width(), window->pixel_height());
        }
        rhi::CommandList* cmd = device->begin_frame(swap_chain.get());
        mark(editor::FramePhase::begin_frame);
        if (cmd == nullptr) {
            if (device->device_lost()) {
                log_error("editor", "the GPU device was lost; closing");
                return 1;
            }
            if (options.max_frames != 0 && ++skipped_in_a_row > k_max_skipped_frames) {
                log_error("editor", "--frames: frames keep failing to draw; giving up");
                return 1;
            }
            // The swap chain is unusable after a failed resize: try again at
            // the current size, and wait a little so a lasting failure idles.
            window->wait_events(16);
            swap_chain->resize(window->pixel_width(), window->pixel_height());
            mark(editor::FramePhase::tail);
            continue;
        }
        record_frame(*cmd, swap_chain->current_texture());
        device->end_frame();
        mark(editor::FramePhase::end_frame);
        // After the frame's cells were built from the scene: loads and
        // unloads world cells (what joins the scene draws next frame).
        world_streaming.update(state.scene, state.scene_generation, state.camera.position, scene_frame);
        mark(editor::FramePhase::streaming);
        if (state.selected != 0 && state.scene.find(state.selected) == nullptr) {
            state.selected = 0;
        }
        frame_info.ui_draw_calls = device->last_frame_stats().ui_draws;
        skipped_in_a_row = 0;
        if (pace_null) {
            window->wait_events(16);
        }

        ++report_frames;
        const auto now = clock::now();
        const double seconds = std::chrono::duration<double>(now - report_start).count();
        if (seconds >= 5.0) {
            const double ms = 1000.0 * seconds / static_cast<double>(report_frames);
            log_info("editor", "frame time " + std::to_string(ms) + " ms");
            device->validation_errors(); // logs new ones now and empties the queue
            report_start = now;
            report_frames = 0;
        }
        if (options.max_frames != 0 && device->frames_completed() >= options.max_frames) {
            break;
        }
        mark(editor::FramePhase::tail);
    }

    device->wait_idle();
    log_info("editor", "rendered " + std::to_string(device->frames_completed()) + " frames");
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
    if (!parse_options(utf8_args(argc, argv), options)) {
        return 2;
    }
    return run(options);
}
