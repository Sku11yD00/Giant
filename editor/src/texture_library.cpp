#include "texture_library.h"

#include "mesh_library.h"

#include "giant/core/log.h"
#include "giant/geometry/gltf.h"
#include "giant/texture/texture.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>
#include <thread>

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

// Decodes an image file, or an image inside a glTF ("file.glb#image2"):
// false with the reason.
bool load_image(const std::string& path, texture::Image& out, std::string& error) {
    std::vector<u8> bytes;
    std::string file;
    u32 index = 0;
    if (TextureLibrary::split_image_ref(path, file, index)) {
        if (!geometry::read_gltf_image(from_utf8(file), index, bytes, error)) return false;
    } else if (!read_bytes(from_utf8(path), bytes, error)) {
        return false;
    }
    return texture::decode_image(bytes, out, error);
}

constexpr u64 k_fnv_basis = 1469598103934665603ull;
constexpr u64 k_fnv_prime = 1099511628211ull;

u64 hash_u64(u64 h, u64 v) {
    for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (i * 8)) & 0xFF)) * k_fnv_prime;
    return h;
}

u64 hash_text(u64 h, std::string_view text) {
    for (const char c : text) h = (h ^ static_cast<u8>(c)) * k_fnv_prime;
    return (h ^ 0xFF) * k_fnv_prime; // ends each string, so "ab"+"c" is not "a"+"bc"
}

// The file's size and change time, hashed in (so an edited file cooks
// again); for an image inside a glTF, the glTF's and its buffer files'
// (a .gltf's images can live in its .bin).
u64 hash_one_file(u64 h, const std::filesystem::path& p, bool& ok) {
    std::error_code ec;
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
    h = hash_u64(h, static_cast<u64>(size));
    return hash_u64(h, static_cast<u64>(time.time_since_epoch().count()));
}

u64 hash_file_state(u64 h, const std::string& path, bool& ok) {
    std::string file;
    u32 index = 0;
    if (!TextureLibrary::split_image_ref(path, file, index)) return hash_one_file(h, from_utf8(path), ok);
    h = hash_one_file(h, from_utf8(file), ok);
    if (!ok) return h;
    std::string lower = file;
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (lower.ends_with(".glb")) return h; // (its images are in its own chunk; reading it all would cost)
    for (const std::filesystem::path& buffer : geometry::gltf_buffer_files(from_utf8(file))) {
        bool buffer_ok = true;
        h = hash_one_file(h, buffer, buffer_ok);
        if (!buffer_ok) h = hash_u64(h, 0); // missing: the cook fails, and says why
    }
    return h;
}

std::string hex(u64 v) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
    return text;
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
    std::filesystem::path file; // the .gtex it is in (read from, or written to), if any
    bool from_cache = false;
    float seconds = 0.0f;
};

// Finer levels read on a worker, waiting for update() to stream them.
struct TextureLibrary::LevelsDone {
    Ref ref = k_no_ref;
    u32 generation = 0;
    render::TextureId id = render::k_no_texture;
    u32 from_mip = 0; // the first level on the GPU when asked
    u32 to_mip = 0;   // the finest read
    std::vector<u8> bytes;
    bool ok = false;
    std::string error;
};

bool TextureLibrary::split_image_ref(std::string_view path, std::string& file, u32& image) {
    const usize hash = path.rfind("#image");
    if (hash == std::string_view::npos || hash == 0) return false;
    const std::string_view digits = path.substr(hash + 6);
    if (digits.empty() || digits.size() > 9 || digits.find_first_not_of("0123456789") != std::string_view::npos) {
        return false;
    }
    file.assign(path.substr(0, hash));
    image = 0;
    for (const char c : digits) image = image * 10 + static_cast<u32>(c - '0');
    return true;
}

const char* TextureLibrary::to_string(Slot slot) {
    switch (slot) {
    case Slot::base_color: return "base_color";
    case Slot::normal: return "normal";
    case Slot::roughness_metal: return "roughness_metal";
    case Slot::occlusion: return "occlusion";
    }
    return "base_color";
}

const char* TextureLibrary::to_string(Status status) {
    switch (status) {
    case Status::cooking: return "cooking";
    case Status::ready: return "ready";
    case Status::failed: return "failed";
    case Status::unloaded: return "unloaded";
    }
    return "cooking";
}

