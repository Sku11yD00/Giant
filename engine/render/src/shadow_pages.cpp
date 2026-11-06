#include "shadow_pages.h"

#include "gpu_layout.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace giant::render {

namespace {

DVec3 to_d(const Vec3& v) { return {double(v.x), double(v.y), double(v.z)}; }

i32 wrap(i32 v, i32 n) {
    const i32 m = v % n;
    return m < 0 ? m + n : m;
}

// A page coordinate, saturated so a wild position cannot overflow.
i32 page_floor(double v) {
    constexpr double lim = 1.0e9;
    const double f = std::floor(v);
    return f != f ? 0 : static_cast<i32>(std::clamp(f, -lim, lim)); // NaN: 0
}

} // namespace

ShadowPages::ShadowPages(const ShadowDesc& desc) : m_desc(desc) {
    m_desc.levels = std::clamp<u32>(m_desc.levels, 1u, 16u);
    if (!(m_desc.extent > 0.0f) || !std::isfinite(m_desc.extent)) {
        m_desc.extent = ShadowDesc{}.extent;
    }
    if (!(m_desc.depth_range > 0.0f) || !std::isfinite(m_desc.depth_range)) {
        m_desc.depth_range = ShadowDesc{}.depth_range;
    }
    // The atlas is at most 128 x 128 pages (16384 texels a side).
    m_desc.pool_pages = std::clamp<u32>(m_desc.pool_pages, 1u, 128u * 128u);
    m_atlas_pages = 1;
    while (m_atlas_pages * m_atlas_pages < m_desc.pool_pages) {
        ++m_atlas_pages;
    }
    m_window.resize(m_desc.levels);
    m_pages.resize(size_t(m_desc.levels) * k_slots);
    m_free.reserve(m_desc.pool_pages);
    for (u32 i = m_desc.pool_pages; i-- > 0;) {
        m_free.push_back(i); // popped from the back: page 0 first
    }
    m_basis = basis_for(Vec3{0.0f, 1.0f, 0.0f});
    m_stats.capacity = m_desc.pool_pages;
}

double ShadowPages::page_size(u32 level) const {
    return double(m_desc.extent) * std::ldexp(1.0, i32(level)) / double(k_pages_per_axis);
}

