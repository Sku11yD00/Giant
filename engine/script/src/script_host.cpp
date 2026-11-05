#include "giant/script/script_host.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace giant::script {

namespace {

// --- The .NET hosting API (hostfxr), declared here so no SDK headers are
// needed to build the engine. See dotnet/runtime docs/design/features/
// native-hosting.md. -----------------------------------------------------
#if defined(_WIN32)
using char_t = wchar_t;
#define GIANT_HOSTFXR_CALL __cdecl
#define GIANT_CORECLR_CALL __stdcall
constexpr const char* k_hostfxr_name = "hostfxr.dll";
#else
using char_t = char;
#define GIANT_HOSTFXR_CALL
#define GIANT_CORECLR_CALL
#if defined(__APPLE__)
constexpr const char* k_hostfxr_name = "libhostfxr.dylib";
#else
constexpr const char* k_hostfxr_name = "libhostfxr.so";
#endif
#endif

struct HostfxrInitializeParameters {
    usize size;
    const char_t* host_path;
    const char_t* dotnet_root;
};
using HostfxrInitializeForRuntimeConfig = std::int32_t(GIANT_HOSTFXR_CALL*)(
    const char_t* runtime_config_path, const HostfxrInitializeParameters* parameters,
    void** host_context);
using HostfxrGetRuntimeDelegate = std::int32_t(GIANT_HOSTFXR_CALL*)(void* host_context,
                                                                    std::int32_t type,
                                                                    void** delegate);
using HostfxrClose = std::int32_t(GIANT_HOSTFXR_CALL*)(void* host_context);
using LoadAssemblyAndGetFunctionPointer = int(GIANT_CORECLR_CALL*)(
    const char_t* assembly_path, const char_t* type_name, const char_t* method_name,
    const char_t* delegate_type_name, void* reserved, void** delegate);
constexpr std::int32_t k_hdt_load_assembly_and_get_function_pointer = 5;
const char_t* const k_unmanaged_callers_only = reinterpret_cast<const char_t*>(-1);

// --- What the managed side calls back into (NativeApi in NativeHost.cs). ---
struct NativeApi {
    int (*execute)(const char* name, const char* args, char** reply);
    void (*log)(int level, const char* text);
    void (*free_string)(char* text);
};

// The command handler of the one host. Main thread only.
CommandHandler* g_commands = nullptr;

char* copy_string(const std::string& text) {
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out != nullptr) {
        std::memcpy(out, text.c_str(), text.size() + 1);
    }
    return out;
}

int native_execute(const char* name, const char* args, char** reply) {
    CommandReply result;
    if (g_commands == nullptr || !*g_commands) {
        result = {false, "scripts cannot run commands right now"};
    } else {
        result = (*g_commands)(name != nullptr ? name : "", args != nullptr ? args : "{}");
    }
    *reply = copy_string(result.text);
    return result.ok ? 0 : 1;
}

void native_log(int level, const char* text) {
    const LogLevel l = level <= 0   ? LogLevel::trace
                       : level == 1 ? LogLevel::info
                       : level == 2 ? LogLevel::warning
                                    : LogLevel::error;
    log_message(l, "script", text != nullptr ? text : "");
}

void native_free_string(char* text) { std::free(text); }

// --- Loading libraries. ------------------------------------------------
void* open_library(const std::filesystem::path& path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryW(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* library, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

// "10.0.14" style folder names, compared by number.
std::vector<int> version_parts(const std::string& name) {
    std::vector<int> parts;
    int current = 0;
    bool any = false;
    for (const char c : name) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + (c - '0');
            any = true;
        } else if (c == '.') {
            parts.push_back(current);
            current = 0;
        } else {
            break; // "-preview..." and the like
        }
    }
    if (any) {
        parts.push_back(current);
    }
    return parts;
}

std::filesystem::path find_hostfxr(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::path best;
    std::vector<int> best_version;
    for (const auto& entry : std::filesystem::directory_iterator(root / "host" / "fxr", error)) {
        const auto version = version_parts(entry.path().filename().string());
        const auto candidate = entry.path() / k_hostfxr_name;
        if (!version.empty() && version > best_version &&
            std::filesystem::exists(candidate, error)) {
            best = candidate;
            best_version = version;
        }
    }
    return best;
}