usize TextureLibrary::KeyHash::operator()(const Key& k) const {
    u64 h = hash_text(k_fnv_basis, k.path);
    h = hash_text(h, k.normal);
    h = (h ^ static_cast<u64>(k.slot)) * k_fnv_prime;
    h = (h ^ (k.green_down ? 1u : 0u)) * k_fnv_prime;
    return static_cast<usize>(h);
}

TextureLibrary::TextureLibrary(render::Renderer* renderer, std::filesystem::path cache, u32 workers)
    : m_renderer(renderer), m_cache(std::move(cache)), m_workers(std::max(workers, 1u)),
      m_jobs(std::make_unique<JobSystem>(m_workers)) {}

TextureLibrary::~TextureLibrary() {
    m_stopping.store(true, std::memory_order_relaxed); // jobs not yet begun return at once
    m_jobs->wait(m_group);
    if (m_renderer != nullptr) {
        for (const Entry& e : m_entries) m_renderer->remove_texture(e.id);
    }
}

std::filesystem::path TextureLibrary::cache_file(const Key& key) const {
    return cache_file(key, max_size);
}

std::filesystem::path TextureLibrary::cache_file(const Key& key, u32 max_side) const {
    if (m_cache.empty()) return {};
    bool ok = true;
    // The key (with the cooker's file version) names the texture; the
    // files as they are now, its version.
    const u64 named = hash_u64(static_cast<u64>(KeyHash{}(key)), texture::k_file_version);
    u64 state = hash_u64(k_fnv_basis, max_side); // a cook capped smaller is another version
    state = hash_file_state(state, key.path, ok);
    if (key.slot == Slot::roughness_metal && !key.normal.empty()) {
        bool normal_ok = true;
        state = hash_file_state(state, key.normal, normal_ok);
        state = hash_u64(state, normal_ok ? 1u : 2u); // a missing normal map cooks without one
    }
    if (!ok) return {};
    return m_cache / (hex(named) + "-" + hex(state) + ".gtex");
}

TextureLibrary::Ref TextureLibrary::request(const Key& given) {
    if (given.path.empty()) return k_no_ref;
    auto it = m_lookup.find(given);
    if (it == m_lookup.end()) {
        // Another spelling of a known path, or new: kept by its normal spelling.
        Key key = given;
        key.path = MeshLibrary::normalize(given.path);
        if (!key.normal.empty()) key.normal = MeshLibrary::normalize(given.normal);
        auto known = m_lookup.find(key);
        if (known == m_lookup.end()) {
            known = m_lookup.emplace(key, static_cast<Ref>(m_entries.size())).first;
            Entry e;
            e.key = key;
            m_entries.push_back(std::move(e));
            queue(known->second);
        }
        it = m_lookup.emplace(given, known->second).first;
    }
    Entry& e = m_entries[it->second];
    if (!e.used) {
        e.used = true;
    }
    if (e.status == Status::unloaded) {
        e.status = Status::cooking;
        queue(it->second);
    }
    return it->second;
}

std::array<TextureLibrary::Ref, 4> TextureLibrary::request(const world::MaterialMaps& maps) {
    const bool green_down = maps.normal_green_down && !maps.normal.empty();
    return {request({maps.base_color, Slot::base_color, false, {}}),
            request({maps.normal, Slot::normal, green_down, {}}),
            request({maps.roughness_metal, Slot::roughness_metal, green_down, maps.normal}),
            request({maps.occlusion, Slot::occlusion, false, {}})};
}

render::TextureId TextureLibrary::id(Ref ref) const {
    return ref < m_entries.size() ? m_entries[ref].id : render::k_no_texture;
}

const TextureLibrary::Entry* TextureLibrary::entry(Ref ref) const {
    return ref < m_entries.size() ? &m_entries[ref] : nullptr;
}

usize TextureLibrary::pending() const {
    return m_waiting_to_cook.size() + m_running + m_to_upload.size() + m_reading;
}

void TextureLibrary::set_used(std::span<const Ref> refs) {
    std::vector<bool> used(m_entries.size(), false);
    for (const Ref ref : refs) {
        if (ref < used.size()) used[ref] = true;
    }
    for (Ref ref = 0; ref < m_entries.size(); ++ref) {
        Entry& e = m_entries[ref];
        if (e.used && !used[ref]) e.unused_since = m_updates;
        e.used = used[ref];
        if (e.used && e.status == Status::unloaded) {
            e.status = Status::cooking;
            queue(ref);
        }
    }
}

