// The chat's way to its C# side (scripts/Giant.Chat, which holds the
// conversation and talks to Claude's service): started with .NET when the
// first message is sent, and given a turn each frame to run the commands
// Claude asks for, on the main thread. A command the person is asked about
// first (ChatGate) is held here until they have answered.
#pragma once

#include "chat.h"

#include "giant/script/script_host.h"

#include <filesystem>
#include <functional>
#include <string>

namespace giant::editor {

class ChatBridge final : public ChatLink {
public:
    // `host` gives .NET, starting it when first needed (null when it cannot
    // be). `managed_dir` holds chat/Giant.Chat.dll. One per process.
    // `gate`: the person's say over what Claude changes (EditorState::chat_gate).
    ChatBridge(ai::CommandRegistry& registry, ChatGate& gate, std::function<script::ScriptHost*()> host,
               std::filesystem::path managed_dir);
    ~ChatBridge() override;

    ChatBridge(const ChatBridge&) = delete;
    ChatBridge& operator=(const ChatBridge&) = delete;

    ai::CommandResult ask(std::string_view what, const ai::Json& args) override;

    // Each frame: runs a command Claude is waiting for, if any (one a frame).
    void update();
    // Claude is working on a message (as of the last update).
    bool busy() const { return m_busy; }
    // Another number whenever the conversation may have changed (for
    // EditorState::chat_stamp: the Chat panel reads it anew then).
    u64 stamp() const { return m_stamp.value(); }

private:
    bool start();

    ChatGate& m_gate;
    std::function<script::ScriptHost*()> m_host;
    std::filesystem::path m_managed_dir;
    int (*m_ask)(const char* what, const char* args, const char** reply) = nullptr;
    int (*m_update)() = nullptr;
    bool m_started = false;
    bool m_busy = false;
    ChatStamp m_stamp;
    std::string m_failed; // why the chat could not start, once it could not
};

} // namespace giant::editor
