// Virtual shadow maps for the sun: which shadow pages exist, where they
// live in the physical page pool, and which must be drawn this frame
// (docs/architecture.md, "Sun shadows").
//
// The sun's view is a clipmap of square levels in light space, centred on
// the camera: level 0 covers `extent` metres, each level up twice as much.
// Each level is a window of 16 x 16 virtual pages of 128 x 128 texels, so
// a texel of level L is extent * 2^L / 2048 metres, and moved a page at a
// time with the camera (a page keeps its slot, its coordinate modulo 16,
// while it stays in the window). Only pages something on screen asked for
// are kept: shading asks for the page it wants (and, when that one is not
// drawn yet, each coarser one it falls back to), the GPU's requests are
// read back a couple of frames later, and the pages they name are given
// room in the pool and drawn. A drawn page stays as it is (cached) until
// what casts onto it changes: a caster added, moved, changed or removed
// dirties the pages under where it was and where it is; a new sun
// direction (in steps of a quarter of a degree) dirties all of them and
// they read as missing until drawn again. Drawing is budgeted: at most
// `pages_per_frame` pages a frame, pages never drawn first (coarse levels
// first), then those dirty longest; only pages asked for lately. A page not drawn yet reads as missing (the shader falls
// back to a coarser level); a dirty one keeps its old content until it is
// drawn again. When the pool is full, the page used longest ago makes room.
//
// Plain CPU bookkeeping, tested on its own; the renderer turns plan()
// into GPU work (clears, culling and raster per level).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/render/renderer.h"

#include <functional>
#include <utility>
#include <span>
#include <vector>

namespace giant::render {

namespace gpu {
struct ShadowLevel;
}

struct ShadowDesc {
    u32 levels = 12;            // 1 to 16
    float extent = 16.0f;       // level 0's width, metres (level 11: about 33 km)
    u32 pool_pages = 1024;      // physical pages of 128 x 128 (the default: 64 MiB of depths; at most 8192)
    float depth_range = 40000.0f; // metres along the sun, centred on the camera
};

struct ShadowStats {
    u32 resident = 0;    // pages with room in the pool
    u32 capacity = 0;    // the pool, pages
    u32 drawn = 0;       // pages drawn this frame
    u32 waiting = 0;     // pages asked for or dirty, not drawn yet
    u32 missing = 0;     // pages asked for that found no room (pool full of pages in use)
};

// Each level's window (its minimum page).
struct ShadowWindows {
    i32 x[16] = {};
    i32 y[16] = {};
};

class ShadowPages {
public:
    static constexpr u32 k_page_texels = 128;
    static constexpr u32 k_pages_per_axis = 16;
    static constexpr u32 k_slots = k_pages_per_axis * k_pages_per_axis; // per level
    static constexpr u32 k_no_page = ~0u;
    // Table entries: the physical page in the low bits, these flags above.
    static constexpr u32 k_page_mask = 0xFFFFu;
    static constexpr u32 k_drawn_bit = 1u << 30; // has content (drawn at least once)
    static constexpr u32 k_draw_bit = 1u << 29;  // drawn this frame

    explicit ShadowPages(const ShadowDesc& desc = {});

    const ShadowDesc& desc() const { return m_desc; }
    u32 levels() const { return m_desc.levels; }
    u32 atlas_pages_per_axis() const { return m_atlas_pages; } // the pool as a square of pages
    double page_size(u32 level) const;  // metres
    double texel_size(u32 level) const { return page_size(level) / k_page_texels; }

    // The light's axes for a sun direction (unit, towards the sun): x and y
    // across, z along the light (away from the sun).
    struct Basis {
        DVec3 x;
        DVec3 y;
        DVec3 z;
    };
    static Basis basis_for(const Vec3& sun_direction);
    const Basis& basis() const { return m_basis; }
    DVec3 to_light(const DVec3& world) const;
    // Depths are 0.5 - (light z - anchor) / depth range (larger: nearer
    // the sun), the anchor a fixed light z near the camera. For a point
    // relative to the camera: 0.5 - z_rel / depth range + depth_offset.
    double depth_offset(const DVec3& camera) const;
    // Sun directions closer than this to the one in use are not taken up.
    static constexpr double k_sun_step = 0.25 * 3.14159265358979323846 / 180.0;