void TextureLibrary::queue(Ref ref) {
    Entry& e = m_entries[ref];
    ++e.generation; // the answer to this request (an older one still cooking is dropped)
    if (!e.queued) {
        e.queued = true;
        m_waiting_to_cook.push_back(ref);
    }
    start_waiting();
}

void TextureLibrary::start_waiting() {
    // At most one cook a worker at once: each holds its image, mips and
    // blocks only while it runs, and finished ones show while others wait.
    // A key already cooking waits for that cook (two would race over its
    // cache file).
    for (usize i = 0; i < m_waiting_to_cook.size() && m_running < m_workers;) {
        const Ref ref = m_waiting_to_cook[i];
        Entry& e = m_entries[ref];
        if (e.cooking > 0) {
            ++i;
            continue;
        }
        m_waiting_to_cook.erase(m_waiting_to_cook.begin() + static_cast<std::ptrdiff_t>(i));
        e.queued = false;
        ++e.cooking;
        ++m_running;
        m_jobs->submit(m_group, [this, key = e.key, ref, generation = e.generation, side = max_size] {
            cook(key, ref, generation, side);
        });
    }
}

// On a worker: the texture from the cache, else decoded and cooked (and
// cached). Always answers with a Done (unless closing): a failure, or even
// an exception, is a failed one.
void TextureLibrary::cook(Key key, Ref ref, u32 generation, u32 max_side) {
    if (m_stopping.load(std::memory_order_relaxed)) return; // closing: nobody will take it
    std::unique_ptr<Done> done;
    try {
        done = std::make_unique<Done>();
        cook_into(key, max_side, *done);
    } catch (const std::exception& e) {
        if (done == nullptr) done = std::make_unique<Done>(); // (if this throws too, JobSystem reports it)
        done->ok = false;
        done->error = key.path + ": " + e.what();
    } catch (...) {
        if (done == nullptr) done = std::make_unique<Done>();
        done->ok = false;
        done->error = key.path + ": cooking failed";
    }
    done->ref = ref;
    done->generation = generation;
    const std::lock_guard lock(m_mutex);
    m_done.push_back(std::move(done));
}

void TextureLibrary::cook_into(const Key& key, u32 max_side, Done& result) {
    const auto begin = std::chrono::steady_clock::now();
    Done* done = &result;
    {
        const texture::Kind kind = kind_of(key.slot);
        const std::filesystem::path cached = cache_file(key, max_side); // (max_size may change meanwhile)
        if (!cached.empty()) {
            std::vector<u8> bytes;
            std::string ignored;
            std::error_code ec;
            if (std::filesystem::is_regular_file(cached, ec) && read_bytes(cached, bytes, ignored) &&
                texture::read_file(bytes, done->cooked, ignored) && done->cooked.kind == kind) {
                done->ok = true;
                done->from_cache = true;
                done->file = cached;
            }
        }
        if (!done->ok) {
            texture::Image image;
            texture::Image normal;
            texture::CookOptions options;
            options.kind = kind;
            options.flip_green = key.green_down;
            options.mask_channel = 0; // glTF occlusion is R
            options.max_size = max_side;
            // Encoded on this worker: handing the library's own workers to
            // the encoder would let them start other cooks inside this one.
            options.jobs = nullptr;
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
            // Cached only when whole (one cooked without its normal map
            // cooks again next time, and says why again).
            if (done->ok && done->warning.empty() && !cached.empty() && !m_stopping.load(std::memory_order_relaxed)) {
                const std::vector<u8> file = texture::write_file(done->cooked);
                std::error_code ec;
                std::filesystem::create_directories(m_cache, ec);
                // Written beside under a name no other thread or editor
                // uses, then renamed: a reader never sees half a file.
                const u64 unique = hash_u64(hash_u64(k_fnv_basis, std::hash<std::thread::id>{}(std::this_thread::get_id())),
                                            static_cast<u64>(begin.time_since_epoch().count()));
                std::filesystem::path temp = cached;
                temp += "." + hex(unique) + ".tmp";
                bool written = false;
                {
                    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
                    written = !file.empty() && static_cast<bool>(out);
                }
                if (written) std::filesystem::rename(temp, cached, ec);
                if (!written || ec) {
                    std::filesystem::remove(temp, ec);
                } else {
                    done->file = cached;
                    // The key's older versions (the file since edited) go.
                    const std::string prefix = cached.filename().string().substr(0, 17); // "<key hash>-"
                    std::filesystem::directory_iterator it(m_cache, ec), end;
                    for (; !ec && it != end; it.increment(ec)) {
                        const std::string name = it->path().filename().string();
                        if (name.size() == cached.filename().string().size() && name.starts_with(prefix) &&
                            name.ends_with(".gtex") && it->path() != cached) {
                            std::error_code ignored_ec;
                            std::filesystem::remove(it->path(), ignored_ec);
                        }
                    }
                }
            }
        }
    }
    done->seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - begin).count();
}

