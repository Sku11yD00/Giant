// Material textures, by image file. Materials name image files
// (world::MaterialMaps); the library cooks each (giant/texture: mips, block
// compression) on its own worker threads the first time a material asks for
// it, keeps the result in a disk cache of .gtex files so the next session
// loads it straight back, and hands it to the renderer a few at a time
// (update(), once a frame, under a byte budget) so a scene full of new
// textures never stalls a frame.
//
// Until a texture is ready its materials draw their plain values. Like
// meshes, textures are assets, not scene changes: not undoable, kept for
// the session.
#pragma once

#include "giant/core/jobs.h"
#include "giant/core/types.h"
#include "giant/render/renderer.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace giant::editor {

class TextureLibrary {
public:
    // What a file is to a material, which decides how it is cooked.
    enum class Slot : u8 {
        base_color,      // sRGB colour, BC7
        normal,          // tangent-space normal map, BC5
        roughness_metal, // glTF metallic-roughness (G, B), BC5, widened by `normal`'s detail
        occlusion,       // glTF occlusion (R), BC4
    };
    static const char* to_string(Slot slot);

    // One texture: a file cooked for a slot. Paths as entities store them
    // (MeshLibrary::normalize).
    struct Key {
        std::string path;
        Slot slot = Slot::base_color;
        bool green_down = false; // normal and roughness_metal: the normal map is DirectX-authored
        std::string normal;      // roughness_metal: the material's normal map ("" for none)
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const;
    };

    // Stable for the session; k_no_ref for none.
    using Ref = u32;
    static constexpr Ref k_no_ref = ~0u;

    enum class Status : u8 { cooking, ready, failed };
    struct Entry {
        Key key;
        Status status = Status::cooking;
        std::string error;                         // failed: why
        std::string warning;                       // ready, but something was left out (a normal map that failed)
        render::TextureId id = render::k_no_texture; // ready, with a renderer
        u32 width = 0;                             // ready: its largest mip
        u32 height = 0;
        u32 mips = 0;
        u64 bytes = 0;              // ready: on the GPU
        bool from_cache = false;    // ready: read from the disk cache, not cooked
        float seconds = 0.0f;       // ready or failed: decoding, cooking or reading it
        u32 generation = 0;         // which request a finished job answers (reload asks again)
    };

    // Without a renderer (tests) textures are cooked and measured only.
    // `cache` is the folder for cooked files ("" for none: always cooked).
    TextureLibrary(render::Renderer* renderer, std::filesystem::path cache, u32 workers = 2);
    ~TextureLibrary(); // waits for cooking under way
    TextureLibrary(const TextureLibrary&) = delete;
    TextureLibrary& operator=(const TextureLibrary&) = delete;

    // The texture for this key, cooking it in the background if new. An
    // empty path is k_no_ref.
    Ref request(const Key& key);
    // Its renderer id once ready and uploaded; k_no_texture before (or on
    // failure, or without a renderer).
    render::TextureId id(Ref ref) const;
    const Entry* entry(Ref ref) const;
    const std::vector<Entry>& entries() const { return m_entries; }
    // Changes whenever a texture becomes ready (its id appears) or changes.
    u64 version() const { return m_version; }
    // Takes finished cooks and uploads them: at least one a call, then more
    // while within upload_bytes_per_update. Call once a frame from the
    // renderer's thread, between frames.
    void update();
    u64 upload_bytes_per_update = u64{48} << 20;
    // Cooks every texture again from its file (the files changed; the cache
    // gives back unchanged ones at once). A texture keeps drawing its old
    // version until the new one is uploaded; one that now fails keeps it.
    void reload();
    // Textures still cooking or waiting to upload.
    usize pending() const;
    // Blocks until nothing is cooking, then uploads everything (tests, and
    // a screenshot that must show textures).
    void finish();

    // The cache file a key's texture is kept in (under `cache`), named by a
    // hash of the key and its files' sizes and change times; empty if the
    // file cannot be read.
    std::filesystem::path cache_file(const Key& key) const;

private:
    struct Done;
    void start(Ref ref);
    void upload(Done& done);

    render::Renderer* m_renderer;
    std::filesystem::path m_cache;
    std::vector<Entry> m_entries;
    std::unordered_map<Key, Ref, KeyHash> m_lookup;
    u64 m_version = 0;
    usize m_cooking = 0; // jobs started, not taken back by update
    std::mutex m_mutex;  // protects m_done
    std::vector<std::unique_ptr<Done>> m_done;
    std::vector<std::unique_ptr<Done>> m_waiting; // taken, not yet uploaded (over the budget)
    WaitGroup m_group;
    std::unique_ptr<JobSystem> m_jobs; // last: destroyed first, running what is queued
};

} // namespace giant::editor