ShadowPages::Basis ShadowPages::basis_for(const Vec3& sun_direction) {
    DVec3 z = -normalize_or_zero(to_d(sun_direction));
    if (length_squared(z) < 0.5) {
        z = {0.0, -1.0, 0.0}; // no sun direction: straight down
    }
    // Any axis across will do; world up unless the sun is (nearly) overhead.
    const DVec3 up = std::fabs(z.y) < 0.99 ? DVec3{0.0, 1.0, 0.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 x = normalize_or_zero(cross(up, z));
    const DVec3 y = cross(z, x);
    return {x, y, z};
}

DVec3 ShadowPages::to_light(const DVec3& world) const {
    return {dot(world, m_basis.x), dot(world, m_basis.y), dot(world, m_basis.z)};
}

bool ShadowPages::in_window(u32 level, i32 x, i32 y) const {
    const Window& w = m_window[level];
    return x >= w.x && x < w.x + i32(k_pages_per_axis) && y >= w.y && y < w.y + i32(k_pages_per_axis);
}

u32 ShadowPages::slot_of(i32 page_x, i32 page_y) const {
    const i32 n = i32(k_pages_per_axis);
    return u32(wrap(page_x, n) + n * wrap(page_y, n));
}

void ShadowPages::free_page(Page& p) {
    if (p.physical != k_no_page) {
        m_free.push_back(p.physical);
    }
    p = Page{};
}

void ShadowPages::begin_frame(const DVec3& camera, const Vec3& sun_direction) {
    ++m_frame;
    m_stats.drawn = 0;
    m_stats.missing = 0;
    for (Page& p : m_pages) {
        p.draw = false;
    }
    if (!m_placed || !(sun_direction == m_sun)) {
        m_sun = sun_direction;
        m_basis = basis_for(sun_direction);
        if (m_placed) {
            invalidate_all(); // every page sees the world from a new angle
        }
    }
    const DVec3 light = to_light(camera);
    for (u32 level = 0; level < m_desc.levels; ++level) {
        const double size = page_size(level);
        // The camera's page in the middle: at least 7.5 pages on every side.
        const Window w{page_floor(light.x / size + 0.5) - i32(k_pages_per_axis / 2),
                       page_floor(light.y / size + 0.5) - i32(k_pages_per_axis / 2)};
        Window& old = m_window[level];
        if (m_placed && w.x == old.x && w.y == old.y) {
            continue;
        }
        old = w;
        for (u32 slot = 0; slot < k_slots; ++slot) {
            Page& p = page(level, slot);
            if (p.physical != k_no_page && !in_window(level, p.x, p.y)) {
                free_page(p); // left the window: its slot now holds another page
            }
        }
    }
    m_placed = true;
    m_stats.resident = m_desc.pool_pages - u32(m_free.size());
}

void ShadowPages::request(std::span<const u32> bits) {
    if (!m_placed) {
        return;
    }
    // Pages that may make room, longest unused first (worked out when the
    // pool first runs dry).
    std::vector<u32> victims;
    bool listed = false;
    size_t next_victim = 0;
    const size_t words = std::min(bits.size(), size_t(m_desc.levels) * (k_slots / 32));
    for (size_t wi = 0; wi < words; ++wi) {
        u32 word = bits[wi];
        while (word != 0u) {
            const u32 bit = u32(std::countr_zero(word));
            word &= word - 1u;
            const u32 level = u32(wi / (k_slots / 32));
            const u32 slot = u32(wi % (k_slots / 32)) * 32u + bit;
            Page& p = page(level, slot);
            if (p.physical != k_no_page) {
                p.used = m_frame;
                continue;
            }
            if (m_free.empty()) {
                if (!listed) {
                    listed = true;
                    for (u32 i = 0; i < m_pages.size(); ++i) {
                        if (m_pages[i].physical != k_no_page && m_pages[i].used < m_frame) {
                            victims.push_back(i);
                        }
                    }
                    std::sort(victims.begin(), victims.end(),
                              [this](u32 a, u32 b) { return m_pages[a].used < m_pages[b].used; });
                }
                // A page asked for this time round is in use: skipped.
                while (next_victim < victims.size() && m_pages[victims[next_victim]].used >= m_frame) {
                    ++next_victim;
                }
                if (next_victim == victims.size()) {
                    ++m_stats.missing;
                    continue;
                }
                free_page(m_pages[victims[next_victim++]]);
            }
            // The page the slot holds in the window as it is now.
            const Window& w = m_window[level];
            const i32 n = i32(k_pages_per_axis);
            p.physical = m_free.back();
            m_free.pop_back();
            p.x = w.x + wrap(i32(slot % k_pages_per_axis) - w.x, n);
            p.y = w.y + wrap(i32(slot / k_pages_per_axis) - w.y, n);
            p.drawn = false;
            p.dirty = true;
            p.draw = false;
            p.used = m_frame;
        }
    }
    m_stats.resident = m_desc.pool_pages - u32(m_free.size());
}

void ShadowPages::invalidate(const DVec3& min, const DVec3& max) {
    if (!m_placed || !(min.x <= max.x && min.y <= max.y && min.z <= max.z)) {
        return; // empty (or NaN)
    }
    // The box's footprint across the light.
    double lx0 = std::numeric_limits<double>::infinity(), ly0 = lx0;
    double lx1 = -lx0, ly1 = -lx0;
    for (u32 corner = 0; corner < 8; ++corner) {
        const DVec3 p{corner & 1 ? max.x : min.x, corner & 2 ? max.y : min.y, corner & 4 ? max.z : min.z};
        const DVec3 l = to_light(p);
        lx0 = std::min(lx0, l.x);
        ly0 = std::min(ly0, l.y);
        lx1 = std::max(lx1, l.x);
        ly1 = std::max(ly1, l.y);
    }
    if (!std::isfinite(lx0) || !std::isfinite(ly0) || !std::isfinite(lx1) || !std::isfinite(ly1)) {
        invalidate_all();
        return;
    }
    for (u32 level = 0; level < m_desc.levels; ++level) {
        const double size = page_size(level);
        const Window& w = m_window[level];
        const i32 n = i32(k_pages_per_axis);
        const i32 x0 = std::max(page_floor(lx0 / size), w.x);
        const i32 y0 = std::max(page_floor(ly0 / size), w.y);
        const i32 x1 = std::min(page_floor(lx1 / size), w.x + n - 1);
        const i32 y1 = std::min(page_floor(ly1 / size), w.y + n - 1);
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                Page& p = page(level, slot_of(x, y));
                if (p.physical != k_no_page) {
                    p.dirty = true;
                }
            }
        }
    }
}

