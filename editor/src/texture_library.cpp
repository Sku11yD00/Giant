#include "texture_library.h"

#include "giant/core/log.h"
#include "giant/texture/texture.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace giant::editor {

namespace {

std::filesystem::path from_utf8(std::string_view path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

texture::Kind kind_of(TextureLibrary::Slot slot) {
    switch (slot) {
    case TextureLibrary::Slot::base_color: return texture::Kind::color;
    case TextureLibrary::Slot::normal: return texture::Kind::normal;
    case TextureLibrary::Slot::roughness_metal: return texture::Kind::roughness_metal;
    case TextureLibrary::Slot::occlusion: return texture::Kind::mask;
    }
    return texture::Kind::color;
}

bool read_bytes(const std::filesystem::path& path, std::vector<u8>& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open the file";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) {
        error = "cannot read the file";
        return false;
    }
    return true;
}

// Decodes an image file: false with the reason.
bool load_image(const std::string& path, texture::Image& out, std::string& error) {
    std::vector<u8> bytes;
    if (!read_bytes(from_utf8(path), bytes, error)) return false;
    return texture::decode_image(bytes, out, error);
}

// The file's size and change time, hashed in (so an edited file cooks again).
u64 hash_file_state(u64 h, const std::string& path, bool& ok) {
    std::error_code ec;
    const std::filesystem::path p = from_utf8(path);
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) {
        ok = false;
        return h;
    }
    const auto time = std::filesystem::last_write_time(p, ec);
    if (ec) {
        ok = false;
        return h;
    }
    const u64 values[2] = {static_cast<u64>(size), static_cast<u64>(time.time_since_epoch().count())};
    for (const u64 v : values) {
        for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (i * 8)) & 0xFF)) * 1099511628211ull;
    }
    return h;
}

u64 hash_bytes(u64 h, std::string_view text) {
    for (const char c : text) h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
    return (h ^ 0xFF) * 1099511628211ull; // ends each string, so "ab"+"c" is not "a"+"bc"
}

} // namespace

// A finished job, waiting for update() to take it.
struct TextureLibrary::Done {
    Ref ref = k_no_ref;
    u32 generation = 0;
    bool ok = false;
    std::string error;
    std::string warning;
    texture::Cooked cooked;
    bool from_cache = false;
    float seconds = 0.0f;
};

const char* TextureLibrary::to_string(Slot slot) {
    switch (slot) {
    case Slot::base_color: return "base_color";
    case Slot::normal: return "normal";
    case Slot::roughness_metal: return "roughness_metal";
    case Slot::occlusion: return "occlusion";
    }
    return "base_color";
}

usize TextureLibrary::KeyHash::operator()(const Key& k) const {
    u64 h = hash_bytes(1469598103934665603ull, k.path);
    h = hash_bytes(h, k.normal);
    h = (h ^ static_cast<u64>(k.slot)) * 1099511628211ull;
    h = (h ^ (k.green_down ? 1u : 0u)) * 1099511628211ull;
    return static_cast<usize>(h);
}

TextureLibrary::TextureLibrary(render::Renderer* renderer, std::filesystem::path cache, u32 workers)
    : m_renderer(renderer), m_cache(std::move(cache)), m_jobs(std::make_unique<JobSystem>(std::max(workers, 1u))) {}

TextureLibrary::~TextureLibrary() {
    m_jobs->wait(m_group);
    if (m_renderer != nullptr) {
        for (const Entry& e : m_entries) m_renderer->remove_texture(e.id);
    }
}

std::filesystem::path TextureLibrary::cache_file(const Key& key) const {
    if (m_cache.empty()) return {};
    bool ok = true;
    // The cooker's file version and the key, then the files as they are now.
    u64 h = hash_bytes(1469598103934665603ull, "gtex " + std::to_string(texture::k_file_version));
    h = static_cast<u64>(KeyHash{}(key)) ^ (h * 31);
    h = hash_file_state(h, key.path, ok);
    if (key.slot == Slot::roughness_metal && !key.normal.empty()) {
        bool normal_ok = true;
        h = hash_file_state(h, key.normal, normal_ok);
        h = (h ^ (normal_ok ? 1u : 2u)) * 1099511628211ull; // a missing normal map cooks without one
    }
    if (!ok) return {};
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.gtex", static_cast<unsigned long long>(h));
    return m_cache / name;
}

TextureLibrary::Ref TextureLibrary::request(const Key& key) {
    if (key.path.empty()) return k_no_ref;
    const auto [it, added] = m_lookup.try_emplace(key, static_cast<Ref>(m_entries.size()));
    if (added) {
        Entry e;
        e.key = key;
        m_entries.push_back(std::move(e));
        start(it->second);
    }
    return it->second;
}

render::TextureId TextureLibrary::id(Ref ref) const {
    return ref < m_entries.size() ? m_entries[ref].id : render::k_no_texture;
}

const TextureLibrary::Entry* TextureLibrary::entry(Ref ref) const {
    return ref < m_entries.size() ? &m_entries[ref] : nullptr;
}

usize TextureLibrary::pending() const {
    return m_cooking;
}

