#include "chat_bridge.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/core/text.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

// What the C# side calls back into (NativeChatApi in Native.cs).
struct ChatApi {
    // 0: done, 1: failed (the reply says what or why), 2: held until the
    // person has answered (no reply; held_tool then).
    int (*run_tool)(const char* name, const char* input, char** reply);
    // The tool held. `what` 0: has the person answered? (0 yes, 2 not yet.)
    // 1: its reply, running it if they allowed it (0 done, 1 failed).
    // 2: it is withdrawn (Claude's work was stopped); 0.
    int (*held_tool)(int what, char** reply); // (HeldAsk, chat.h)
    // A picture of the result of the tool run last (by run_tool, or held_tool's finish): the `index`th, 0 first.
    // 1 with its bytes (the editor's: to be copied before the next call), their count and its type
    // ("image/png"); 0 when there is none of that index (asked so, the editor lets the pictures go:
    // they are asked for in order, once).
    int (*tool_image)(int index, const unsigned char** data, int* size, const char** mime_type);
    void (*log)(int level, const char* text);
    void (*free_string)(char* text);
};

// The registry and the gate of the one bridge. Main thread only.
ai::CommandRegistry* g_registry = nullptr;
ChatGate* g_gate = nullptr;
// The pictures of the result of the tool run last, until the next is run.
std::vector<ai::CommandImage> g_images;


char* copy_string(const std::string& text) {
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out != nullptr) {
        std::memcpy(out, text.c_str(), text.size() + 1);
    }
    return out;
}

int chat_run_tool(const char* name, const char* input, char** reply) {
    g_images.clear();
    ChatToolReply result;
    const auto parsed = ai::parse_json(input != nullptr ? input : "{}");
    if (g_registry == nullptr) {
        result = {false, "the editor cannot run commands right now"};
    } else if (!parsed) {
        result = {false, "the tool's input is not valid JSON"};
    } else {
        result = run_chat_tool(*g_registry, name != nullptr ? name : "", *parsed, g_gate);
    }
    if (result.held) {
        *reply = nullptr;
        return k_chat_tool_held;
    }
    g_images = std::move(result.images);
    *reply = copy_string(result.text);
    return result.ok ? k_chat_tool_done : k_chat_tool_failed;
}

int chat_tool_image(int index, const unsigned char** data, int* size, const char** mime_type) {
    if (index < 0 || static_cast<usize>(index) >= g_images.size()) {
        g_images.clear(); // (asked past the last: all were copied, and need not be kept here)
        return 0;
    }
    const ai::CommandImage& image = g_images[static_cast<usize>(index)];
    *data = reinterpret_cast<const unsigned char*>(image.data.data());
    *size = static_cast<int>(image.data.size()); // (at most k_chat_most_image_bytes)
    *mime_type = image.mime_type.c_str();
    return 1;
}

int chat_held_tool(int what, char** reply) {
    *reply = nullptr;
    const bool finish = what == static_cast<int>(HeldAsk::finish);
    if (finish) {
        g_images.clear();
    }
    if (g_registry == nullptr || g_gate == nullptr) {
        if (finish) {
            *reply = copy_string("the editor cannot run commands right now");
        }
        return finish ? k_chat_tool_failed : k_chat_tool_done;
    }
    std::string text;
    std::vector<ai::CommandImage> images;
    const int status = held_chat_tool(*g_registry, *g_gate, what, text, images);
    if (finish) {
        g_images = std::move(images);
        *reply = copy_string(text);
    }
    return status;
}

void chat_log(int level, const char* text) {
    const LogLevel l = level <= 0 ? LogLevel::trace : level == 1 ? LogLevel::info : level == 2 ? LogLevel::warning : LogLevel::error;
    log_message(l, "chat", text != nullptr ? text : "");
}

void chat_free_string(char* text) { std::free(text); }

const ChatApi k_api{&chat_run_tool, &chat_held_tool, &chat_tool_image, &chat_log, &chat_free_string};

} // namespace

ChatBridge::ChatBridge(ai::CommandRegistry& registry, ChatGate& gate, std::function<script::ScriptHost*()> host,
                       std::filesystem::path managed_dir)
    : m_gate(gate), m_host(std::move(host)), m_managed_dir(std::move(managed_dir)) {
    GIANT_ASSERT(g_registry == nullptr, "one chat per process");
    g_registry = &registry;
    g_gate = &gate;
}

ChatBridge::~ChatBridge() {
    g_images.clear();
    g_registry = nullptr;
    g_gate = nullptr;
}

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
                                               {"busy", false},                {"started", false}, {"asking", nullptr}});
        }
        return ai::CommandResult::success(what == "stop" ? ai::Json{{"stopped", false}} : ai::Json::object());
    }
    m_stamp.asked(what); // (whatever comes of it: a message refused shows too)
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
    // (Before anything else can go wrong: what the person allowed for the last message is not
    // allowed for this one, and stopped or cleared work takes its question from the window at
    // once, not when the C# side next has its turn.)
    m_gate.chat_asked(what);
    auto value = ai::parse_json(text);
    if (!value || !value->is_object()) {
        return ai::CommandResult::failure("the chat's answer could not be read");
    }
    if (what == "read" || what == "send") {
        (*value)["started"] = true;
    }
    if (what == "read") {
        (*value)["asking"] = m_gate.asking();
    }
    if (what == "send") {
        m_busy = true; // (until the next update says otherwise)
    }
    return ai::CommandResult::success(std::move(*value));
}

void ChatBridge::update() {
    if (m_started) {
        const int now = m_update();
        m_busy = (now & 1) != 0;
        m_stamp.updated(now);
        m_gate.chat_updated(m_busy);
    }
}

} // namespace giant::editor