void ShadowPages::invalidate_all() {
    for (Page& p : m_pages) {
        if (p.physical != k_no_page) {
            p.dirty = true;
        }
    }
}

std::vector<ShadowPages::Draw> ShadowPages::plan(u32 budget) {
    std::vector<u32> waiting;
    for (u32 i = 0; i < m_pages.size(); ++i) {
        if (m_pages[i].physical != k_no_page && m_pages[i].dirty) {
            waiting.push_back(i);
        }
    }
    // Pages never drawn first (they read as missing), then coarse levels
    // (they cover the most), then the ones used most recently.
    const auto order = [this](u32 a, u32 b) {
        const Page& pa = m_pages[a];
        const Page& pb = m_pages[b];
        if (pa.drawn != pb.drawn) {
            return !pa.drawn;
        }
        const u32 la = a / k_slots;
        const u32 lb = b / k_slots;
        if (la != lb) {
            return la > lb;
        }
        if (pa.used != pb.used) {
            return pa.used > pb.used;
        }
        return a < b;
    };
    const size_t count = std::min<size_t>(waiting.size(), std::max<u32>(budget, 1u));
    std::partial_sort(waiting.begin(), waiting.begin() + std::ptrdiff_t(count), waiting.end(), order);
    std::vector<Draw> out;
    out.reserve(count);
    for (size_t k = 0; k < count; ++k) {
        const u32 i = waiting[k];
        Page& p = m_pages[i];
        p.dirty = false;
        p.drawn = true;
        p.draw = true;
        out.push_back({i / k_slots, i % k_slots, p.physical, p.x, p.y});
    }
    m_stats.drawn = u32(count);
    m_stats.waiting = u32(waiting.size() - count);
    return out;
}

void ShadowPages::write_table(std::span<u32> out) const {
    const size_t n = std::min(out.size(), m_pages.size());
    for (size_t i = 0; i < n; ++i) {
        const Page& p = m_pages[i];
        out[i] = p.physical == k_no_page
                     ? k_no_page
                     : (p.physical & k_page_mask) | (p.drawn ? k_drawn_bit : 0u) | (p.draw ? k_draw_bit : 0u);
    }
}

void ShadowPages::write_levels(gpu::ShadowLevel* out, const DVec3& camera) const {
    const DVec3 light = to_light(camera);
    for (u32 level = 0; level < m_desc.levels; ++level) {
        const double size = page_size(level);
        const Window& w = m_window[level];
        gpu::ShadowLevel& l = out[level];
        // In double, then made camera-relative: exact near the camera.
        l.origin[0] = static_cast<float>(double(w.x) * size - light.x);
        l.origin[1] = static_cast<float>(double(w.y) * size - light.y);
        l.page_size = static_cast<float>(size);
        l.texel_size = static_cast<float>(size / k_page_texels);
        l.page[0] = w.x;
        l.page[1] = w.y;
        l.pad[0] = 0;
        l.pad[1] = 0;
    }
}