void TextureLibrary::start(Ref ref) {
    Entry& e = m_entries[ref];
    ++e.generation;
    ++m_cooking;
    const Key key = e.key;
    const u32 generation = e.generation;
    m_jobs->submit(m_group, [this, key, ref, generation] {
        const auto begin = std::chrono::steady_clock::now();
        auto done = std::make_unique<Done>();
        done->ref = ref;
        done->generation = generation;
        const texture::Kind kind = kind_of(key.slot);
        const std::filesystem::path cached = cache_file(key);
        if (!cached.empty()) {
            std::vector<u8> bytes;
            std::string ignored;
            if (std::filesystem::exists(cached) && read_bytes(cached, bytes, ignored) &&
                texture::read_file(bytes, done->cooked, ignored) && done->cooked.kind == kind) {
                done->ok = true;
                done->from_cache = true;
            }
        }
        if (!done->ok) {
            texture::Image image;
            texture::Image normal;
            texture::CookOptions options;
            options.kind = kind;
            options.flip_green = key.green_down;
            options.mask_channel = 0; // glTF occlusion is R
            options.jobs = m_jobs.get();
            if (!load_image(key.path, image, done->error)) {
                done->error = key.path + ": " + done->error;
            } else {
                if (key.slot == Slot::roughness_metal && !key.normal.empty()) {
                    std::string why;
                    if (load_image(key.normal, normal, why)) {
                        options.normal = &normal;
                    } else {
                        done->warning = "normal map " + key.normal + " left out: " + why;
                    }
                }
                done->ok = texture::cook(image, options, done->cooked, done->error);
                if (!done->ok) done->error = key.path + ": " + done->error;
            }
            if (done->ok && !cached.empty()) {
                // Written beside, then renamed: a reader never sees half a file.
                static std::atomic<u64> serial{0};
                std::filesystem::path temp = cached;
                temp += ".tmp" + std::to_string(serial.fetch_add(1));
                const std::vector<u8> file = texture::write_file(done->cooked);
                std::error_code ec;
                std::filesystem::create_directories(m_cache, ec);
                bool written = false;
                {
                    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
                    written = !file.empty() && static_cast<bool>(out);
                }
                if (written) std::filesystem::rename(temp, cached, ec);
                if (!written || ec) std::filesystem::remove(temp, ec);
            }
        }
        done->seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - begin).count();
        const std::lock_guard lock(m_mutex);
        m_done.push_back(std::move(done));
    });
}

void TextureLibrary::upload(Done& done) {
    Entry& e = m_entries[done.ref];
    if (done.generation != e.generation) return; // asked again since (reload): the newer answer counts
    const std::string name = e.key.path + " (" + to_string(e.key.slot) + ")";
    render::TextureId id = render::k_no_texture;
    if (done.ok && m_renderer != nullptr) {
        id = m_renderer->add_texture(done.cooked.desc, done.cooked.contents);
        if (id == render::k_no_texture) {
            done.ok = false;
            done.error = e.key.path + ": the renderer refused the texture";
        }
    }
    if (!done.ok) {
        log_warning("editor", "texture " + name + " failed: " + done.error);
        e.error = done.error;
        e.seconds = done.seconds;
        if (e.status != Status::ready) e.status = Status::failed; // else it keeps its last good version
        ++m_version;
        return;
    }
    if (m_renderer != nullptr) m_renderer->remove_texture(e.id); // frames in flight finish with it
    e.id = id;
    e.status = Status::ready;
    e.error.clear();
    e.warning = std::move(done.warning);
    e.width = done.cooked.desc.width;
    e.height = done.cooked.desc.height;
    e.mips = done.cooked.desc.mips;
    e.bytes = done.cooked.contents.size();
    e.from_cache = done.from_cache;
    e.seconds = done.seconds;
    ++m_version;
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.2f", static_cast<double>(done.seconds));
    log_info("editor", std::string(done.from_cache ? "loaded cooked texture " : "cooked texture ") + name + ": " +
                           std::to_string(e.width) + "x" + std::to_string(e.height) + ", " +
                           std::to_string(e.mips) + " mips (" + seconds + " s)");
    if (!e.warning.empty()) log_warning("editor", "texture " + name + ": " + e.warning);
}

void TextureLibrary::update() {
    {
        const std::lock_guard lock(m_mutex);
        for (auto& d : m_done) m_waiting.push_back(std::move(d));
        m_done.clear();
    }
    u64 spent = 0;
    usize taken = 0;
    for (; taken < m_waiting.size(); ++taken) {
        Done& d = *m_waiting[taken];
        const u64 bytes = d.ok ? d.cooked.contents.size() : 0;
        if (taken > 0 && spent + bytes > upload_bytes_per_update) break; // next frame
        spent += bytes;
        upload(d);
        --m_cooking;
    }
    m_waiting.erase(m_waiting.begin(), m_waiting.begin() + static_cast<std::ptrdiff_t>(taken));
}

void TextureLibrary::reload() {
    for (Ref ref = 0; ref < m_entries.size(); ++ref) {
        if (m_entries[ref].status == Status::failed) m_entries[ref].status = Status::cooking;
        start(ref);
    }
    ++m_version;
}

void TextureLibrary::finish() {
    m_jobs->wait(m_group);
    const u64 budget = upload_bytes_per_update;
    upload_bytes_per_update = ~u64{0};
    update();
    upload_bytes_per_update = budget;
}

} // namespace giant::editor