template <typename Fn>
Fn symbol(void* library, const char* name) {
    return reinterpret_cast<Fn>(find_symbol(library, name));
}

// An environment variable as a path; empty when unset. Windows reads the
// wide value (non-ASCII folders) without the CRT's deprecated getenv.
std::filesystem::path env_path(const char* name) {
#if defined(_WIN32)
    const std::wstring wide(name, name + std::strlen(name));
    wchar_t* value = nullptr;
    usize length = 0;
    std::filesystem::path result;
    if (_wdupenv_s(&value, &length, wide.c_str()) == 0 && value != nullptr) {
        result = value;
    }
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::filesystem::path(value) : std::filesystem::path();
#endif
}

} // namespace

std::filesystem::path find_dotnet_root() {
    std::error_code error;
    if (auto env = env_path("DOTNET_ROOT"); !env.empty()) {
        return env;
    }
    std::vector<std::filesystem::path> candidates;
#if defined(_WIN32)
    if (auto program_files = env_path("ProgramFiles"); !program_files.empty()) {
        candidates.emplace_back(program_files / "dotnet");
    }
    candidates.emplace_back("C:/Program Files/dotnet");
    const wchar_t* exe = L"dotnet.exe";
    const wchar_t separator = L';';
#else
    candidates = {"/usr/lib/dotnet", "/usr/share/dotnet", "/usr/local/share/dotnet",
                  "/opt/dotnet"};
    if (auto home = env_path("HOME"); !home.empty()) {
        candidates.emplace_back(home / ".dotnet");
    }
    const char* exe = "dotnet";
    const char separator = ':';
#endif
    for (const auto& candidate : candidates) {
        if (!find_hostfxr(candidate).empty()) {
            return candidate;
        }
    }
    // Last, the folder of `dotnet` on the PATH (following symlinks).
    if (const auto path = env_path("PATH"); !path.empty()) {
        const auto all = path.native();
        usize start = 0;
        while (start <= all.size()) {
            const usize end = std::min(all.find(separator, start), all.size());
            const std::filesystem::path dir = all.substr(start, end - start);
            const auto program = dir / exe;
            if (!dir.empty() && std::filesystem::exists(program, error)) {
                const auto real = std::filesystem::canonical(program, error);
                if (!error && !find_hostfxr(real.parent_path()).empty()) {
                    return real.parent_path();
                }
            }
            start = end + 1;
        }
    }
    return {};
}

struct ScriptHost::Api {
    int (*initialize)(const NativeApi* api, int size) = nullptr;
    int (*load_scripts)(const char* path) = nullptr;
    int (*update)(float delta_seconds) = nullptr;
    int (*unload_scripts)() = nullptr;
    int (*running_script_count)() = nullptr;
    CommandHandler commands;
    NativeApi native{};
};

ScriptHost::ScriptHost(ConstructKey, std::unique_ptr<Api> api) : m_api(std::move(api)) {}

