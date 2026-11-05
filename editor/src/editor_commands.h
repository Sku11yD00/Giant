// The editor's own commands: everything the editor can do so far, as
// commands people and the AI share (rule 7 in AGENTS.md).
#pragma once

#include "giant/ai/commands.h"
#include "giant/core/log_history.h"
#include "giant/platform/window.h"
#include "giant/rhi/rhi.h"

#include <functional>
#include <vector>

namespace giant::editor {

// What the commands read and change. Owned by main; outlives the registry.
struct EditorState {
    platform::Window* window = nullptr;
    rhi::Device* device = nullptr;
    const LogHistory* log = nullptr;
    // A clear day-sky blue until the renderer draws something. Linear colour:
    // the back buffer view encodes it to sRGB for the screen.
    rhi::Color clear_color{0.15f, 0.34f, 0.71f, 1.0f};
    bool quit_requested = false;
    // Renders the editor as it looks now (viewport and panels) and returns
    // RGBA8 pixels at the window's pixel size; empty on failure. Set by main.
    std::function<std::vector<u8>()> render_screenshot;
};

// Registers every editor command. Asserts on a bad registration (a bug).
void register_editor_commands(ai::CommandRegistry& registry, EditorState& state);

// What the AI is told when it connects over MCP.
const char* mcp_instructions();

} // namespace giant::editor