u32 ShadowPages::entry_at(u32 level, const DVec3& world) const {
    if (level >= m_desc.levels || !m_placed) {
        return k_no_page;
    }
    const DVec3 l = to_light(world);
    const double size = page_size(level);
    const i32 x = page_floor(l.x / size);
    const i32 y = page_floor(l.y / size);
    if (!in_window(level, x, y)) {
        return k_no_page;
    }
    const Page& p = page(level, slot_of(x, y));
    if (p.physical == k_no_page) {
        return k_no_page;
    }
    return (p.physical & k_page_mask) | (p.drawn ? k_drawn_bit : 0u) | (p.draw ? k_draw_bit : 0u);
}

ShadowPages::Caster ShadowPages::caster_of(const Instance& in, const MeshLookup& meshes) const {
    Caster c;
    c.instance = in;
    const geometry::ClusterMesh* mesh = meshes ? meshes(in.mesh) : nullptr;
    if (mesh == nullptr) {
        return c;
    }
    const geometry::Sphere& s = mesh->bounds;
    const double scale = std::max({std::fabs(double(in.scale.x)), std::fabs(double(in.scale.y)),
                                   std::fabs(double(in.scale.z))});
    const Vec3 scaled{s.center.x * in.scale.x, s.center.y * in.scale.y, s.center.z * in.scale.z};
    const DVec3 centre = in.position + to_d(rotate(in.rotation, scaled));
    const double r = double(s.radius) * scale;
    if (!std::isfinite(centre.x) || !std::isfinite(centre.y) || !std::isfinite(centre.z) || !std::isfinite(r)) {
        return c;
    }
    c.min = centre - DVec3{r, r, r};
    c.max = centre + DVec3{r, r, r};
    c.live = true;
    return c;
}

void ShadowPages::track(std::span<const Instance> instances, std::span<const u8> casts, u64 meshes_version,
                        const MeshLookup& meshes) {
    const auto casting = [&](size_t i) { return casts.empty() || (i < casts.size() && casts[i] != 0); };
    const bool remesh = meshes_version != m_meshes_version;
    m_meshes_version = meshes_version;
    const size_t limit = std::max<size_t>(256, std::max(instances.size(), m_casters.size()) / 8);
    // Boxes to dirty; past the limit, everything is.
    std::vector<std::pair<DVec3, DVec3>> boxes;
    bool all = false;
    const auto dirty = [&](const Caster& c) {
        if (!c.live || !c.casts || all) {
            return;
        }
        if (boxes.size() >= limit) {
            all = true;
            boxes.clear();
            return;
        }
        boxes.emplace_back(c.min, c.max);
    };
    const size_t common = std::min(instances.size(), m_casters.size());
    for (size_t i = 0; i < common; ++i) {
        const Instance& in = instances[i];
        Caster& c = m_casters[i];
        const Instance& was = c.instance;
        const bool moved = !(in.position == was.position && in.rotation == was.rotation && in.scale == was.scale &&
                             in.mesh == was.mesh);
        const bool cast = casting(i);
        if (!moved && !remesh) {
            c.instance = in; // the material may differ: shadows do not care
            if (cast != c.casts) {
                c.casts = true; // dirties where it is, whichever way it changed
                dirty(c);
                c.casts = cast;
            }
            continue;
        }
        Caster now = caster_of(in, meshes);
        now.casts = cast;
        if (!moved && now.live == c.live && now.casts == c.casts &&
            (!now.live || (now.min == c.min && now.max == c.max))) {
            c = now;
            continue; // the mesh table changed, this caster did not
        }
        dirty(c);
        dirty(now);
        c = now;
    }
    for (size_t i = common; i < m_casters.size(); ++i) {
        dirty(m_casters[i]); // removed
    }
    m_casters.resize(instances.size());
    for (size_t i = common; i < instances.size(); ++i) {
        m_casters[i] = caster_of(instances[i], meshes); // added
        m_casters[i].casts = casting(i);
        dirty(m_casters[i]);
    }
    if (all) {
        invalidate_all();
        return;
    }
    for (const auto& [lo, hi] : boxes) {
        invalidate(lo, hi);
    }
}

} // namespace giant::render
