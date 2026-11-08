// Game save slots: the manifest, its commit and its cleaning.
#include "giant/save/slot.h"

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace giant::save {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

namespace {

constexpr const char* k_manifest = "save.json";
constexpr const char* k_previous = "save.prev.json";

std::optional<std::string> read_text(const fs::path& path, usize most) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > most) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::stringstream text;
    text << file.rdbuf();
    if (file.bad()) return std::nullopt;
    return text.str();
}

// Written beside, flushed to the disk, then renamed over: neither a crash
// nor a power loss leaves half a file under its name, and what a later
// rename names is on the disk before it.
bool write_text(const fs::path& path, const std::string& text) {
    fs::path partial = path;
    partial += ".partial";
#if defined(_WIN32)
    const HANDLE file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (usize at = 0; ok && at < text.size();) {
        const DWORD part = static_cast<DWORD>(std::min<usize>(text.size() - at, usize{1} << 30));
        DWORD wrote = 0;
        ok = WriteFile(file, text.data() + at, part, &wrote, nullptr) != 0 && wrote == part;
        at += wrote;
    }
    ok = ok && FlushFileBuffers(file) != 0;
    ok = CloseHandle(file) != 0 && ok;
    if (ok) ok = MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        std::error_code ignored;
        fs::remove(partial, ignored);
    }
    return ok;
#else
    const int file = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (file < 0) return false;
    bool ok = true;
    for (usize at = 0; ok && at < text.size();) {
        const ssize_t wrote = ::write(file, text.data() + at, text.size() - at);
        if (wrote < 0 && errno == EINTR) continue;
        ok = wrote > 0;
        if (ok) at += static_cast<usize>(wrote);
    }
    ok = ok && ::fsync(file) == 0;
    ok = ::close(file) == 0 && ok;
    std::error_code error;
    if (ok) fs::rename(partial, path, error);
    if (!ok || error) {
        fs::remove(partial, error);
        return false;
    }
    return true;
#endif
}

// The folder's own entries (its renames) on the disk too.
void sync_folder([[maybe_unused]] const fs::path& dir) {
#if !defined(_WIN32)
    const int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d >= 0) {
        (void)::fsync(d);
        (void)::close(d);
    }
#endif
}

template <typename T>
bool parse_number(std::string_view s, T& out) {
    if (s.empty()) return false;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), out);
    return error == std::errc{} && end == s.data() + s.size();
}

std::string cell_file(CellKey key, u64 generation) {
    return "cells/" + std::to_string(key.x) + "_" + std::to_string(key.z) + "." + std::to_string(generation) + ".json";
}
std::string section_file(const std::string& name, u64 generation) {
    return "sections/" + name + "." + std::to_string(generation) + ".json";
}

// "<stem>.<G>.json" split (false: not of that shape).
bool split_generation(std::string_view name, std::string_view& stem, u64& generation) {
    constexpr std::string_view ext = ".json";
    if (name.size() <= ext.size() || name.substr(name.size() - ext.size()) != ext) return false;
    name.remove_suffix(ext.size());
    const usize dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    stem = name.substr(0, dot);
    return parse_number(name.substr(dot + 1), generation);
}

// A cell's file name names its own cell, in the cells folder.
bool cell_file_ok(CellKey key, std::string_view file) {
    constexpr std::string_view dir = "cells/";
    if (file.substr(0, dir.size()) != dir) return false;
    std::string_view stem;
    u64 g = 0;
    if (!split_generation(file.substr(dir.size()), stem, g)) return false;
    return file == cell_file(key, g);
}
bool section_file_ok(const std::string& name, std::string_view file) {
    constexpr std::string_view dir = "sections/";
    if (file.substr(0, dir.size()) != dir) return false;
    std::string_view stem;
    u64 g = 0;
    if (!split_generation(file.substr(dir.size()), stem, g)) return false;
    return file == section_file(name, g);
}

