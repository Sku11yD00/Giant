#include "chat_bridge.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/core/text.h"

#include <cstdlib>
#include <cstring>
#include <system_error>
#include <utility>

namespace giant::editor {

namespace {

// What the C# side calls back into (NativeChatApi in Native.cs).
struct ChatApi {
    int (*run_tool)(const char* name, const char* input, char** reply);
    void (*log)(int level, const char* text);
    void (*free_string)(char* text);
};

// The registry of the one bridge. Main thread only.
ai::CommandRegistry* g_registry = nullptr;

char* copy_string(const std::string& text) {
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out != nullptr) {
        std::memcpy(out, text.c_str(), text.size() + 1);
    }
    return out;
}

int chat_run_tool(const char* name, const char* input, char** reply) {
    ChatToolReply result;
    const auto parsed = ai::parse_json(input != nullptr ? input : "{}");
    if (g_registry == nullptr) {
        result = {false, "the editor cannot run commands right now"};
    } else if (!parsed) {
        result = {false, "the tool's input is not valid JSON"};
    } else {
        result = run_chat_tool(*g_registry, name != nullptr ? name : "", *parsed);
    }
    *reply = copy_string(result.text);
    return result.ok ? 0 : 1;
}

void chat_log(int level, const char* text) {
    const LogLevel l = level <= 0 ? LogLevel::trace : level == 1 ? LogLevel::info : level == 2 ? LogLevel::warning : LogLevel::error;
    log_message(l, "chat", text != nullptr ? text : "");
}

void chat_free_string(char* text) { std::free(text); }

const ChatApi k_api{&chat_run_tool, &chat_log, &chat_free_string};

} // namespace

ChatBridge::ChatBridge(ai::CommandRegistry& registry, std::function<script::ScriptHost*()> host,
                       std::filesystem::path managed_dir)
    : m_host(std::move(host)), m_managed_dir(std::move(managed_dir)) {
    GIANT_ASSERT(g_registry == nullptr, "one chat per process");
    g_registry = &registry;
}

ChatBridge::~ChatBridge() { g_registry = nullptr; }

bool ChatBridge::start() {
    if (m_started) {
        return true;
    }
    if (!m_failed.empty()) {
        return false;
    }
    const std::filesystem::path assembly = m_managed_dir / "chat" / "Giant.Chat.dll";
    std::error_code error;
    if (!std::filesystem::exists(assembly, error)) {
        m_failed = "the chat is missing from this editor (" + path_to_utf8(assembly) + ")";
        return false;
    }
    script::ScriptHost* host = m_host ? m_host() : nullptr;
    if (host == nullptr) {
        m_failed = "the chat needs the .NET 10 runtime, which could not be started; see get_log";
        return false;
    }
    const char* type = "Giant.Chat.Native, Giant.Chat";
    const auto initialize =
        reinterpret_cast<int (*)(const ChatApi*, int, const char*)>(host->function(assembly, type, "Initialize"));
    m_ask = reinterpret_cast<int (*)(const char*, const char*, const char**)>(host->function(assembly, type, "Ask"));
    m_update = reinterpret_cast<int (*)()>(host->function(assembly, type, "Update"));
    if (initialize == nullptr || m_ask == nullptr || m_update == nullptr ||
        initialize(&k_api, static_cast<int>(sizeof(ChatApi)), ai::to_json_text(chat_tools()).c_str()) != 0) {
        m_failed = "the chat does not match this editor build; see get_log";
        return false;
    }
    m_started = true;
    log_info("chat", "the chat with Claude started");
    return true;
}

ai::CommandResult ChatBridge::ask(std::string_view what, const ai::Json& args) {
    // Only a message starts .NET: reading an unstarted chat says it is empty.
    if (!m_started && what != "send") {
        if (what == "read") {
            return ai::CommandResult::success({{"entries", ai::Json::array()}, {"after", 0}, {"latest", 0}, {"oldest", 1},
                                               {"busy", false},                {"started", false}});
        }
        return ai::CommandResult::success(what == "stop" ? ai::Json{{"stopped", false}} : ai::Json::object());
    }
    if (!start()) {
        return ai::CommandResult::failure(m_failed);
    }
    const std::string name(what);
    const char* reply = nullptr;
    const int status = m_ask(name.c_str(), ai::to_json_text(args).c_str(), &reply);
    const std::string text = reply != nullptr ? reply : "";
    if (status != 0) {
        return ai::CommandResult::failure(text);
    }
    auto value = ai::parse_json(text);
    if (!value || !value->is_object()) {
        return ai::CommandResult::failure("the chat's answer could not be read");
    }
    if (what == "read" || what == "send") {
        (*value)["started"] = true;
    }
    if (what == "send") {
        m_busy = true; // (until the next update says otherwise)
    }
    ++m_stamp; // (a message sent, the work stopped, the conversation cleared: something to read anew)
    return ai::CommandResult::success(std::move(*value));
}

void ChatBridge::update() {
    if (m_started) {
        const int now = m_update();
        m_busy = (now & 1) != 0;
        if (now != m_last) {
            m_last = now;
            ++m_stamp;
        }
    }
}

} // namespace giant::editor
