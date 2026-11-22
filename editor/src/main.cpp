// Giant Editor. Opens a window, starts the graphics device and renders the
// scene and panels every frame. Everything it can do is a command (editor_commands.h),
// and --mcp lets Claude Code or another MCP client run those commands.
//
//   giant_editor [--frames N] [--backend null|d3d12] [--software] [--hidden]
//                [--debug-layer] [--require-validation] [--no-vsync] [--mcp]
//                [--scripts path/to/Scripts.dll] [--allow-mods-scripts-by-command]
//                [--allow-scripts-by-command] [--chat-asks]
//                [--game path/to/world [--player-settings file]]
//
// With game.json beside it (a folder made by package_game) the program is
// that game without being told: see take_packaged_game.
// --require-validation fails the run if the GPU validation layer is not
// actually running (CI uses it so "no validation errors" means something).
// --mcp speaks MCP over stdin/stdout (logs go to stderr); the editor closes
// when the client disconnects. --scripts loads C# scripts at start-up.
// --allow-mods-scripts-by-command is for automated runs: enable_mod then
// allows a mod's scripts itself, where the editor otherwise asks the person
// in its window (a script is a program with their rights; docs/mods.md).
// --allow-scripts-by-command is the same for load_scripts, which otherwise
// loads a file only once the person allowed it in the window
// (docs/scripting.md). --chat-asks has Claude's changes from the chat wait
// for the person's answer even with --hidden, where no one can give it
// (tests of the waiting; with a window it is the Chat panel's own box).
#include "chat_bridge.h"
#include "editor_commands.h"
#include "mesh_library.h"
#include "texture_library.h"
#include "scene_render.h"
#include "effects_state.h"
#include "event_effects.h"
#include "weather_effects.h"
#include "ui/imgui_layer.h"
#include "viewport_camera.h"
#include "far_ground.h"
#include "scatter_placer.h"
#include "animation_state.h"
#include "cloth_state.h"
#include "hair_state.h"
#include "pose_bakes.h"
#include "people_list.h"
#include "people_routines.h"
#include "people_senses.h"
#include "people_figures.h"
#include "people_state.h"
#include "people_travel.h"
#include "swing_state.h"
#include "nav_state.h"
#include "ragdoll_state.h"
#include "walker_state.h"
#include "audio_state.h"
#include "hit_targets.h"
#include "physics_state.h"
#include "play_state.h"
#include "clock_state.h"
#include "water_meshes.h"
#include "giant/world/water.h"
#include "weather_state.h"
#include "terrain_brush_tool.h"
#include "terrain_meshes.h"
#include "world_streaming.h"
#include "game_menus.h"
#include "game_package.h"
#include "game_saves.h"
#include "game_ui.h"
#include "ui/panels.h"

#include "giant/ai/mcp_server.h"
#include "giant/core/log.h"
#include "giant/core/log_history.h"
#include "giant/core/text.h"
#include "giant/core/text_files.h"
#include "giant/core/thread_time.h"
#include "giant/platform/line_reader.h"
#include "giant/platform/window.h"
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant/world/weather.h"
#include "giant/script/script_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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
    bool allow_mods_scripts_by_command = false; // automated runs: enable_mod allows a mod's scripts without asking the person
    bool allow_scripts_by_command = false;      // automated runs: load_scripts loads a file without asking the person
    bool chat_asks = false;                     // Claude's changes from the chat wait for the person even with --hidden
    std::string game_name;       // a packaged game's (game.json beside the program): its window's title, its settings' folder
    std::string player_settings; // a game: the file its settings are kept in (empty: in the person's settings folder)
    std::string game; // a world folder to run as a game (its menus lead, no editor panels); empty: the editor
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
        } else if (arg == "--allow-mods-scripts-by-command") {
            out.allow_mods_scripts_by_command = true;
        } else if (arg == "--allow-scripts-by-command") {
            out.allow_scripts_by_command = true;
        } else if (arg == "--chat-asks") {
            out.chat_asks = true;
        } else if (arg == "--scripts" && i + 1 < argc) {
            out.scripts = argv[++i];
        } else if (arg == "--player-settings" && i + 1 < argc) {
            out.player_settings = argv[++i];
            if (out.player_settings.empty()) {
                log_error("editor", "--player-settings needs a file");
                return false;
            }
        } else if (arg == "--game" && i + 1 < argc) {
            out.game = argv[++i];
            if (out.game.empty()) {
                log_error("editor", "--game needs a world folder");
                return false;
            }
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

// The program's own file (package_game copies it under the game's name). Empty when it cannot be
// told (anywhere but Windows and Linux).
std::filesystem::path program_file() {
#if defined(_WIN32)
    std::wstring name(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (length == 0 || length >= name.size()) return {};
    name.resize(length);
    return std::filesystem::path(name);
#else
    std::error_code error;
    const std::filesystem::path file = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : file;
#endif
}

// A packaged game (game_package.h): a game's game.json beside the program makes the program that
// game, unless it was started as another (--game). Its working folder becomes its own: the world's
// paths are from there, wherever it was started from (paths on the command line are made whole
// first). Its scripts are loaded unasked, as --scripts': whoever runs a game runs what it is made
// of. Another program's game.json (no "giant_game": 1) is let be. False (logged): it is a game's
// and cannot be run.
bool take_packaged_game(Options& options) {
    if (!options.game.empty()) return true;
    const std::filesystem::path folder = platform::executable_dir();
    std::error_code error;
    const std::filesystem::path file = folder / std::string(editor::k_game_file);
    if (!std::filesystem::is_regular_file(file, error)) return true;
    const std::optional<std::string> text = read_text_file(file, 65536);
    // (Some other program's game.json, or none that can be read: the editor, as ever.)
    if (!text || !editor::is_game_info(*text)) return true;
    std::string why;
    const std::optional<editor::GameInfo> info = editor::game_info_from_text(*text, why);
    if (!info) {
        log_error("game", "game.json beside the program: " + why);
        return false;
    }
    // (Paths given on the command line are from where it was started, not from the game's folder.)
    for (std::string* given : {&options.player_settings, &options.scripts}) {
        if (given->empty()) continue;
        const std::filesystem::path whole = std::filesystem::absolute(path_from_utf8(*given), error);
        if (!error) *given = path_to_utf8(whole);
    }
    std::filesystem::current_path(folder, error);
    if (error) {
        log_error("game", "the game's folder could not be made the working folder: " + error.message());
        return false;
    }
    options.game = info->world;
    options.game_name = info->name;
    if (options.scripts.empty()) options.scripts = info->scripts;
    log_info("game", info->name + " " + info->version);
    return true;
}

// The editor's settings on this machine, the person's own (where the chat's key is too): %APPDATA%\Giant,
// or $XDG_CONFIG_HOME/Giant, or ~/.config/Giant. Empty when none of those is set.
std::filesystem::path settings_folder() {
#if defined(_WIN32)
    const std::filesystem::path base = environment_path("APPDATA");
#else
    std::filesystem::path base = environment_path("XDG_CONFIG_HOME");
    if (base.empty() || !base.is_absolute()) {
        const std::filesystem::path home = environment_path("HOME");
        base = home.empty() ? std::filesystem::path() : home / ".config";
    }
#endif
    return base.empty() || !base.is_absolute() ? std::filesystem::path() : base / "Giant";
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
                if (m_state.mods_scripts_loaded.empty()) {
                    // (Scripts hear of what happens from when they are loaded; beside mods' scripts, what those have yet to hear too.)
                    m_events_told = m_state.routines != nullptr ? m_state.routines->changes() : 0;
                    m_events_missed = false;
                }
            }
        }
        update_mods_scripts();
        if (m_host != nullptr && (!m_state.scripts_loaded.empty() || !m_state.mods_scripts_loaded.empty())) {
            tell_person_events();
            m_host->update(delta_seconds);
        }
        m_state.scripts_running = m_host != nullptr ? m_host->running_scripts() : 0;
    }

    // .NET, started when first needed (by scripts, or the chat). Null when it cannot be.
    script::ScriptHost* host() { return start() ? m_host.get() : nullptr; }