void TextureLibrary::take_done() {
    std::vector<std::unique_ptr<Done>> done;
    {
        const std::lock_guard lock(m_mutex);
        done.swap(m_done);
    }
    for (auto& d : done) {
        --m_running;
        Entry& e = m_entries[d->ref];
        --e.cooking;
        // Asked for again since (reload): the newer answer counts.
        if (d->generation == e.generation) {
            ++e.uploading;
            m_to_upload.push_back(std::move(d));
        }
    }
    start_waiting();
}

u32 TextureLibrary::upload_first(const Done& done, bool whole) const {
    u32 first = whole ? 0 : tail_of(done.cooked.desc);
    const Entry& e = m_entries[done.ref];
    if (m_renderer != nullptr && e.id != render::k_no_texture) {
        // A new version of a drawn texture: as sharp as the one it replaces
        // (a reload does not blur the screen).
        if (const auto r = m_renderer->texture_residency(e.id)) first = std::min(first, r->first_mip);
    }
    return first;
}

u32 TextureLibrary::tail_of(const rhi::TextureDesc& desc) const {
    u32 tail = 0;
    const u32 coarsest = render::coarsest_first_mip(desc);
    while (tail < coarsest && std::max(desc.width, desc.height) >> tail > std::max(tail_size, 1u)) ++tail;
    return tail;
}