std::unique_ptr<ScriptHost> ScriptHost::create(const ScriptHostDesc& desc) {
    static std::atomic<bool> created{false};
    if (created.exchange(true)) {
        log_error("script", ".NET can only be started once per process");
        return nullptr;
    }

    const std::filesystem::path root = desc.dotnet_root.empty() ? find_dotnet_root() : desc.dotnet_root;
    const std::filesystem::path hostfxr = root.empty() ? std::filesystem::path() : find_hostfxr(root);
    if (hostfxr.empty()) {
        log_error("script", "C# scripts need the .NET 10 runtime, which was not found. Install it "
                            "from https://dotnet.microsoft.com/download/dotnet/10.0 or set DOTNET_ROOT.");
        return nullptr;
    }
    void* library = open_library(hostfxr);
    if (library == nullptr) {
        log_error("script", "could not load " + hostfxr.string());
        return nullptr;
    }
    // The library stays loaded for the life of the process: .NET cannot be
    // unloaded once started.
    const auto init = symbol<HostfxrInitializeForRuntimeConfig>(
        library, "hostfxr_initialize_for_runtime_config");
    const auto get_delegate =
        symbol<HostfxrGetRuntimeDelegate>(library, "hostfxr_get_runtime_delegate");
    const auto close = symbol<HostfxrClose>(library, "hostfxr_close");
    if (init == nullptr || get_delegate == nullptr || close == nullptr) {
        log_error("script", "hostfxr is missing the hosting functions; .NET is too old");
        return nullptr;
    }

    const std::filesystem::path config = desc.managed_dir / "Giant.Scripting.runtimeconfig.json";
    const std::filesystem::path assembly = desc.managed_dir / "Giant.Scripting.dll";
    std::error_code error;
    if (!std::filesystem::exists(config, error) || !std::filesystem::exists(assembly, error)) {
        log_error("script", "the engine's C# API is missing from " + desc.managed_dir.string());
        return nullptr;
    }

    const std::filesystem::path::string_type root_string = root.native();
    HostfxrInitializeParameters params{sizeof(HostfxrInitializeParameters), nullptr,
                                       root_string.c_str()};
    void* context = nullptr;
    const std::int32_t rc = init(config.c_str(), &params, &context);
    if (rc < 0 || context == nullptr) {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(rc));
        log_error("script", std::string("could not start .NET (") + code +
                                "); is the .NET 10 runtime installed?");
        if (context != nullptr) {
            close(context);
        }
        return nullptr;
    }
    void* load_raw = nullptr;
    const std::int32_t delegate_rc =
        get_delegate(context, k_hdt_load_assembly_and_get_function_pointer, &load_raw);
    close(context);
    if (delegate_rc < 0 || load_raw == nullptr) {
        log_error("script", "could not get the .NET loader");
        return nullptr;
    }
    const auto load = reinterpret_cast<LoadAssemblyAndGetFunctionPointer>(load_raw);

#if defined(_WIN32)
    const wchar_t* type_name = L"Giant.NativeHost, Giant.Scripting";
#define GIANT_NAME(x) L##x
#else
    const char* type_name = "Giant.NativeHost, Giant.Scripting";
#define GIANT_NAME(x) x
#endif
    auto api = std::make_unique<Api>();
    bool ok = true;
    const auto get = [&](const char_t* method, auto& out) {
        void* fn = nullptr;
        const int status = load(assembly.c_str(), type_name, method, k_unmanaged_callers_only,
                                nullptr, &fn);
        if (status < 0 || fn == nullptr) {
            ok = false;
            return;
        }
        out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(fn);
    };
    get(GIANT_NAME("Initialize"), api->initialize);
    get(GIANT_NAME("LoadScripts"), api->load_scripts);
    get(GIANT_NAME("Update"), api->update);
    get(GIANT_NAME("UnloadScripts"), api->unload_scripts);
    get(GIANT_NAME("RunningScriptCount"), api->running_script_count);
#undef GIANT_NAME
    if (!ok) {
        log_error("script", "Giant.Scripting.dll does not match this engine build");
        return nullptr;
    }

    api->commands = desc.commands;
    api->native = {&native_execute, &native_log, &native_free_string};
    if (api->initialize(&api->native, static_cast<int>(sizeof(NativeApi))) != 0) {
        log_error("script", "Giant.Scripting.dll does not match this engine build");
        return nullptr;
    }
    auto host = std::make_unique<ScriptHost>(ConstructKey{}, std::move(api));
    g_commands = &host->m_api->commands;
    log_info("script", ".NET started from " + root.string());
    return host;
}

ScriptHost::~ScriptHost() {
    unload_scripts();
    g_commands = nullptr;
}

bool ScriptHost::load_scripts(const std::filesystem::path& assembly) {
    const std::u8string u8 = assembly.u8string();
    const std::string utf8(u8.begin(), u8.end());
    return m_api->load_scripts(utf8.c_str()) == 0;
}

void ScriptHost::update(float delta_seconds) { m_api->update(delta_seconds); }

void ScriptHost::unload_scripts() { m_api->unload_scripts(); }

u32 ScriptHost::running_scripts() const {
    return static_cast<u32>(std::max(m_api->running_script_count(), 0));
}

} // namespace giant::script
