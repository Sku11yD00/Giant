// Material textures, by image file. Materials name image files
// (world::MaterialMaps); the library cooks each (giant/texture: mips, block
// compression) on its own worker threads the first time a material asks for
// it, keeps the result in a disk cache of .gtex files so the next session
// loads it straight back, and hands it to the renderer a few at a time
// (update(), once a frame, under a byte budget) so a scene full of new
// textures does not stall a frame. Textures no drawn material has used for
// a while leave the GPU (asked for again, they come back from the cache).
//
// Streamed (photoreal part 3b-2): a texture goes to the GPU from its
// tail (sides up to tail_size), and its finer levels follow where drawn
// pixels want them (the renderer's feedback), read from its .gtex on a
// worker and uploaded by the renderer a row of blocks at a time; levels no
// longer wanted are dropped after a while, at once when the textures pass
// gpu_budget_bytes (still over: the biggest lose their finest level even if
// wanted), finer requests then waiting until they fit. Without a cache, a
// streamed texture's levels stay in memory to stream from.
//
// Until a texture is ready its materials draw their plain values. Like
// meshes, textures are assets, not scene changes: not undoable.
#pragma once

#include "giant/core/jobs.h"
#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "giant/texture/texture.h"
#include "giant/world/scene.h"

#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
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
    // (MeshLibrary::normalize; other spellings of a path find the same one).
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

    // unloaded: left the GPU unused (asked for again, it loads again).
    enum class Status : u8 { cooking, ready, failed, unloaded };
    static const char* to_string(Status status);
    struct Entry {
        Key key;
        Status status = Status::cooking;
        std::string error;                           // failed (or a reload that failed): why
        std::string warning;                         // ready, but something was left out (a normal map that failed)
        render::TextureId id = render::k_no_texture; // ready, with a renderer
        u32 width = 0;                               // ready: its largest mip
        u32 height = 0;
        u32 mips = 0;
        u64 bytes = 0;           // ready: on the GPU
        bool from_cache = false; // ready: read from the disk cache, not cooked
        // Ready base colour textures: their average colours and alpha
        // coverage by cutoff, for the voxel world (default: white, solid).
        texture::ColorSummary summary;
        float seconds = 0.0f;    // ready or failed: decoding, cooking or reading it
        u32 generation = 0;      // which request a finished job answers (reload asks again)
        bool queued = false;     // waiting to start cooking
        u32 cooking = 0;         // jobs cooking it (results not yet taken; one at a time)
        u32 uploading = 0;       // finished results waiting to upload
        bool used = true;        // by a drawn material (set_used); newly requested ones count as used
        u64 unused_since = 0;    // the update() count when it stopped being used
        // Streaming (ready, with a renderer). `bytes` above: its levels on
        // the GPU now.
        u32 first_mip = 0;       // the finest level on the GPU
        u32 tail_mip = 0;        // the finest kept with nothing wanting more (sides up to tail_size)
        u32 wanted_mip = render::TextureResidency::k_not_seen; // what drawn pixels wanted lately
        u64 full_bytes = 0;      // all its levels
        bool reading = false;    // finer levels being read (then uploaded by the renderer)
        u32 read_to = 0;         // the level a read under way brings it to
        u64 trimmed_at = 0;      // the update() that last trimmed it (no read in the same one)
        u64 coarser_since = 0;   // the update() since which it has had more than wanted (0: not)
        // Where its levels come from: its .gtex (read from the file again),
        // or kept in memory when it has none (no cache, or cooked with a
        // warning).
        rhi::TextureDesc desc;
        std::filesystem::path file;
        std::shared_ptr<const std::vector<u8>> contents;
    };

    // Without a renderer (tests) textures are cooked and measured only.
    // `cache` is the folder for cooked files ("" for none: always cooked).
    TextureLibrary(render::Renderer* renderer, std::filesystem::path cache, u32 workers = 2);
    ~TextureLibrary(); // cooking not yet started is dropped; what is under way finishes
    TextureLibrary(const TextureLibrary&) = delete;
    TextureLibrary& operator=(const TextureLibrary&) = delete;

    // The texture for this key, cooking it in the background if new (or
    // loading it again if it was unloaded). An empty path is k_no_ref.
    Ref request(const Key& key);
    // A material's four maps, as a mesh drawing it asks for them: base
    // colour, normal, roughness-metal (with the normal map), occlusion.
    std::array<Ref, 4> request(const world::MaterialMaps& maps);
    // Its renderer id once ready and uploaded; k_no_texture before (or on
    // failure, once unloaded, or without a renderer).
    render::TextureId id(Ref ref) const;
    const Entry* entry(Ref ref) const;
    const std::vector<Entry>& entries() const { return m_entries; }
    // Changes whenever a texture's id appears, changes or goes.
    u64 version() const { return m_version; }
    // The textures drawn materials use now (all others count as unused from
    // now on); unloaded ones among them load again.
    void set_used(std::span<const Ref> refs);
    // Takes finished cooks and uploads their tails: at least one a call,
    // then more while within upload_bytes_per_update; starts waiting cooks
    // (at most `workers` at once, so each holds its memory only while it
    // runs); streams levels in and out (above); and unloads textures unused
    // for unload_after_updates calls. Call once a frame from the
    // renderer's thread, before Renderer::render.
    void update();
    u64 upload_bytes_per_update = u64{16} << 20; // tails a call (at least one)
    u64 unload_after_updates = 600;               // about 10 s at 60 fps
    // 4096 a side (about 22 MiB as BC7, 5.3 MiB at 2048) keeps a cook's
    // memory (two at once) in hand; streaming no longer needs the cap.
    u32 max_size = 4096;
    u32 tail_size = 256;                    // largest side uploaded at first
    u64 gpu_budget_bytes = u64{1} << 30;    // all textures' levels on the GPU
    u64 trim_after_updates = 120;           // more detail than wanted this long: dropped
    u32 max_level_reads = 4;                // finer levels read at once
    // Tests: the level a texture is wanted at, instead of the GPU's
    // feedback (render::TextureResidency::k_not_seen: not drawn).
    std::function<u32(Ref)> wanted_override;
    // Cooks every texture again from its file (the files changed; the cache
    // gives back unchanged ones at once). A texture keeps drawing its old
    // version until the new one is uploaded; one that now fails keeps it.
    void reload();
    // Textures waiting to cook, cooking, waiting to upload, or having
    // finer levels read.
    usize pending() const;
    // Waits until nothing is waiting, cooking or being read (starting what
    // waits), without uploading (tests).
    void wait_cooked();
    // wait_cooked, then uploads new cooks whole and puts every streamed
    // texture up whole at once, read on this thread (tests, and a
    // screenshot that must show textures sharp; levels not wanted go again
    // after trim_after_updates, or at once over gpu_budget_bytes).
    void finish();

    // The cache file a key's texture is kept in (under `cache`): named by a
    // hash of the key, then one of its files' sizes and change times (an
    // edited file cooks again; the older file of the key is deleted);
    // empty if the file cannot be read.
    std::filesystem::path cache_file(const Key& key) const;

    // An image inside a glTF, as a map names it: "path/to/file.glb#image2"
    // (the file's image 2; any name, as glTFs can be .vrm and the like).
    // False for a plain path.
    static bool split_image_ref(std::string_view path, std::string& file, u32& image);