void TextureLibrary::upload(Done& done, bool whole) {
    Entry& e = m_entries[done.ref];
    if (done.generation != e.generation) return; // asked for again while it waited to upload
    const std::string name = e.key.path + " (" + to_string(e.key.slot) + ")";
    const rhi::TextureDesc& desc = done.cooked.desc;
    const u32 tail = tail_of(desc);
    const u32 first = upload_first(done, whole);
    render::TextureId id = render::k_no_texture;
    if (done.ok && m_renderer != nullptr) {
        const std::span<const u8> all(done.cooked.contents);
        const u64 skip = rhi::level_offset(desc, first);
        id = skip <= all.size() ? m_renderer->add_texture(desc, all.subspan(static_cast<usize>(skip)), first)
                                : render::k_no_texture;
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
    e.width = desc.width;
    e.height = desc.height;
    e.mips = desc.mips;
    e.full_bytes = done.cooked.contents.size();
    e.bytes = m_renderer != nullptr ? done.cooked.contents.size() - rhi::level_offset(desc, first) : e.full_bytes;
    e.first_mip = m_renderer != nullptr ? first : 0;
    e.tail_mip = tail;
    e.wanted_mip = render::TextureResidency::k_not_seen;
    e.reading = false; // (a read for the texture it replaces is dropped)
    e.coarser_since = 0;
    e.desc = desc;
    e.desc.name = "material texture";
    e.file = done.file;
    // Without a file its levels stay in memory (only while they may stream).
    e.contents.reset();
    if (e.file.empty() && m_renderer != nullptr && tail > 0) {
        e.contents = std::make_shared<const std::vector<u8>>(std::move(done.cooked.contents));
    }
    e.from_cache = done.from_cache;
    e.seconds = done.seconds;
    if (!e.used) e.unused_since = m_updates; // its wait to unload starts now
    ++m_version;
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.2f", static_cast<double>(done.seconds));
    log_info("editor", std::string(done.from_cache ? "loaded cooked texture " : "cooked texture ") + name + ": " +
                           std::to_string(e.width) + "x" + std::to_string(e.height) + ", " +
                           std::to_string(e.mips) + " mips (" + seconds + " s)");
    if (!e.warning.empty()) log_warning("editor", "texture " + name + ": " + e.warning);
}

void TextureLibrary::take_levels() {
    std::vector<std::unique_ptr<LevelsDone>> done;
    {
        const std::lock_guard lock(m_mutex);
        done.swap(m_levels_done);
    }
    for (auto& d : done) {
        --m_reading;
        Entry& e = m_entries[d->ref];
        if (d->id == e.id) e.reading = false; // (also when asked for again: a failed cook keeps this id)
        if (d->generation != e.generation || d->id != e.id || e.id == render::k_no_texture) {
            continue; // cooked again, replaced or unloaded meanwhile
        }
        if (!d->ok) {
            // Its .gtex changed or went (another editor, a cleared cache):
            // cooked again; it draws what it has meanwhile.
            log_warning("editor", "texture " + e.key.path + " (" + to_string(e.key.slot) +
                                      "): finer levels could not be read (" + d->error + "); cooking it again");
            e.file.clear(); // no more reads until cooked again
            queue(d->ref);
            continue;
        }
        const std::optional<render::TextureResidency> r = m_renderer->texture_residency(e.id);
        if (!r || r->first_mip != d->from_mip || r->target_mip != r->first_mip) {
            continue; // its levels changed meanwhile: asked again by stream()
        }
        m_renderer->stream_texture(e.id, d->to_mip, std::move(d->bytes));
    }
}

void TextureLibrary::stream() {
    if (m_renderer == nullptr) return;
    struct Finer {
        Ref ref;
        u32 from;
        u32 to;
        u32 gap;
    };
    struct Coarser {
        Ref ref;
        u32 to;
        u64 freed;
    };
    std::vector<Finer> finer;
    std::vector<Coarser> coarser;
    std::vector<Coarser> wanted; // finer than their tails and wanted so: trimmed only past the budget
    u64 resident = 0;
    for (Ref ref = 0; ref < m_entries.size(); ++ref) {
        Entry& e = m_entries[ref];
        if (e.status != Status::ready || e.id == render::k_no_texture) continue;
        // (Not while a newer version is on its way: it may replace the file.)
        const bool readable =
            (!e.file.empty() || e.contents != nullptr) && !e.queued && e.cooking == 0 && e.uploading == 0;
        const std::optional<render::TextureResidency> r = m_renderer->texture_residency(e.id);
        if (!r) continue;
        e.first_mip = r->first_mip;
        e.bytes = r->bytes; // (ids are unchanged: no new version)
        // Counted at the most it is heading for: levels on their way
        // (read, or handed to the renderer) count already.
        // A trim counts at its target: it lands in a few frames (behind
        // uploads under way), and counting it whole would trim more.
        u32 heading = r->target_mip;
        if (e.reading) heading = std::min(heading, e.read_to);
        resident += heading == r->first_mip ? r->bytes
                                            : rhi::texture_bytes(render::texture_from_mip(e.desc, heading));
        e.wanted_mip = wanted_override ? wanted_override(ref) : r->wanted_mip;
        // Never coarser than its tail; not drawn lately: its tail.
        const u32 desired = std::min(e.wanted_mip, e.tail_mip);
        const bool busy = e.reading || r->target_mip != r->first_mip;
        if (desired < e.first_mip) {
            e.coarser_since = 0;
            if (!busy && readable) finer.push_back({ref, e.first_mip, desired, e.first_mip - desired});
        } else if (desired > e.first_mip) {
            if (e.coarser_since == 0) e.coarser_since = m_updates;
            if (!busy) {
                const u64 kept = rhi::texture_bytes(render::texture_from_mip(e.desc, desired));
                coarser.push_back({ref, desired, r->bytes > kept ? r->bytes - kept : 0});
            }
        } else {
            e.coarser_since = 0;
        }
        if (!busy && desired <= e.first_mip && e.first_mip < e.tail_mip) {
            const u64 kept = rhi::texture_bytes(render::texture_from_mip(e.desc, e.first_mip + 1));
            wanted.push_back({ref, e.first_mip + 1, r->bytes > kept ? r->bytes - kept : 0});
        }
    }
    // Detail no longer wanted goes after a while, or at once (most bytes
    // first) while over the budget.
    std::sort(coarser.begin(), coarser.end(), [](const Coarser& a, const Coarser& b) { return a.freed > b.freed; });
    for (const Coarser& c : coarser) {
        Entry& e = m_entries[c.ref];
        if (resident > gpu_budget_bytes || m_updates - e.coarser_since >= trim_after_updates) {
            if (m_renderer->trim_texture(e.id, c.to)) {
                resident -= std::min(resident, c.freed);
                e.coarser_since = 0;
                e.trimmed_at = m_updates;
            }
        }
    }
    // Still over (the budget was lowered): the biggest textures lose their
    // finest level, wanted or not, until it fits.
    std::sort(wanted.begin(), wanted.end(), [](const Coarser& a, const Coarser& b) {
        return a.freed != b.freed ? a.freed > b.freed : a.ref < b.ref;
    });
    for (const Coarser& c : wanted) {
        if (resident <= gpu_budget_bytes) break;
        if (m_renderer->trim_texture(m_entries[c.ref].id, c.to)) {
            resident -= std::min(resident, c.freed);
            m_entries[c.ref].trimmed_at = m_updates;
        }
    }
    // Finer levels where wanted: the furthest from it first, within the
    // budget (coarser than wanted if that is what fits).
    std::sort(finer.begin(), finer.end(), [](const Finer& a, const Finer& b) {
        return a.gap != b.gap ? a.gap > b.gap : a.ref < b.ref;
    });
    for (const Finer& f : finer) {
        if (m_reading >= std::max(max_level_reads, 1u)) break;
        Entry& e = m_entries[f.ref];
        if (e.trimmed_at == m_updates) continue; // trimmed for the budget just now
        u32 to = f.to;
        const auto bytes = [&](u32 mip) { return rhi::level_offset(e.desc, f.from) - rhi::level_offset(e.desc, mip); };
        while (to < f.from && resident + bytes(to) > gpu_budget_bytes) ++to;
        if (to == f.from) continue; // nothing finer fits
        e.reading = true;
        e.read_to = to;
        ++m_reading;
        resident += bytes(to);
        m_jobs->submit(m_group, [this, ref = f.ref, generation = e.generation, id = e.id, from = f.from, to,
                                 desc = e.desc, file = e.file, contents = e.contents] {
            if (m_stopping.load(std::memory_order_relaxed)) return; // closing: nobody will take it
            auto done = std::make_unique<LevelsDone>();
            done->ref = ref;
            done->generation = generation;
            done->id = id;
            done->from_mip = from;
            done->to_mip = to;
            const u64 begin = rhi::level_offset(desc, to);
            const u64 end = rhi::level_offset(desc, from);
            try {
                if (contents != nullptr) {
                    if (end <= contents->size()) {
                        done->bytes.assign(contents->begin() + static_cast<std::ptrdiff_t>(begin),
                                           contents->begin() + static_cast<std::ptrdiff_t>(end));
                        done->ok = true;
                    } else {
                        done->error = "its levels are not all kept";
                    }
                } else {
                    // The .gtex as cooked: its header, then the contents.
                    std::error_code ec;
                    const auto size = std::filesystem::file_size(file, ec);
                    std::ifstream in(file, std::ios::binary);
                    if (ec || !in || size != texture::k_file_header_bytes + rhi::texture_bytes(desc)) {
                        done->error = "its cooked file changed or is gone";
                    } else {
                        done->bytes.resize(static_cast<usize>(end - begin));
                        in.seekg(static_cast<std::streamoff>(texture::k_file_header_bytes + begin));
                        in.read(reinterpret_cast<char*>(done->bytes.data()),
                                static_cast<std::streamsize>(done->bytes.size()));
                        done->ok = static_cast<bool>(in);
                        if (!done->ok) done->error = "its cooked file is cut short";
                    }
                }
            } catch (const std::exception& ex) {
                done->ok = false;
                done->error = ex.what();
            }
            if (!done->ok) done->bytes.clear();
            const std::lock_guard lock(m_mutex);
            m_levels_done.push_back(std::move(done));
        });
    }
}

void TextureLibrary::update() {
    ++m_updates;
    take_done();
    u64 spent = 0;
    usize taken = 0;
    for (; taken < m_to_upload.size(); ++taken) {
        Done& d = *m_to_upload[taken];
        const bool current = d.generation == m_entries[d.ref].generation;
        // Charged what goes up: its tail, as sharp as a version it
        // replaces, or all of it when whole.
        const u64 bytes = !current || !d.ok ? 0
                                            : rhi::texture_bytes(render::texture_from_mip(
                                                  d.cooked.desc, upload_first(d, m_whole)));
        if (spent > 0 && spent + bytes > upload_bytes_per_update) break; // next frame
        spent += bytes;
        --m_entries[d.ref].uploading;
        if (current) upload(d, m_whole);
    }
    m_to_upload.erase(m_to_upload.begin(), m_to_upload.begin() + static_cast<std::ptrdiff_t>(taken));
    take_levels();
    stream();
    // Textures no drawn material has used for a while leave the GPU.
    for (Entry& e : m_entries) {
        if (!e.used && e.status == Status::ready && !e.queued && e.cooking == 0 && e.uploading == 0 &&
            m_updates - e.unused_since >= unload_after_updates) {
            if (m_renderer != nullptr) m_renderer->remove_texture(e.id);
            e.id = render::k_no_texture;
            e.status = Status::unloaded;
            e.bytes = 0;
            e.contents.reset();
            e.reading = false; // (a read under way is dropped: the id is gone)
            ++m_version;
        }
    }
}

void TextureLibrary::reload() {
    for (Ref ref = 0; ref < m_entries.size(); ++ref) {
        Entry& e = m_entries[ref];
        if (e.status == Status::unloaded || (e.status == Status::ready && !e.used)) {
            continue; // loads from its file when used again (an unused one is on its way out)
        }
        if (e.status == Status::failed) e.status = Status::cooking;
        e.error.clear();
        queue(ref);
    }
    ++m_version;
}

void TextureLibrary::wait_cooked() {
    while (m_running > 0 || !m_waiting_to_cook.empty() || m_reading > 0) {
        m_jobs->wait(m_group); // cooks and level reads both land meanwhile
        take_done();           // starts what waits
        take_levels();         // (a failed read queues a cook again)
    }
}

void TextureLibrary::finish() {
    // (Again while a read found its file gone and cooked it again.)
    for (int pass = 0; pass < 4; ++pass) {
        wait_cooked();
        const u64 budget = upload_bytes_per_update;
        upload_bytes_per_update = ~u64{0};
        m_whole = true;
        update();
        m_whole = false;
        upload_bytes_per_update = budget;
        // Reads update() started: taken (dropped by make_whole's new ids).
        while (m_reading > 0) {
            m_jobs->wait(m_group);
            take_levels();
        }
        // Textures already up from their tails: put up whole now.
        for (Ref ref = 0; ref < m_entries.size(); ++ref) make_whole(ref);
        if (pending() == 0) break;
    }
}

void TextureLibrary::make_whole(Ref ref) {
    Entry& e = m_entries[ref];
    if (m_renderer == nullptr || e.status != Status::ready || e.id == render::k_no_texture) return;
    const std::optional<render::TextureResidency> r = m_renderer->texture_residency(e.id);
    if (!r || (r->first_mip == 0 && r->target_mip == 0)) return;
    const u64 size = rhi::texture_bytes(e.desc);
    std::vector<u8> read;
    std::span<const u8> all;
    if (e.contents != nullptr && e.contents->size() == size) {
        all = *e.contents;
    } else if (!e.file.empty()) {
        std::error_code ec;
        const auto file_size = std::filesystem::file_size(e.file, ec);
        std::ifstream in(e.file, std::ios::binary);
        if (!ec && in && file_size == texture::k_file_header_bytes + size) {
            read.resize(static_cast<usize>(size));
            in.seekg(static_cast<std::streamoff>(texture::k_file_header_bytes));
            in.read(reinterpret_cast<char*>(read.data()), static_cast<std::streamsize>(read.size()));
            if (in) all = read;
        }
    }
    const render::TextureId id = all.empty() ? render::k_no_texture : m_renderer->add_texture(e.desc, all, 0);
    if (id == render::k_no_texture) {
        log_warning("editor", "texture " + e.key.path + " (" + to_string(e.key.slot) +
                                  "): could not be put up whole (its cooked file changed or is gone)");
        return;
    }
    m_renderer->remove_texture(e.id); // frames in flight finish with it
    e.id = id;
    e.first_mip = 0;
    e.bytes = size;
    e.reading = false; // (a read for the old id is dropped)
    e.wanted_mip = render::TextureResidency::k_not_seen;
    e.coarser_since = m_updates; // kept sharp at least trim_after_updates
    ++m_version;
}

} // namespace giant::editor