std::string hex(u64 v) {
    static const char* digits = "0123456789abcdef";
    std::string s(16, '0');
    for (int k = 15; k >= 0; --k) {
        s[static_cast<usize>(k)] = digits[v & 15u];
        v >>= 4;
    }
    return s;
}

std::optional<std::string> check_keys(const Json& j, std::initializer_list<const char*> keys, const char* where) {
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (std::find_if(keys.begin(), keys.end(), [&](const char* k) { return it.key() == k; }) == keys.end())
            return std::string("unknown key '") + it.key() + "' in " + where;
    }
    return std::nullopt;
}

// A whole number within [lo, hi] (not a bool, not a fraction).
template <typename T>
std::optional<T> whole(const Json& j, i64 lo, u64 hi) {
    if (j.is_number_unsigned()) {
        const u64 v = j.get<u64>();
        if (v > hi) return std::nullopt;
        return static_cast<T>(v);
    }
    if (j.is_number_integer()) {
        const i64 v = j.get<i64>();
        if (v < lo || (v >= 0 && static_cast<u64>(v) > hi)) return std::nullopt;
        return static_cast<T>(v);
    }
    return std::nullopt;
}

// The format a manifest's text says it is (0: none it says).
u64 format_of(std::string_view text) {
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("format")) return 0;
    return whole<u64>(j["format"], 0, ~u64{0}).value_or(0);
}

std::set<std::string> files_of(const Manifest& m) {
    std::set<std::string> out;
    for (const auto& [name, file] : m.sections) out.insert(file);
    for (const auto& [key, file] : m.cells) out.insert(file);
    return out;
}

} // namespace

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    // Lowercase only (folders that ignore case would make two names one), and none a device's on Windows.
    if (!std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        }))
        return false;
    for (const std::string_view device : {"con", "prn", "aux", "nul"})
        if (name == device) return false;
    if (name.size() == 4 && (name.substr(0, 3) == "com" || name.substr(0, 3) == "lpt") && name[3] >= '0' && name[3] <= '9')
        return false;
    return true;
}