    // Starts a frame: moves the windows with the camera (pages that leave
    // are freed) and, if the sun moved, dirties every page.
    void begin_frame(const DVec3& camera, const Vec3& sun_direction);
    // The windows as they are now, to go with requests asked against them.
    using Windows = ShadowWindows;
    Windows windows() const;
    // The GPU's requests, a bit per slot, level after level (levels() * 8
    // words), from a recent frame. With `asked_in` (the windows then),
    // slots whose page has changed since are dropped; without, they are
    // read against the windows as they are now.
    void request(std::span<const u32> bits, const Windows* asked_in = nullptr);
    // Something casting shadows changed inside this world box: the pages
    // under it (on every level) are drawn again.
    void invalidate(const DVec3& min, const DVec3& max);
    void invalidate_all();

    // What to draw this frame, at most `budget` pages (at least one if any
    // wait): marks them drawn. Call once a frame, after the requests.
    struct Draw {
        u32 level = 0;
        u32 slot = 0;
        u32 physical = 0;
        i32 page_x = 0; // absolute page coordinate in the level's grid
        i32 page_y = 0;
    };
    std::vector<Draw> plan(u32 budget);

    // For the GPU: levels() * k_slots table entries (k_no_page, or the
    // physical page with flags), and per level its window.
    void write_table(std::span<u32> out) const;
    void write_levels(gpu::ShadowLevel* out, const DVec3& camera) const;
    // A level's window: its minimum page.
    i32 window_x(u32 level) const { return m_window[level].x; }
    i32 window_y(u32 level) const { return m_window[level].y; }
    u32 slot_of(i32 page_x, i32 page_y) const;

    const ShadowStats& stats() const { return m_stats; }
    // The state of the page holding a world position on a level (tests):
    // k_no_page, or its table entry.
    u32 entry_at(u32 level, const DVec3& world) const;

    // Keeps a copy of the casters (the frame's instances with their world
    // bounds) and dirties the pages under every one that changed: moved,
    // another mesh, or starting or stopping to cast (`casts`, per instance:
    // drawn as a mesh this frame; empty: all cast). Many changes at once
    // (more than max(256, an eighth)) dirty everything.
    using MeshLookup = std::function<const geometry::ClusterMesh*(MeshId)>;
    // With `cells` (FrameInput::cells), only the cells that changed since
    // the last call are compared.
    void track(std::span<const Instance> instances, std::span<const CellRange> cells, std::span<const u8> casts,
               u64 meshes_version, const MeshLookup& meshes);

private:
    struct Page {
        u32 physical = k_no_page;
        i32 x = 0;
        i32 y = 0;
        bool drawn = false; // has content
        bool dirty = false; // to be drawn (again)
        bool draw = false;  // drawn this frame
        u64 used = 0;       // the frame it was last asked for
        u64 dirty_since = 0; // the frame it became dirty
    };
    struct Window {
        i32 x = 0;
        i32 y = 0;
    };
    struct Caster {
        Instance instance;
        DVec3 min{};
        DVec3 max{};
        bool live = false;  // has a mesh (and bounds)
        bool casts = false; // drawn this frame (with live: dirties pages)
    };

    Page& page(u32 level, u32 slot) { return m_pages[level * k_slots + slot]; }
    const Page& page(u32 level, u32 slot) const { return m_pages[level * k_slots + slot]; }
    void free_page(Page& p);
    void mark_dirty(Page& p);
    // Dirty pages not asked for within this many frames wait to be asked again.
    static constexpr u64 k_recent_frames = 30;
    bool in_window(u32 level, i32 x, i32 y) const;
    Caster caster_of(const Instance& in, const MeshLookup& meshes) const;

    ShadowDesc m_desc;
    u32 m_atlas_pages = 0;
    Basis m_basis;
    Vec3 m_sun{};
    double m_anchor = 0.0; // light z depths are measured from
    bool m_anchor_placed = false;
    bool m_placed = false;
    u64 m_frame = 0;
    std::vector<Window> m_window;
    std::vector<Page> m_pages; // per level and slot
    std::vector<u32> m_free;   // physical pages
    std::vector<Caster> m_casters;
    std::vector<CellRange> m_seen_cells;    // FrameInput::cells as last tracked
    std::vector<CellRange> m_changed_cells; // scratch
    std::vector<u8> m_look;                 // scratch: 1 for instances in changed cells
    u64 m_meshes_version = ~u64{0};
    ShadowStats m_stats;
    // Scratch, kept to avoid allocating every frame.
    std::vector<u32> m_asked;
    std::vector<u32> m_victims;
    std::vector<u32> m_waiting;
    std::vector<std::pair<DVec3, DVec3>> m_boxes;
};

} // namespace giant::render