private:
    struct Done;
    struct LevelsDone;
    void queue(Ref ref);
    void stream();
    void take_levels();
    void start_waiting();
    void cook(Key key, Ref ref, u32 generation, u32 max_side);
    void cook_into(const Key& key, u32 max_side, Done& done);
    std::filesystem::path cache_file(const Key& key, u32 max_side) const;
    void upload(Done& done, bool whole);
    // The finest level whose sides are within tail_size (and that a
    // texture can start at).
    u32 tail_of(const rhi::TextureDesc& desc) const;
    // The level a cooked result goes up from: its tail (0 when whole), as
    // sharp as a drawn version it replaces.
    u32 upload_first(const Done& done, bool whole) const;
    // Puts a ready texture up with all its levels at once (finish()).
    void make_whole(Ref ref);
    void take_done();

    render::Renderer* m_renderer;
    std::filesystem::path m_cache;
    u32 m_workers;
    std::vector<Entry> m_entries;
    std::unordered_map<Key, Ref, KeyHash> m_lookup; // the given spellings too
    u64 m_version = 0;
    u64 m_updates = 0;
    usize m_running = 0; // jobs submitted, not taken back
    std::deque<Ref> m_waiting_to_cook;
    std::atomic<bool> m_stopping{false};
    std::mutex m_mutex; // protects m_done
    std::vector<std::unique_ptr<Done>> m_done;
    std::vector<std::unique_ptr<Done>> m_to_upload; // taken, not yet uploaded (over the budget)
    std::vector<std::unique_ptr<LevelsDone>> m_levels_done; // (m_mutex)
    usize m_reading = 0;    // level reads submitted, not taken back
    bool m_whole = false;   // finish(): uploads whole
    WaitGroup m_group;
    std::unique_ptr<JobSystem> m_jobs; // last: destroyed first, running what is queued
};

} // namespace giant::editor