private:
    // The open world's mods' scripts (EditorState::mods_scripts, found by open_world): loaded beside the
    // world's own when the world is opened, unloaded when it is closed or opened again.
    void update_mods_scripts() {
        const bool active = m_state.world != nullptr && m_state.world->active(m_state.scene_generation);
        const editor::ModsScriptsKey want =
            editor::mods_scripts_key(m_state.mods_scripts, active, m_state.world != nullptr ? m_state.world->session() : 0);
        if (want == m_mods_loaded && !m_mods_partly) {
            return;
        }
        m_mods_loaded = want;
        m_mods_partly = false;
        if (m_host != nullptr) {
            m_host->unload_added_scripts(); // (whatever was added: also one that did not start)
        }
        m_state.mods_scripts_loaded.clear();
        if (want == editor::ModsScriptsKey{}) {
            return;
        }
        if (!start()) {
            log_error("editor", "the mods' scripts were not loaded: .NET could not be started");
            return;
        }
        // (A copy: a script may run commands as it starts, open_world among them, which finds the scripts anew.)
        const std::vector<editor::ModsScripts::Run> run = m_state.mods_scripts.run;
        for (const editor::ModsScripts::Run& one : run) {
            if (m_host->add_scripts(path_from_utf8(one.path))) {
                m_state.mods_scripts_loaded.push_back(one.mod);
            } else {
                log_error("editor", "the scripts of the mod \"" + one.mod + "\" could not be loaded (see above)");
            }
            const bool still = m_state.world != nullptr && m_state.world->active(m_state.scene_generation);
            if (!(editor::mods_scripts_key(m_state.mods_scripts, still, m_state.world != nullptr ? m_state.world->session() : 0) == want)) {
                m_mods_partly = true; // (one of them changed what runs: the next frame loads what does then, anew)
                break;
            }
        }
        if (m_state.scripts_loaded.empty()) {
            // (Scripts hear of what happens from when they are loaded; beside the world's own, from when those were.)
            m_events_told = m_state.routines != nullptr ? m_state.routines->changes() : 0;
            m_events_missed = false;
        }
    }

    // What happened to people's behaviours since the scripts were last told, a share a frame
    // (many at once are told over the frames after: never more script time in one).
    void tell_person_events() {
        if (m_state.routines == nullptr || m_events_told >= m_state.routines->changes()) {
            return;
        }
        // (More waiting than are kept; not those of another scene, which are not told on purpose.)
        const u64 made = m_state.routines->changes();
        if (made - m_events_told > editor::PeopleRoutines::k_changes_kept && !m_events_missed) {
            m_events_missed = true; // (said once a load)
            log_warning("editor", "more happened to people than scripts are told of a frame: the oldest events were not told");
        }
        m_changes.clear();
        m_events.clear();
        m_state.routines->changes_since(m_state.scene, m_events_told, k_events_a_frame, m_changes);
        for (const editor::PeopleRoutines::Change& c : m_changes) {
            m_events.push_back({c.person, c.behaviour, c.from, c.to, c.seconds});
        }
        m_host->person_events(m_events);
    }

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
    static constexpr usize k_events_a_frame = 256;
    u64 m_events_told = 0;
    bool m_events_missed = false;
    editor::ModsScriptsKey m_mods_loaded; // which opening's mods' scripts are loaded ({}: none)
    bool m_mods_partly = false;           // loading them was broken off: loaded anew next frame, whatever the key
    std::vector<editor::PeopleRoutines::Change> m_changes; // (kept: nothing is allocated a frame)
    std::vector<script::PersonEvent> m_events;
};