u64 hash_text(std::string_view text) {
    u64 h = 14695981039346656037ull;
    for (const char c : text) {
        h ^= static_cast<u8>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::string save_manifest(const Manifest& m) {
    Json j;
    j["format"] = m.format;
    j["world"] = {{"name", m.world_name}, {"hash", hex(m.world_hash)}};
    j["saved_unix"] = m.saved_unix;
    j["play_seconds"] = m.play_seconds;
    j["generation"] = m.generation;
    Json sections = Json::object();
    for (const auto& [name, file] : m.sections) sections[name] = file;
    j["sections"] = std::move(sections);
    Json cells = Json::array();
    for (const auto& [key, file] : m.cells) cells.push_back({{"x", key.x}, {"z", key.z}, {"file", file}});
    j["cells"] = std::move(cells);
    return j.dump(1, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::optional<std::string> load_manifest(std::string_view text, Manifest& out) {
    Json j;
    try {
        j = Json::parse(text);
    } catch (const Json::parse_error& e) {
        return std::string("not JSON: ") + e.what();
    }
    if (!j.is_object()) return std::string("a manifest is an object");
    if (auto e = check_keys(j, {"format", "world", "saved_unix", "play_seconds", "generation", "sections", "cells"},
                            "the manifest"))
        return e;
    Manifest m;
    try {
        const std::optional<u64> format = j.contains("format") ? whole<u64>(j["format"], 0, ~u64{0}) : std::nullopt;
        if (!format) return std::string("no format");
        if (*format > k_format) return "format " + std::to_string(*format) + " is newer than this engine reads";
        if (*format < 1) return std::string("format 0 is not a save");
        m.format = static_cast<u32>(*format);
        const Json& w = j.at("world");
        if (!w.is_object()) return std::string("'world' is an object");
        if (auto e = check_keys(w, {"name", "hash"}, "'world'")) return e;
        m.world_name = w.at("name").get<std::string>();
        const std::string h = w.at("hash").get<std::string>();
        if (h.size() != 16) return std::string("the world's hash is 16 hex digits");
        const auto [end, error] = std::from_chars(h.data(), h.data() + h.size(), m.world_hash, 16);
        if (error != std::errc{} || end != h.data() + h.size()) return std::string("the world's hash is 16 hex digits");
        if (j.contains("saved_unix")) {
            const auto v = whole<i64>(j["saved_unix"], std::numeric_limits<i64>::min(),
                                      static_cast<u64>(std::numeric_limits<i64>::max()));
            if (!v) return std::string("saved_unix is a whole number");
            m.saved_unix = *v;
        }
        if (j.contains("play_seconds")) {
            if (!j["play_seconds"].is_number() || j["play_seconds"].is_boolean())
                return std::string("bad play_seconds");
            m.play_seconds = j["play_seconds"].get<double>();
        }
        if (!(m.play_seconds >= 0.0) || m.play_seconds > k_max_play_seconds) return std::string("bad play_seconds");
        const std::optional<u64> generation =
            j.contains("generation") ? whole<u64>(j["generation"], 0, ~u64{0}) : std::nullopt;
        if (!generation) return std::string("no generation");
        m.generation = *generation;
        const Json& sections = j.value("sections", Json::object());
        if (!sections.is_object()) return std::string("'sections' is an object");
        if (sections.size() > k_max_sections) return std::string("too many sections");
        for (auto it = sections.begin(); it != sections.end(); ++it) {
            if (!valid_name(it.key())) return "bad section name '" + it.key() + "'";
            const std::string file = it.value().get<std::string>();
            if (!section_file_ok(it.key(), file)) return "bad file '" + file + "' for section '" + it.key() + "'";
            m.sections[it.key()] = file;
        }
        const Json& cells = j.value("cells", Json::array());
        if (!cells.is_array()) return std::string("'cells' is an array");
        if (cells.size() > k_max_cells) return std::string("too many cells");
        for (const Json& c : cells) {
            if (!c.is_object()) return std::string("a cell is an object");
            if (auto e = check_keys(c, {"x", "z", "file"}, "a cell")) return e;
            const i64 lo = std::numeric_limits<i32>::min();
            const u64 hi = static_cast<u64>(std::numeric_limits<i32>::max());
            const std::optional<i32> x = whole<i32>(c.at("x"), lo, hi), z = whole<i32>(c.at("z"), lo, hi);
            if (!x || !z) return std::string("a cell's x and z are whole numbers an i32 holds");
            const CellKey key{*x, *z};
            const std::string file = c.at("file").get<std::string>();
            if (!cell_file_ok(key, file)) return "bad file '" + file + "' for a cell";
            if (!m.cells.emplace(key, file).second)
                return "cell " + std::to_string(key.x) + "," + std::to_string(key.z) + " is listed twice";
        }
    } catch (const Json::exception& e) {
        return std::string("bad manifest: ") + e.what();
    }
    out = std::move(m);
    return std::nullopt;
}

Opened open_slot(const fs::path& slot) {
    Opened out;
    std::string first_error;
    for (const char* name : {k_manifest, k_previous}) {
        const std::optional<std::string> text = read_text(slot / name, usize{256} << 20);
        if (!text) {
            if (first_error.empty()) first_error = std::string(name) + " is missing or unreadable";
            continue;
        }
        Manifest m;
        if (const auto e = load_manifest(*text, m)) {
            // A newer engine's save: never the one before in its place (a commit from that would undo it).
            if (format_of(*text) > k_format) {
                out.error = std::string(name) + ": " + *e;
                return out;
            }
            if (first_error.empty()) first_error = std::string(name) + ": " + *e;
            continue;
        }
        out.manifest = std::move(m);
        out.from_previous = name == std::string_view(k_previous);
        return out;
    }
    out.error = first_error;
    return out;
}

std::optional<std::string> read_chunk(const fs::path& slot, std::string_view file) {
    // Only what a manifest can name (no way out of the slot).
    std::string_view stem;
    u64 g = 0;
    const bool cell = file.substr(0, 6) == "cells/", section = file.substr(0, 9) == "sections/";
    if ((!cell && !section) || file.find("..") != std::string_view::npos || file.find('\\') != std::string_view::npos ||
        !split_generation(file.substr(cell ? 6 : 9), stem, g) || stem.find('/') != std::string_view::npos)
        return std::nullopt;
    return read_text(slot / fs::path(std::string(file)), k_max_chunk_bytes);
}

Committed commit(const fs::path& slot, const Manifest& base, const Changes& changes, const Meta& meta) {
    Committed out;
    out.manifest = base;
    std::error_code error;
    fs::create_directories(slot / "cells", error);
    if (!error) fs::create_directories(slot / "sections", error);
    if (error) {
        out.error = "cannot make the slot's folders: " + error.message();
        return out;
    }
    sync_folder(slot.parent_path()); // (a new slot's own entry)
    // What the slot holds now: the base must be it (or empty: a fresh save over it).
    std::optional<std::string> current_text = read_text(slot / k_manifest, usize{256} << 20);
    Manifest current;
    bool current_ok = current_text && !load_manifest(*current_text, current).has_value();
    if (current_text && !current_ok && format_of(*current_text) > k_format) {
        out.error = "the slot holds a newer engine's save: not written over";
        return out;
    }
    Manifest previous;
    const std::optional<std::string> previous_text = read_text(slot / k_previous, usize{256} << 20);
    const bool previous_ok = previous_text && !load_manifest(*previous_text, previous).has_value();
    if (!current_ok && previous_text && !previous_ok && format_of(*previous_text) > k_format) {
        out.error = "the slot holds a newer engine's save: not written over";
        return out;
    }
    const Manifest* on_disk = current_ok ? &current : previous_ok ? &previous : nullptr;
    const u64 disk_generation = on_disk ? on_disk->generation : 0;
    if (base.generation != 0 && base.generation != disk_generation) {
        out.error = "the slot changed since its base was opened (generation " + std::to_string(disk_generation) +
                    ", base " + std::to_string(base.generation) + ")";
        return out;
    }
    for (const auto& [name, text] : changes.sections) {
        if (!valid_name(name)) {
            out.error = "bad section name '" + name + "'";
            return out;
        }
        if (text.size() > k_max_chunk_bytes) {
            out.error = "section '" + name + "' is too big";
            return out;
        }
    }
    for (const auto& [key, text] : changes.cells) {
        if (text && text->size() > k_max_chunk_bytes) {
            out.error = "cell " + std::to_string(key.x) + "," + std::to_string(key.z) + " is too big";
            return out;
        }
    }
    // Past every generation a file in the slot is named for (one a crash left too): no name is reused.
    u64 highest = std::max(disk_generation, base.generation);
    for (const char* dir : {"cells", "sections"}) {
        for (fs::directory_iterator it(slot / dir, error), end; !error && it != end; it.increment(error)) {
            std::string_view stem;
            u64 g = 0;
            const std::string name = it->path().filename().string();
            if (split_generation(name, stem, g)) highest = std::max(highest, g);
        }
        error.clear();
    }
    if (highest == ~u64{0}) {
        out.error = "the slot's generations are spent";
        return out;
    }
    Manifest next = base.generation != 0 ? base : Manifest{};
    next.format = k_format;
    next.generation = highest + 1;
    next.world_name = meta.world_name;
    next.world_hash = meta.world_hash;
    next.saved_unix = meta.saved_unix;
    next.play_seconds =
        std::isfinite(meta.play_seconds) ? std::clamp(meta.play_seconds, 0.0, k_max_play_seconds) : 0.0;
    // What it carries over must be there.
    for (const auto& [key, file] : next.cells) {
        if (changes.cells.count(key) == 0 && !fs::is_regular_file(slot / file, error)) {
            out.error = "the base is not this slot's (missing " + file + ")";
            return out;
        }
    }
    for (const auto& [name, file] : next.sections) {
        if (changes.sections.count(name) == 0 && !fs::is_regular_file(slot / file, error)) {
            out.error = "the base is not this slot's (missing " + file + ")";
            return out;
        }
    }
    // The chunks' names, under the new generation's; the manifest as it will be read back, first.
    for (const auto& [name, text] : changes.sections) next.sections[name] = section_file(name, next.generation);
    for (const auto& [key, text] : changes.cells) {
        if (text) {
            next.cells[key] = cell_file(key, next.generation);
        } else {
            next.cells.erase(key);
        }
    }
    if (next.sections.size() > k_max_sections || next.cells.size() > k_max_cells) {
        out.error = "too many sections or cells";
        return out;
    }
    const std::string manifest_text = save_manifest(next);
    {
        Manifest back;
        if (const auto e = load_manifest(manifest_text, back)) {
            out.error = "the manifest would not read back: " + *e;
            return out;
        }
        next = std::move(back); // (a world name not UTF-8 is as it reads)
    }
    for (const auto& [name, text] : changes.sections) {
        const std::string& file = next.sections.at(name);
        if (!write_text(slot / file, text)) {
            out.error = "cannot write " + file;
            return out;
        }
        ++out.written;
    }
    for (const auto& [key, text] : changes.cells) {
        if (!text) continue;
        const std::string& file = next.cells.at(key);
        if (!write_text(slot / file, *text)) {
            out.error = "cannot write " + file;
            return out;
        }
        ++out.written;
    }
    sync_folder(slot / "cells");
    sync_folder(slot / "sections");
    // The old manifest kept as the one before (unless it is bad: then the one before stays).
    if (current_ok && !write_text(slot / k_previous, *current_text)) {
        out.error = "cannot write save.prev.json";
        return out;
    }
    const Manifest& kept_previous = current_ok ? current : previous;
    // The commit.
    if (!write_text(slot / k_manifest, manifest_text)) {
        out.error = "cannot write save.json";
        return out;
    }
    sync_folder(slot);
    out.manifest = next;
    // Cleaning: what neither this manifest nor the one before names.
    std::set<std::string> keep = files_of(next);
    if (current_ok || previous_ok) {
        const std::set<std::string> old = files_of(kept_previous);
        keep.insert(old.begin(), old.end());
    }
    // (One that cannot go now, a reader holding it, waits for the next commit; the rest still go.)
    for (const char* dir : {"cells", "sections"}) {
        for (fs::directory_iterator it(slot / dir, error), end; !error && it != end; it.increment(error)) {
            std::error_code each;
            const std::string rel = std::string(dir) + "/" + it->path().filename().string();
            if (keep.count(rel) == 0 && it->is_regular_file(each) && fs::remove(it->path(), each)) ++out.removed;
        }
        error.clear();
    }
    for (fs::directory_iterator it(slot, error), end; !error && it != end; it.increment(error)) {
        std::error_code each;
        const std::string name = it->path().filename().string();
        if (name.size() > 8 && name.substr(name.size() - 8) == ".partial" && it->is_regular_file(each) &&
            fs::remove(it->path(), each))
            ++out.removed;
    }
    return out;
}

std::vector<Listed> list_slots(const fs::path& root) {
    std::vector<Listed> out;
    std::error_code error;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (!valid_name(name) || !it->is_directory(error)) continue;
        out.push_back({name, open_slot(it->path())});
    }
    std::sort(out.begin(), out.end(), [](const Listed& a, const Listed& b) { return a.name < b.name; });
    return out;
}

bool delete_slot(const fs::path& root, std::string_view name) {
    if (!valid_name(name)) return false;
    std::error_code error;
    const fs::path dir = root / std::string(name);
    if (!fs::is_directory(dir, error)) return false;
    return fs::remove_all(dir, error) > 0 && !error;
}

} // namespace giant::save