int run(const Options& options) {
    // Before anything logs, so the AI can read start-up messages too.
    const LogHistory log_history;

    platform::WindowDesc window_desc;
    window_desc.title = options.game.empty() ? "Giant Editor" : options.game_name.empty() ? "Giant" : options.game_name;
    window_desc.hidden = options.hidden;
    // A game: the person's settings as it last kept them, read before the window is made (its mode and
    // size, vsync) and handed to the menus once they are there.
    editor::GameMenus::Settings kept;
    std::string kept_in;
    if (!options.game.empty()) {
        try {
            const std::filesystem::path folder = settings_folder();
            kept_in = !options.player_settings.empty() ? options.player_settings
                      : folder.empty()                  ? std::string()
                      : options.game_name.empty()       ? path_to_utf8(folder / "game_settings.json")
                                                        // (A packaged game's own: not another game's.)
                                                        : path_to_utf8(folder / "games" / path_from_utf8(options.game_name) /
                                                                       "game_settings.json");
        } catch (const std::exception&) {
            // (A folder whose name is not text: the settings last while the program runs.)
        }
        if (!kept_in.empty()) {
            const std::optional<std::string> text = read_text_file(path_from_utf8(kept_in), 65536);
            std::error_code there;
            if (!text && std::filesystem::exists(path_from_utf8(kept_in), there))
                log_warning("game", "the settings in " + kept_in + " could not be read (or are over 64 KB): the defaults stand");
            if (text) {
                std::vector<std::string> problems;
                kept = editor::settings_from_text(*text, problems);
                for (const std::string& problem : problems) log_warning("game", "the settings in " + kept_in + ": " + problem);
                if (!problems.empty()) {
                    // What a person wrote by hand is not lost to the next write: it is set aside.
                    std::error_code ec;
                    std::filesystem::copy_file(path_from_utf8(kept_in), path_from_utf8(kept_in + ".bad"),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                    log_warning("game", ec ? "they could not be set aside as " + kept_in + ".bad" : "they are set aside as " + kept_in + ".bad");
                }
            }
        }
        window_desc.fullscreen = kept.fullscreen;
        if (kept.window_width != 0) {
            window_desc.width = kept.window_width;
            window_desc.height = kept.window_height;
        }
    }
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
    sc_desc.vsync = options.vsync && kept.vsync; // (--no-vsync is the run's; the setting the person's)
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
    state.vsync_now = sc_desc.vsync;
    try {
        state.program = path_to_utf8(program_file());
        state.managed_folder = path_to_utf8(find_managed_dir());
    } catch (const std::exception&) {
        // (A folder whose name is not text: package_game says the program is not there.)
    }
    state.meshes = &meshes;
    // Cooked material textures are kept beside the working folder's assets
    // between sessions (cooking a 4K map takes seconds; reading it back, not).
    editor::TextureLibrary textures(renderer.get(), "giant_cache/textures"); // after the renderer: released first
    state.textures = &textures;
    editor::WorldStreaming world_streaming;
    state.world = &world_streaming;
    editor::TerrainMeshes terrain_meshes(renderer.get()); // after the renderer: released first
    state.terrain_meshes = &terrain_meshes;
    editor::TerrainPaintSync terrain_paint; // the tiles' paint, handed to the renderer
    terrain_meshes.skirt_depth = 8.0f; // hides the gaps where the far ground's chunks meet the tiles
    editor::FarGround far_ground(renderer.get()); // after the renderer: released first
    state.far_ground = &far_ground;
    editor::ScatterPlacer scatter;
    state.scatter = &scatter;
    editor::WeatherState weather;
    state.weather = &weather;
    editor::ClockState time_of_day; // the time of day shown (play moves it on)
    state.clock = &time_of_day;
    editor::HitTargets hit_targets; // before the physics, which reads its shapes
    state.hits = &hit_targets;
    editor::PhysicsState physics;
    state.physics = &physics;
    physics.set_hit_targets(&hit_targets);
    editor::PlayState play(physics);
    state.play = &play;
    editor::AnimationState animation(renderer.get()); // after the renderer: its copies leave first
    state.animation = &animation;
    editor::ClothState cloth(renderer.get()); // likewise
    state.cloth = &cloth;
    editor::HairState hair(renderer.get(), &meshes); // likewise: its grooms leave first
    state.hair = &hair;
    editor::PoseBakes pose_bakes(renderer.get(), &meshes); // its meshes leave the renderer before it goes
    state.poses = &pose_bakes;
    editor::SwingState swing(renderer.get(), &meshes);
    state.swing = &swing;
    editor::EffectsState effects; // entities' effects and spawned ones, born as particles
    state.effects = &effects;
    editor::WeatherEffects weather_effects; // rain splashing, a storm's leaves and grit, near the camera
    state.weather_effects = &weather_effects;
    editor::NavState nav(renderer.get(), &meshes, &scatter); // after what it reads
    state.nav = &nav;
    editor::WalkerState walkers(physics, nav); // after both: its characters and searches go first
    state.walkers = &walkers;
    editor::PeopleTravel travel; // where the people are while the world runs, and their trips
    state.travel = &travel;
    editor::PeopleRoutines routines; // who is sent where as the day goes on
    state.routines = &routines;
    editor::PeopleSenses senses; // what the people hear, and how alarmed each is
    state.senses = &senses;
    editor::PeopleState people; // the people near the camera, as entities
    people.travel = &travel;
    people.walkers = &walkers;
    people.routines = &routines; // (what each does shows as its look's clip for it)
    people.animation = &animation;
    people.meshes = &meshes;
    state.people = &people;
    editor::PeopleFigures figures; // those farther off, drawn as baked poses
    figures.travel = &travel;
    figures.routines = &routines;
    figures.bodies = &people;
    figures.poses = &pose_bakes;
    figures.meshes = &meshes;
    state.figures = &figures;
    editor::connect_walkers(state);
    editor::RagdollState ragdolls(physics, animation, &walkers); // after them: its ragdolls leave the physics first
    state.ragdolls = &ragdolls;
    people.ragdolls = &ragdolls;
    editor::connect_ragdolls(state);
    editor::AudioState audio; // the speakers when shown (hidden runs: tests, gates, stay silent)
    editor::WaterHearing water_hearing; // the water heard round the camera, a few ways a frame
    if (!options.hidden) audio.open_device();
    state.audio = &audio;
    editor::GameSaves saves; // beside the open world; its job waited for before what it reads goes
    state.saves = &saves;
    editor::connect_blows(state); // shots and blasts reach the swings and the cloth
    editor::connect_sounds(state); // after connect_blows: hits knock and are heard
    editor::connect_senses(state); // after both: the people hear them too
    editor::connect_event_effects(state); // after both: hits and splashes throw up particles too
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
                                         options.hidden || !options.game.empty() ? nullptr : "giant_editor_layout.ini"); // (a game has no panels to lay out)
    if (ui == nullptr) {
        return 1;
    }
    editor::GameUi game_ui(device.get()); // the game's own UI: its font made and uploaded now, once
    state.game_ui = &game_ui;
    editor::GameMenus menus(state); // the pause, saves and settings menus, on the game UI
    state.menus = &menus;
    if (!options.game.empty()) {
        menus.set_settings(kept);
        menus.keep_in(kept_in);
    }
    play.escape_stops = false; // Esc in play opens the pause menu (its Quit stops play)
    std::vector<rhi::UiDrawList> ui_lists; // the game's UI, then the panels
    // Before the scripts: their OnStop may run commands (a raycast reads it).
    editor::SceneFrame scene_frame; // reused every frame
    scene_frame.textures = &textures;
    physics.take_frame_changes = [&scene_frame] { return scene_frame.take_for_followers(); };
    editor::EditorPanels panels(commands, state, log_history);
    // After the registry, so scripts' OnStop can still run commands.
    ScriptRunner scripts(commands, state);
    // The chat with Claude: its C# side starts with the first message.
    // (Claude asks before it changes anything, until the person says otherwise in the Chat panel; where
    // there is no window no one could answer, and whoever drives the editor has every command anyway.)
    // (A game draws no window of the editor's to ask in: as with none at all.)
    const bool nobody_to_ask = options.hidden || !options.game.empty();
    state.chat_gate.asks = !nobody_to_ask || options.chat_asks;
    editor::ChatBridge chat(commands, state.chat_gate, [&scripts] { return scripts.host(); }, find_managed_dir());
    state.chat = &chat;
    // (Gone from the state before it is destroyed: scripts' OnStop, which runs after, may ask for it.)
    struct ChatGone {
        editor::EditorState& state;
        ~ChatGone() { state.chat = nullptr; }
    } chat_gone{state};
    state.scripts_to_load = options.scripts;
    state.scripts_consent = options.allow_scripts_by_command ? editor::ScriptsConsent::by_command
                            : nobody_to_ask                  ? editor::ScriptsConsent::none
                                                             : editor::ScriptsConsent::ask;
    if (!options.scripts.empty()) {
        // (The person's own choice, on the command line: theirs to load again without being asked.)
        if (std::string key = editor::script_file_key(path_from_utf8(options.scripts)); !key.empty()) {
            state.scripts_allowed.push_back(std::move(key));
        }
    }
    state.mods_scripts_consent = options.allow_mods_scripts_by_command ? editor::ScriptsConsent::by_command
                                 : nobody_to_ask                           ? editor::ScriptsConsent::none
                                                                           : editor::ScriptsConsent::ask;
    if (const std::filesystem::path settings = settings_folder(); !settings.empty()) {
        try {
            state.mods_scripts_allowed_file = path_to_utf8(settings / "mods_scripts_allowed.json");
        } catch (const std::exception&) {
            // (A folder whose name is not text: no mod's scripts can be allowed.)
        }
    }
    editor::TerrainBrushTool terrain_brush;
    // Records the scene and the panels into `target` (an sRGB image).
    // Where the main thread's time goes each frame (get_engine_info's
    // phases_since): mark(p) gives p the time since the last mark.
    using clock = std::chrono::steady_clock;
    // A screenshot (taken by a command) is the command's time.
    editor::FramePhaseMs phase_ms{};
    rhi::DeviceCounts device_mark = device->counts(); // the device's totals when the last frame was timed
    float frame_wait_ms = 0.0f;      // the frame being timed: begin_frame's wait for the GPU
    float frame_release_ms = 0.0f;   // and its freeing
    auto phase_mark = clock::now();
    bool in_screenshot = false;
    const auto mark = [&](editor::FramePhase phase) {
        const auto now = clock::now();
        const editor::FramePhase to = in_screenshot ? editor::FramePhase::mcp : phase;
        phase_ms[static_cast<usize>(to)] += std::chrono::duration<float, std::milli>(now - phase_mark).count();
        phase_mark = now;
    };
    std::vector<sim::DustPuff> dust_near;
    std::vector<render::DustPuff> dust_frame;
    std::vector<render::Occluder> occluder_frame;
    std::vector<render::Decal> wet_decals;   // what went in the water, wet a while
    std::vector<render::Decal> frame_decals; // the scene's and those
    std::vector<render::LocalLight> frame_lights; // the scene's lamps and the effects' lights
    frame_lights.reserve(render::k_max_lights + editor::EffectsState::k_max_effect_lights);
    float render_seconds = 1.0f / 60.0f; // since the last frame (auto exposure eases over it)
    float particle_seconds = 0.0f;       // the game's since the last frame (particles move by it)
    editor::WaterMeshes water_meshes;    // the scene's lakes, rivers and sea, drawn
    double water_seconds = 0.0;          // the waves' clock (the game's time)
    world::SwellTrack swell_track;       // the sea's swell following the wind
    static_assert(render::k_swell_waves == world::k_swell_waves);
    const auto record_frame = [&](rhi::CommandList& cmd, rhi::TextureHandle target) {
        mark(editor::FramePhase::other);
        // The people's trips moved on by the time simulated (stopped: everyone back where the scene has them).
        const editor::PeopleTravel::Clock travel_clock = editor::PeopleTravel::clock_of(physics);
        travel.update(state.scene, travel_clock.run, travel_clock.seconds);
        mark(editor::FramePhase::travel);
        // The one followed keeps the camera by it, where it is this frame (follow_person; play ends it).
        editor::follow_person_camera(state);
        // What the people hear of the noises made (a share of the people a frame while any spreads),
        // and who sees the player (at the camera: its eyes while playing).
        // (A screenshot's frame does not look or listen again.)
        if (play.playing()) {
            senses.player = state.camera.position;
        } else {
            senses.player.reset();
        }
        if (!in_screenshot) senses.update(state.scene, travel);
        mark(editor::FramePhase::senses);
        // Those whose routines say so set off (a share of the people a frame), by the time of day shown.
        routines.update(state.scene, travel, time_of_day.shown().hours, &senses);
        mark(editor::FramePhase::routines);
        // The people near the camera get their entities (a few a frame) before the scene is drawn.
        people.view = editor::PeopleState::view_of(
            state.camera.yaw_degrees, state.camera.pitch_degrees, state.camera.vertical_fov_degrees,
            window->pixel_height() > 0 ? static_cast<float>(window->pixel_width()) / static_cast<float>(window->pixel_height())
                                       : 1.0f);
        people.update(state.scene, state.camera.position,
                      world_streaming.active(state.scene_generation) ? &world_streaming : nullptr);
        // Those farther off are figures (a share of the people a frame), drawn with the poses baked
        // (those done since are handed to the renderer first, a few meshes a frame).
        pose_bakes.update();
        figures.hold = people.hold;
        figures.update(state.scene, state.camera.position);
        mark(editor::FramePhase::people);
        {
            // Behind a game's loading screen nothing is played: the ground's tiles are meshed and
            // put up as fast as the machine can, not a few a frame.
            const bool loading = state.menus != nullptr && state.menus->loading().on;
            const u32 most = loading ? editor::TerrainMeshes::k_loading_per_frame : editor::TerrainMeshes::k_per_frame;
            terrain_meshes.builds_per_frame = terrain_meshes.uploads_per_frame = terrain_meshes.builds_in_flight = most;
            terrain_meshes.more_workers = loading ? editor::MoreWorkers::for_loading() : 0u;
        }
        terrain_meshes.update(state.scene.terrain);
        terrain_paint.update(state.scene.terrain, *renderer);
        mark(editor::FramePhase::terrain);
        far_ground.update(state.camera.position, world_streaming, state.scene_generation, terrain_meshes,
                          state.scene.terrain);
        mark(editor::FramePhase::far_ground);
        scatter.camera = state.camera.position;
        {
            // Behind a game's loading screen the plants are placed as fast as the machine can.
            const bool loading = state.menus != nullptr && state.menus->loading().on;
            scatter.cells_in_flight = loading ? editor::ScatterPlacer::k_loading_cells_in_flight : editor::ScatterPlacer::k_cells_in_flight;
            scatter.more_workers = loading ? editor::MoreWorkers::for_loading() : 0u;
            scatter.instances_per_frame =
                loading ? editor::ScatterPlacer::k_loading_instances_per_frame : editor::ScatterPlacer::k_instances_per_frame;
        }
        scatter.update(state.scene, terrain_meshes.last_changes());
        physics.refresh_forest(state.scene, scatter); // fallen trees of cells placed again, drawn lying this frame
        mark(editor::FramePhase::scatter);
        // Edits since the animation last looked (commands, panels): followed
        // first, so no cell is built with a copy its entity should no longer draw.
        animation.follow_now(state.scene, meshes, state.scene.untaken_changes());
        cloth.follow(state.scene, state.scene.untaken_changes());
        hair.follow(state.scene, state.scene.untaken_changes());
        swing.follow(state.scene, state.scene.untaken_changes());
        effects.follow(state.scene, state.scene.untaken_changes());
        nav.follow(state.scene, state.scene.untaken_changes());
        // Drawn with other meshes now: their cells rebuilt.
        std::vector<world::EntityId> redraw = animation.take_redraw();
        const std::vector<world::EntityId> cloth_redraw = cloth.take_redraw();
        redraw.insert(redraw.end(), cloth_redraw.begin(), cloth_redraw.end());
        const std::vector<world::EntityId> walker_redraw = walkers.take_redraw();
        redraw.insert(redraw.end(), walker_redraw.begin(), walker_redraw.end());
        const std::vector<world::EntityId> ragdoll_redraw = ragdolls.take_redraw();
        redraw.insert(redraw.end(), ragdoll_redraw.begin(), ragdoll_redraw.end());
        // Only drawn elsewhere: moved in place where they can be.
        std::vector<world::EntityId> moved = physics.take_redraw();
        const std::vector<world::EntityId> swung = swing.take_moved();
        moved.insert(moved.end(), swung.begin(), swung.end());
        const std::vector<world::EntityId> walked_ids = walkers.take_moved();
        moved.insert(moved.end(), walked_ids.begin(), walked_ids.end());
        const std::vector<world::EntityId> fallen_ids = ragdolls.take_moved();
        moved.insert(moved.end(), fallen_ids.begin(), fallen_ids.end());
        const std::vector<world::CellCoord> tree_redraw = physics.take_tree_redraw();
        textures.update(); // cooked since last frame: uploaded, a budget's worth
        editor::scene_frame(state.scene, *renderer, meshes, scene_frame, &terrain_meshes, &far_ground, &scatter,
                            &physics.poses(), redraw, &physics.piece_poses(), &physics.fallen_trees(), tree_redraw,
                            &animation.copies(), &cloth.meshes(), moved, &swing.poses(), &walkers.poses(),
                            &physics.moving(), &ragdolls.poses(), &animation.part_copies(), &figures.draws());
        mark(editor::FramePhase::scene);
        render::FrameInput input;
        input.view = editor::to_view(state.camera);
        // The sun (or at night the moon) where the clock puts it.
        const editor::ClockLight lit = editor::clock_light(state.scene.lighting, time_of_day.shown());
        input.lighting = editor::weather_lighting(lit.lighting, weather.shown(), lit.daylight);
        input.sky = editor::weather_sky(lit.lighting, weather.shown(), weather.cloud_offset(), weather.ground(),
                                        weather.time());
        editor::clock_sky(input.sky, time_of_day.shown());
        input.settings = state.render_settings;
        // Behind a game's loading screen nothing is played: the voxel world (the far land and its
        // light) is built with far more of each frame, so play does not begin with it still filling in.
        if (state.menus != nullptr && state.menus->loading().on)
            input.settings.voxel_ms_per_frame = std::max(input.settings.voxel_ms_per_frame, editor::GameMenus::k_loading_voxel_ms);
        input.instances = scene_frame.instances;
        input.cells = scene_frame.cells;
        input.materials = scene_frame.materials;
        input.skin_looks = scene_frame.skin_looks;
        input.eye_looks = scene_frame.eye_looks;
        input.terrain_layers = scene_frame.terrain_layers;
        input.decals = scene_frame.decals;
        wet_decals.clear();
        physics.wet().decals(physics.world(), wet_decals);
        if (!wet_decals.empty()) { // (after the scene's: wet over a stain)
            frame_decals.assign(scene_frame.decals.begin(), scene_frame.decals.end());
            frame_decals.insert(frame_decals.end(), wet_decals.begin(), wet_decals.end());
            input.decals = frame_decals;
        }
        input.lights = scene_frame.lights;
        if (!effects.lights().empty()) { // (a fire's glow: last update's, as its particles)
            frame_lights.assign(scene_frame.lights.begin(), scene_frame.lights.end());
            frame_lights.insert(frame_lights.end(), effects.lights().begin(), effects.lights().end());
            input.lights = frame_lights;
        }
        // The sea's swell (floated on by physics at this same clock).
        const world::Swell& swell = swell_track.swell();
        for (u32 i = 0; i < world::k_swell_waves; ++i) {
            const world::SwellWave& w = swell.waves[i];
            input.swell[i] = {w.dir_x, w.dir_z, w.k, w.amplitude, w.phase};
        }
        state.water_swell = world::swell_most(swell);
        // The water's surfaces: drawn in the lit view with the water on,
        // in the voxel world (lakes and rivers) whatever is drawn.
        water_meshes.update(state.scene.water);
        input.water_vertices = water_meshes.vertices();
        input.water_indices = water_meshes.indices();
        input.water_looks = water_meshes.looks();
        input.seas = water_meshes.seas();
        input.water_indices_under_seas = water_meshes.indices_under_seas();
        input.water_version = water_meshes.version();
        if (input.settings.water && input.settings.debug_view == render::DebugView::lit) {
            // The water the camera is in (not in a room under it).
            const float crest = world::swell_most(swell);
            input.underwater = editor::camera_water(state.scene.water, state.camera.position, crest);
            const double top = input.underwater.level +
                               (input.underwater.swell ? crest * static_cast<double>(input.underwater.look.waves) : 0.0);
            if (input.underwater.on && !physics.world().water_reaches(state.camera.position, top)) {
                input.underwater.on = false;
            }
        }
        state.water_broken_lakes = water_meshes.broken_lakes();
        input.water_seconds = water_seconds;
        // The dust in the air nearest the camera.
        physics.dust().nearest(state.camera.position, render::k_max_dust_puffs, dust_near);
        dust_frame.clear();
        for (const sim::DustPuff& d : dust_near) dust_frame.push_back({d.position, d.radius, d.density, d.color});
        input.dust = dust_frame;
        // Characters' bodies, for the light round them (where they are drawn).
        occluder_frame.clear();
        if (input.settings.character_occlusion) {
            editor::character_occluders(state.scene, meshes, animation, &textures, &physics.poses(), &walkers.poses(),
                                        &swing.poses(), state.camera.position, occluder_frame, &ragdolls.poses());
        }
        input.occluders = occluder_frame;
        input.wind = weather.wind();
        input.particle_styles = effects.styles();
        hair.place(&animation, &physics.poses(), &walkers.poses(), &swing.poses(), &ragdolls.poses()); // where each is drawn this frame
        input.hair = hair.draws();
        if (!in_screenshot) { // (a screenshot's frame gives no births again, and stands still)
            input.particle_bursts = effects.take();
            input.particle_seconds = particle_seconds; // the game's clock: paused, they stand still
            particle_seconds = 0.0f;
        } else {
            input.particle_seconds = 0.0f;
        }
        input.delta_seconds = in_screenshot ? 0.0f : render_seconds; // (a screenshot's frame does not ease it again)
        renderer->render(cmd, target, input);
        mark(editor::FramePhase::render);
        if (!in_screenshot) {
            for (usize i = 0; i < render::k_cpu_section_names.size(); ++i) {
                phase_ms[static_cast<usize>(editor::FramePhase::count) + i] += renderer->last_cpu_ms()[i];
            }
        }
        state.scene_stats = renderer->last_stats();
        cmd.begin_timing("UI");
        ui_lists.clear();
        if (game_ui.shown()) ui_lists.push_back(game_ui.list()); // under the panels
        for (const rhi::UiDrawList& l : ui->frame().lists) ui_lists.push_back(l);
        cmd.draw_ui(target, rhi::UiFrame{ui_lists});
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

    // Run as a game: the world it is of is opened now, and the game's menus lead from the first frame.
    if (!options.game.empty()) {
        if (game_ui.font() == nullptr) {
            log_error("game", "the game's menus cannot be drawn (no font): a game has no other way in or out");
            return 1;
        }
        state.game_mode = true;
        const ai::CommandResult opened = commands.execute("open_world", {{"folder", options.game}}, {});
        if (!opened.ok) {
            log_error("game", "the game's world could not be opened (" + options.game + "): " + opened.error);
            return 1;
        }
    }

    // The null backend has no display to wait for, so pace it like vsync
    // instead of spinning a CPU core.
    const bool pace_null = device->backend() == rhi::Backend::null && sc_desc.vsync;

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
        const bool window_handled = window->take_handled();
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
            chat.update(); // (Claude's work goes on behind a minimised window)
            last_frame = std::chrono::steady_clock::now();
            phase_ms = {}; // minimised time is no frame's
            device_mark = device->counts();
            frame_wait_ms = 0.0f;
            frame_release_ms = 0.0f;
            phase_mark = last_frame;
            cpu_mark = thread_cpu_ms();
            continue;
        }
        const auto frame_start = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(frame_start - last_frame).count();
        last_frame = frame_start;
        render_seconds = delta;
        frame_info.frame_ms = 1000.0f * delta;
        // The phases since the last frame began: the same span as delta.
        mark(editor::FramePhase::other);
        const double cpu_now = thread_cpu_ms();
        {
            // The device's work over the same span (its running totals'
            // difference: loads between frames count too).
            const rhi::DeviceCounts counts = device->counts();
            const editor::FrameDevice frame_device{static_cast<u32>(counts.created - device_mark.created),
                                                   static_cast<u32>(counts.allocations - device_mark.allocations),
                                                   static_cast<u32>(counts.released - device_mark.released),
                                                   frame_wait_ms, frame_release_ms};
            state.frame_times.add(frame_info.frame_ms, phase_ms, static_cast<float>(cpu_now - cpu_mark), frame_device,
                                  window_handled);
            device_mark = counts;
            frame_wait_ms = 0.0f;
            frame_release_ms = 0.0f;
        }
        cpu_mark = cpu_now;
        phase_ms = {};

        scripts.update(menus.paused() ? 0.0f : delta); // (a menu over play: scripts stand still too)
        chat.update(); // (a command Claude asked for, if one waits)
        state.chat_stamp = chat.stamp();
        state.chat_busy = chat.busy();
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
        // A menu over play takes the mouse and keys (Esc opens the pause menu).
        const float pixels_per_point = window->point_width() > 0
                                           ? static_cast<float>(window->pixel_width()) /
                                                 static_cast<float>(window->point_width())
                                           : 1.0f;
        const bool menu_took = menus.update(input, pixels_per_point);
        // While a menu is open over play, the game stands still.
        float game_delta = static_cast<float>(menus.game_seconds(delta));
        if (play.playing()) {
            // The player's keys and mouse, not the editor camera's.
            if (!menu_took) play.handle_input(input, ui->wants_keyboard());
        } else if (!state.game_mode) { // (a game has no editor camera, brush or panels: its menus only)
            viewport_camera.handle_input(input, ui->wants_mouse());
            terrain_brush.update(input, ui->wants_mouse(), static_cast<float>(window->point_width()),
                                 static_cast<float>(window->point_height()), delta, state, commands);
            viewport_camera.update(state.camera, delta);
        }
        if (!state.game_mode) panels.draw(frame_info);
        ui->end_frame();
        mark(editor::FramePhase::ui);
        saves.update(state, game_delta); // a save or load's result, a load's cells a share a frame (before play)
        // (A game loaded behind the loading screen that was saved in play plays from here: held still
        // from this, its first frame, as it is by a menu over play.)
        if (menus.paused()) game_delta = 0.0f;
        // The player's wanted motion (or play stopped: Esc, a new scene).
        const bool playing = play.before_step(state.scene, state.camera, state.scene_generation, game_delta);
        window->set_relative_mouse(playing && !menus.paused()); // mouse look (a menu: the pointer is free)
        weather.update(saves.weather() != nullptr ? *saves.weather() : state.scene.weather, game_delta, state.camera.position,
                       state.scene_generation); // after the panels' changes (a loaded game's shown over the scene's)
        // Play moves the time on, and simulating when asked to (`simulate` with `clock`).
        time_of_day.update(state.scene.clock, game_delta, state.clock_runs(playing, physics.simulating()),
                           state.scene_generation);
        // What the last frame took from the scene; dynamic entities move
        // while simulating (drawn there this frame).
        mark(editor::FramePhase::weather);
        physics.wind = weather.shown() ? world::wind_velocity(*weather.shown()) : Vec3{};
        physics.feed_forest(state.scene, scatter, state.camera.position);
        walkers.others.clear(); // the player kept clear of
        if (play.playing()) {
            if (const auto p = play.player()) {
                walkers.others.push_back({p->position, p->velocity, play.settings().body.radius});
            }
        }
        walkers.before_step(state.scene, game_delta); // steered towards their paths' corners
        ragdolls.before_step(state.scene, meshes, game_delta); // those gone let go, those struck down made, those rested risen
        // The sea's swell from the wind, at the water's clock, then the clock
        // moved on with the game (paused: the sea stands still with what floats).
        // (Physics stepped on by a command, step_physics or step_play: the clock with it.)
        water_seconds = std::max(water_seconds, physics.world().swell_seconds() +
                                                    physics.world().step_fraction() * sim::PhysicsWorld::k_step_seconds);
        swell_track.update(weather.wind().velocity, water_seconds);
        physics.world().set_swell(swell_track.swell(), water_seconds);
        water_seconds += game_delta;
        physics.update(state.scene, scene_frame.take_for_followers(), game_delta);
        walkers.after_step(); // drawn where they walked
        ragdolls.after_step(); // posed by their bones (handed to the animation below)
        if (playing) play.after_step(state.camera, game_delta); // the camera at its eyes
        mark(editor::FramePhase::physics);
        animation.set_view(editor::to_view(state.camera), window->pixel_height()); // animation LOD by screen size
        // Matched characters walk as their walkers are steered (the rest stand).
        for (const auto& [id, walker] : walkers.walkers()) {
            if (walker.phase == editor::WalkerState::Phase::walking) {
                animation.set_goal(id, editor::AnimationState::goal_of(walker.velocity, walker.yaw));
            }
        }
        animation.update(state.scene, meshes, scene_frame.take_for_animation(), game_delta);
        hit_targets.update(state.scene, meshes, animation); // bullets meet this pose next frame
        mark(editor::FramePhase::animation);
        cloth.update(weather.wind(), state.camera.position, game_delta); // drawn as it moved next frame
        hair.update(weather.wind(), state.camera.position, game_delta);  // likewise
        swing.update(weather.wind(), state.camera.position, game_delta);
        mark(editor::FramePhase::cloth);
        editor::event_effects_frame(state, game_delta); // this frame's dust thrown up, running feet
        effects.follow(state.scene, state.scene.untaken_changes());
        particle_seconds += game_delta; // (a frame not drawn: carried to the next)
        effects.update(state.scene, state.camera.position, game_delta,
                       [&state](double x, double z) { return state.scene.terrain.height_at(x, z); });
        {
            const sim::PhysicsWorld& pw = physics.world();
            weather_effects.update(
                effects, weather.shown(), weather.ground(), weather.wind(), state.camera.position, game_delta,
                [&state](double x, double z) { return state.scene.terrain.height_at(x, z); },
                [&pw](double x, double z) { return editor::WeatherEffects::water_in(pw, x, z); },
                [&pw](double x, double z, double from_y, double distance) {
                    return editor::WeatherEffects::surface_in(pw, x, z, from_y, distance);
                },
                [&pw](const DVec3& p) { return editor::WeatherEffects::sheltered_in(pw, p); });
        }
        mark(editor::FramePhase::effects);
        nav.update(state.scene); // tiles taken and started, searches moved on (bounded)
        mark(editor::FramePhase::nav);
        audio.set_weather(weather.shown() ? &*weather.shown() : nullptr, weather.ground());
        {
            // The water heard round the camera (as the physics has it: the sea with its swell).
            const sim::PhysicsWorld& pw = physics.world();
            const Vec3 wind_v = weather.wind().velocity;
            const float wind = std::sqrt(wind_v.x * wind_v.x + wind_v.z * wind_v.z);
            audio.set_water(water_hearing.update(
                [&pw](double x, double z) { return pw.water_at(x, z); },
                [&state](double x, double z) { return state.scene.terrain.height_at(x, z); }, state.camera.position,
                wind, [&pw, &state](double level) { return pw.water_reaches(state.camera.position, level); }));
        }
        audio.update(state.camera, delta); // heard from the camera (where the player's eyes are in play)
        mark(editor::FramePhase::audio);
        // The game's UI from this frame's camera (the player's eyes, in play): markers meet the scene drawn.
        game_ui.update(state, delta, static_cast<float>(window->pixel_width()),
                       static_cast<float>(window->pixel_height())); // (counted with the panels)
        mark(editor::FramePhase::ui);
        // (Only a plain window's size is the one to open with next time.)
        if (state.game_mode && !menus.settings().fullscreen && !window->fullscreen() && !window->maximized())
            menus.note_window(window->point_width(), window->point_height());
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
        {
            // (Behind a game's loading screen, cells read are put in the scene whole frames' worth at once.)
            const u32 a_frame = world_streaming.settings.entities_per_frame;
            const u32 at_once = world_streaming.settings.reads_in_flight;
            const u32 readers = world_streaming.settings.more_readers;
            if (state.menus != nullptr && state.menus->loading().on) {
                world_streaming.settings.entities_per_frame = std::max(a_frame, editor::k_loading_entities_per_frame);
                world_streaming.settings.reads_in_flight = std::max(at_once, editor::k_loading_reads_in_flight);
                world_streaming.settings.more_readers = std::max(readers, editor::MoreWorkers::for_loading());
            }
            world_streaming.update(state.scene, state.scene_generation, state.camera.position, scene_frame);
            world_streaming.settings.entities_per_frame = a_frame;
            world_streaming.settings.reads_in_flight = at_once;
            world_streaming.settings.more_readers = readers;
        }
        mark(editor::FramePhase::streaming);
        if (state.selected != 0 && state.scene.find(state.selected) == nullptr) {
            state.selected = 0;
        }
        frame_info.ui_draw_calls = device->last_frame_stats().ui_draws;
        frame_wait_ms = device->last_frame_stats().wait_ms;
        frame_release_ms = device->last_frame_stats().release_ms;
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

    menus.store(); // (a game: its settings as they are now, the window's size with them)
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
    try {
        if (!take_packaged_game(options)) {
            return 1;
        }
    } catch (const std::exception& error) {
        log_error("game", std::string("game.json beside the program: ") + error.what());
        return 1;
    }
    if (!options.player_settings.empty() && options.game.empty()) {
        log_error("editor", "--player-settings is a game's (--game, or a packaged game): the editor keeps no settings");
        return 2;
    }
    return run(options);
}
