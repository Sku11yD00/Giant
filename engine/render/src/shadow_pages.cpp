#include "shadow_pages.h"

#include "cells.h"

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
    // At most 8192 pages (a 512 MiB atlas: 2^27 words, D3D12's largest raw view).
    m_desc.pool_pages = std::clamp<u32>(m_desc.pool_pages, 1u, 8192u);
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

double ShadowPages::depth_offset(const DVec3& camera) const {
    return -(to_light(camera).z - m_anchor) / double(m_desc.depth_range);
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
    m_prev_given = 0;
    for (Page& p : m_pages) {
        p.draw = false;
    }
    // The sun is taken in steps of k_sun_step: a sun moving slowly (time of
    // day) redraws the pages now and then, not every frame.
    const DVec3 now = normalize_or_zero(to_d(sun_direction));
    const DVec3 was = normalize_or_zero(to_d(m_sun));
    bool retired = false;
    if (!m_placed || !(dot(now, was) >= std::cos(k_sun_step))) {
        // Every page sees the world from a new angle (light space turns
        // about the world's origin): a new view, the old one standing in
        // until its pages are drawn again.
        if (m_placed) {
            retire_view(camera);
            retired = true;
        }
        m_sun = sun_direction;
        m_basis = basis_for(sun_direction);
        m_anchor_placed = false;
    }
    DVec3 light = to_light(camera);
    // Depths are measured from a fixed point along the light (not the
    // camera, which moves while drawn pages stay), moved only when the
    // camera has gone a quarter of the depth range from it.
    if (!m_anchor_placed || std::fabs(light.z - m_anchor) > double(m_desc.depth_range) * 0.25) {
        if (m_anchor_placed && !retired) {
            retire_view(camera); // depths from the old anchor
            retired = true;
        }
        m_anchor = light.z;
        m_anchor_placed = true;
    }
    if (m_prev.on && m_frame > m_prev.since + k_previous_frames) {
        release_previous();
    }
    if (retired) {
        // Placed afresh about the camera below.
        m_placed = false;
        light = to_light(camera);
    }
    for (u32 level = 0; level < m_desc.levels; ++level) {
        const double size = page_size(level);
        Window& old = m_window[level];
        // Kept while the camera stays within a page of the middle (at least
        // 7 pages on every side), so a camera bobbing across a page edge
        // does not move it back and forth.
        const double cx = light.x / size - double(old.x);
        const double cy = light.y / size - double(old.y);
        const double half = double(k_pages_per_axis / 2);
        if (m_placed && cx >= half - 1.0 && cx < half + 1.0 && cy >= half - 1.0 && cy < half + 1.0) {
            continue;
        }
        // Moved: the camera's page in the middle (7.5 pages on every side).
        const Window w{page_floor(light.x / size + 0.5) - i32(k_pages_per_axis / 2),
                       page_floor(light.y / size + 0.5) - i32(k_pages_per_axis / 2)};
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
    if (!m_reask.empty()) {
        // The pages used lately, in the new view at once (no wait for the
        // GPU to ask again).
        // The same pages about the camera's page as in the view left.
        m_reask_indices.clear();
        for (const Reask& q : m_reask) {
            const double size = page_size(q.level);
            const i32 x = page_floor(light.x / size) + q.dx;
            const i32 y = page_floor(light.y / size) + q.dy;
            if (in_window(q.level, x, y)) {
                m_reask_indices.push_back(q.level * k_slots + slot_of(x, y));
            }
        }
        // Only into free room: none of the previous view's pages goes
        // before anything has been drawn (those that find none are asked
        // for again by shading).
        allocate(m_reask_indices, false);
        m_reask.clear();
    }
    m_stats.resident = m_desc.pool_pages - u32(m_free.size());
}

void ShadowPages::retire_view(const DVec3& camera) {
    const DVec3 light = to_light(camera);
    m_reask.clear();
    // A view that drew less than the previous one holds (two steps close
    // together, a fast clock): the previous stays, this one's pages go.
    u32 drawn = 0;
    for (const Page& p : m_pages) {
        drawn += p.physical != k_no_page && p.drawn ? 1u : 0u;
    }
    const bool keep = m_prev.on && drawn < m_prev.held;
    if (!keep) {
        release_previous();
    }
    for (u32 i = 0; i < m_pages.size(); ++i) {
        Page& p = m_pages[i];
        if (p.physical != k_no_page && p.used + k_recent_frames >= m_frame) {
            const u32 level = i / k_slots;
            const double size = page_size(level);
            m_reask.push_back({level, p.x - page_floor(light.x / size), p.y - page_floor(light.y / size)});
        }
    }
    if (keep) {
        for (Page& p : m_pages) {
            free_page(p);
        }
        return;
    }
    m_prev.on = true;
    m_prev.basis = m_basis;
    m_prev.anchor = m_anchor;
    m_prev.window = m_window;
    m_prev.table.assign(m_pages.size(), k_no_page);
    m_prev.since = m_frame;
    m_prev.held = 0;
    m_prev.cursor = u32(m_pages.size());
    for (u32 i = 0; i < m_pages.size(); ++i) {
        Page& p = m_pages[i];
        if (p.physical == k_no_page) {
            continue;
        }
        if (p.drawn) {
            m_prev.table[i] = (p.physical & k_page_mask) | k_drawn_bit;
            ++m_prev.held;
            p = Page{}; // (its physical page is the previous view's now)
        } else {
            free_page(p);
        }
    }
    if (m_prev.held == 0) {
        release_previous();
    }
}

void ShadowPages::release_previous() {
    if (m_prev.on) {
        for (u32& e : m_prev.table) {
            if (e != k_no_page) {
                m_free.push_back(e & k_page_mask);
                e = k_no_page;
            }
        }
    }
    m_prev.on = false;
    m_prev.held = 0;
}

bool ShadowPages::release_previous_page() {
    if (!m_prev.on) {
        return false;
    }
    // Coarse levels first: the new view draws those first, so they are the
    // first the previous view's are no longer needed for.
    for (; m_prev.cursor > 0; --m_prev.cursor) {
        u32& e = m_prev.table[m_prev.cursor - 1];
        if (e != k_no_page) {
            m_free.push_back(e & k_page_mask);
            e = k_no_page;
            if (--m_prev.held == 0) {
                m_prev.on = false;
            }
            return true;
        }
    }
    m_prev.on = false;
    return false;
}

double ShadowPages::previous_depth_offset(const DVec3& camera) const {
    const double z = dot(camera, m_prev.basis.z);
    return -(z - m_prev.anchor) / double(m_desc.depth_range);
}

u32 ShadowPages::previous_entry_at(u32 level, const DVec3& world) const {
    if (!m_prev.on || level >= m_desc.levels) {
        return k_no_page;
    }
    const double size = page_size(level);
    const i32 x = page_floor(dot(world, m_prev.basis.x) / size);
    const i32 y = page_floor(dot(world, m_prev.basis.y) / size);
    const Window& w = m_prev.window[level];
    const i32 n = i32(k_pages_per_axis);
    if (x < w.x || x >= w.x + n || y < w.y || y >= w.y + n) {
        return k_no_page;
    }
    return m_prev.table[level * k_slots + slot_of(x, y)];
}

ShadowPages::Windows ShadowPages::windows() const {
    Windows w;
    for (u32 level = 0; level < m_desc.levels; ++level) {
        w.x[level] = m_window[level].x;
        w.y[level] = m_window[level].y;
    }
    return w;
}

void ShadowPages::request(std::span<const u32> bits, const Windows* asked_in) {
    if (!m_placed) {
        return;
    }
    m_stats.missing = 0;
    const size_t words = std::min(bits.size(), size_t(m_desc.levels) * (k_slots / 32));
    // The page a slot holds in a window.
    const auto page_in = [](i32 window, u32 slot_axis) {
        return window + wrap(i32(slot_axis) - window, i32(k_pages_per_axis));
    };
    // The requests asked for, as the windows are now: a slot whose page
    // has changed since (the window moved) is dropped, not taken for the
    // page now in its place.
    m_asked.clear();
    for (size_t wi = 0; wi < words; ++wi) {
        u32 word = bits[wi];
        while (word != 0u) {
            const u32 bit = u32(std::countr_zero(word));
            word &= word - 1u;
            const u32 level = u32(wi / (k_slots / 32));
            const u32 slot = u32(wi % (k_slots / 32)) * 32u + bit;
            if (asked_in != nullptr) {
                const u32 sx = slot % k_pages_per_axis;
                const u32 sy = slot / k_pages_per_axis;
                if (page_in(asked_in->x[level], sx) != page_in(m_window[level].x, sx) ||
                    page_in(asked_in->y[level], sy) != page_in(m_window[level].y, sy)) {
                    continue;
                }
            }
            m_asked.push_back(level * k_slots + slot);
        }
    }
    // First every resident page asked for is marked in use, so none of
    // them is given up to make room for another.
    for (const u32 i : m_asked) {
        if (m_pages[i].physical != k_no_page) {
            m_pages[i].used = m_frame;
        }
    }
    allocate(m_asked, true);
    m_stats.resident = m_desc.pool_pages - u32(m_free.size());
}

void ShadowPages::allocate(std::span<const u32> indices, bool asked) {
    const auto page_in = [](i32 window, u32 slot_axis) {
        return window + wrap(i32(slot_axis) - window, i32(k_pages_per_axis));
    };
    // Pages that may make room, longest unused first (worked out when the
    // pool first runs dry, once the previous view has none left to give).
    m_victims.clear();
    bool listed = false;
    size_t next_victim = 0;
    for (const u32 i : indices) {
        Page& p = m_pages[i];
        if (p.physical != k_no_page) {
            continue;
        }
        if (m_free.empty() && asked && m_prev_given < k_previous_given_per_frame) {
            m_prev_given += release_previous_page() ? 1u : 0u;
        }
        if (m_free.empty()) {
            if (!asked) {
                continue; // (re-asked: only where there is room)
            }
            if (!listed) {
                listed = true;
                for (u32 j = 0; j < m_pages.size(); ++j) {
                    if (m_pages[j].physical != k_no_page && m_pages[j].used < m_frame) {
                        m_victims.push_back(j);
                    }
                }
                std::sort(m_victims.begin(), m_victims.end(),
                          [this](u32 x, u32 y) { return m_pages[x].used < m_pages[y].used; });
            }
            if (next_victim == m_victims.size()) {
                ++m_stats.missing;
                continue;
            }
            free_page(m_pages[m_victims[next_victim++]]);
        }
        const u32 level = i / k_slots;
        const u32 slot = i % k_slots;
        p.physical = m_free.back();
        m_free.pop_back();
        p.x = page_in(m_window[level].x, slot % k_pages_per_axis);
        p.y = page_in(m_window[level].y, slot / k_pages_per_axis);
        p.drawn = false;
        p.draw = false;
        p.used = m_frame;
        mark_dirty(p);
    }
}

void ShadowPages::mark_dirty(Page& p) {
    if (!p.dirty) {
        p.dirty = true;
        p.dirty_since = m_frame;
    }
}

template <typename Visit>
bool ShadowPages::for_pages_under(const DVec3& min, const DVec3& max, u32 levels, Visit&& visit) {
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
        return false;
    }
    for (u32 level = 0; level < std::min(levels, m_desc.levels); ++level) {
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
                    visit(p);
                }
            }
        }
    }
    return true;
}

void ShadowPages::invalidate(const DVec3& min, const DVec3& max) {
    if (!m_placed || !(min.x <= max.x && min.y <= max.y && min.z <= max.z)) {
        return; // empty (or NaN)
    }
    if (!for_pages_under(min, max, m_desc.levels, [this](Page& p) { mark_dirty(p); })) {
        invalidate_all();
    }
}

void ShadowPages::sway(const DVec3& min, const DVec3& max, float reach) {
    if (!m_placed || !(reach > 0.0f) || !std::isfinite(reach) ||
        !(min.x <= max.x && min.y <= max.y && min.z <= max.z)) {
        return;
    }
    // Levels whose texels are smaller than the motion (finer levels first).
    u32 levels = 0;
    while (levels < m_desc.levels && page_size(levels) / double(k_page_texels) < double(reach)) {
        ++levels;
    }
    const DVec3 grow{reach, reach, reach};
    for_pages_under(min - grow, max + grow, levels, [this](Page& p) { p.sways = m_frame; });
}

void ShadowPages::invalidate_all() {
    for (Page& p : m_pages) {
        if (p.physical != k_no_page) {
            mark_dirty(p);
        }
    }
}

std::vector<ShadowPages::Draw> ShadowPages::plan(u32 budget, u32 sway_budget) {
    // Drawn pages only if shading asked for them lately (the rest stay
    // dirty, drawn once asked for again); pages not drawn (new, or after a
    // new sun or depth anchor) always, so coarse fallbacks come back.
    m_waiting.clear();
    for (u32 i = 0; i < m_pages.size(); ++i) {
        const Page& p = m_pages[i];
        if (p.physical != k_no_page && p.dirty && (!p.drawn || p.used + k_recent_frames >= m_frame)) {
            m_waiting.push_back(i);
        }
    }
    // Pages never drawn first (they read as missing), coarse levels first
    // among them (they cover the most); then the pages dirty longest, so
    // every level gets its turn however often things change.
    const auto order = [this](u32 a, u32 b) {
        const Page& pa = m_pages[a];
        const Page& pb = m_pages[b];
        if (pa.drawn != pb.drawn) {
            return !pa.drawn;
        }
        if (!pa.drawn) {
            const u32 la = a / k_slots;
            const u32 lb = b / k_slots;
            if (la != lb) {
                return la > lb;
            }
        } else if (pa.dirty_since != pb.dirty_since) {
            return pa.dirty_since < pb.dirty_since;
        }
        const u32 la = a / k_slots;
        const u32 lb = b / k_slots;
        if (la != lb) {
            return la > lb;
        }
        return a < b;
    };
    const size_t count = std::min<size_t>(m_waiting.size(), std::max<u32>(budget, 1u));
    std::partial_sort(m_waiting.begin(), m_waiting.begin() + std::ptrdiff_t(count), m_waiting.end(), order);
    std::vector<Draw> out;
    out.reserve(count + sway_budget);
    for (size_t k = 0; k < count; ++k) {
        const u32 i = m_waiting[k];
        Page& p = m_pages[i];
        p.dirty = false;
        p.drawn = true;
        p.draw = true;
        p.drawn_at = m_frame;
        out.push_back({i / k_slots, i % k_slots, p.physical, p.x, p.y});
    }
    m_stats.drawn = u32(count);
    m_stats.waiting = u32(m_waiting.size() - count);
    // The previous view is needed while any page of this one is still to
    // be drawn for the first time (those drawn this frame are read this
    // frame), and until shading's requests in the new view have come back
    // (they find pages the ones asked again missed).
    if (m_prev.on && m_frame > m_prev.since + k_previous_min_frames) {
        bool first_waiting = false;
        for (size_t k = count; k < m_waiting.size() && !first_waiting; ++k) {
            first_waiting = !m_pages[m_waiting[k]].drawn;
        }
        if (!first_waiting) {
            release_previous();
            m_stats.resident = m_desc.pool_pages - u32(m_free.size());
        }
    }
    // Then pages under swaying things, drawn longest ago first (those
    // shading asks for: the rest wait until asked again).
    m_stats.swayed = 0;
    if (sway_budget > 0) {
        m_swaying.clear();
        for (u32 i = 0; i < m_pages.size(); ++i) {
            const Page& p = m_pages[i];
            if (p.physical != k_no_page && p.drawn && !p.draw && p.sways == m_frame &&
                p.used + k_recent_frames >= m_frame) {
                m_swaying.push_back(i);
            }
        }
        const size_t more = std::min<size_t>(m_swaying.size(), sway_budget);
        std::partial_sort(m_swaying.begin(), m_swaying.begin() + std::ptrdiff_t(more), m_swaying.end(),
                          [this](u32 a, u32 b) {
                              const Page& pa = m_pages[a];
                              const Page& pb = m_pages[b];
                              return pa.drawn_at != pb.drawn_at ? pa.drawn_at < pb.drawn_at : a < b;
                          });
        for (size_t k = 0; k < more; ++k) {
            const u32 i = m_swaying[k];
            Page& p = m_pages[i];
            if (p.dirty && m_stats.waiting > 0) {
                --m_stats.waiting; // it was waiting too: drawn now
            }
            p.dirty = false;
            p.draw = true;
            p.drawn_at = m_frame;
            out.push_back({i / k_slots, i % k_slots, p.physical, p.x, p.y});
        }
        m_stats.swayed = u32(more);
        m_stats.drawn += u32(more);
    }
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
    if (out.size() >= 2 * m_pages.size()) {
        for (size_t i = 0; i < m_pages.size(); ++i) {
            out[m_pages.size() + i] = m_prev.on ? m_prev.table[i] : k_no_page;
        }
    }
}

void ShadowPages::write_levels(std::span<gpu::ShadowLevel> out, const DVec3& camera) const {
    const bool both = out.size() >= 2 * size_t{m_desc.levels};
    for (u32 view = 0; view < (both ? 2u : 1u); ++view) {
    const Basis& b = view == 0 || !m_prev.on ? m_basis : m_prev.basis;
    const std::vector<Window>& windows = view == 0 || !m_prev.on ? m_window : m_prev.window;
    const DVec3 light{dot(camera, b.x), dot(camera, b.y), dot(camera, b.z)};
    for (u32 level = 0; level < m_desc.levels && level < out.size(); ++level) {
        const double size = page_size(level);
        const Window& w = windows[level];
        gpu::ShadowLevel& l = out[view * m_desc.levels + level];
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

// Dirtied boxes are gathered in squares this many metres wide.
constexpr double k_dirty_square = 32.0;

void ShadowPages::track(std::span<const Instance> instances, std::span<const CellRange> cells,
                        std::span<const u8> casts, u64 meshes_version, const MeshLookup& meshes) {
    const auto casting = [&](size_t i) { return casts.empty() || (i < casts.size() && casts[i] != 0); };
    // Meshes came or went: those whose bounds differ from what the casters
    // used (terrain tiles stream in all the time; most meshes stay put).
    bool stale_any = false;
    m_meshes_version_changed = meshes_version != m_meshes_version;
    if (m_meshes_version_changed) {
        m_meshes_version = meshes_version;
        m_mesh_stale.assign(m_mesh_seen.size(), 0);
        for (MeshId id = 0; id < m_mesh_seen.size(); ++id) {
            MeshSeen& seen = m_mesh_seen[id];
            if (!seen.known) {
                continue;
            }
            const geometry::ClusterMesh* mesh = meshes ? meshes(id) : nullptr;
            const bool live = mesh != nullptr;
            if (live != seen.live || (live && !(mesh->bounds.center == seen.bounds.center &&
                                                mesh->bounds.radius == seen.bounds.radius))) {
                m_mesh_stale[id] = 1;
                stale_any = true;
                seen.live = live;
                seen.bounds = live ? mesh->bounds : geometry::Sphere{};
            }
        }
    }
    // k_no_mesh never has one; ids past any real table are looked at again
    // on every mesh change.
    const bool remeshed = m_meshes_version_changed;
    const auto stale = [&](MeshId id) {
        if (id == k_no_mesh) {
            return false;
        }
        return id >= k_max_seen_mesh ? remeshed : stale_any && id < m_mesh_stale.size() && m_mesh_stale[id] != 0;
    };
    // Only instances in the cells listed can have moved, and those past the
    // casters are new (whether each casts is per frame: looked at for all).
    const bool only = changed_cells(cells, instances.size(), m_seen_cells, m_changed_cells) &&
                      instances.size() >= m_casters.size();
    if (only) {
        m_look.assign(instances.size(), 0);
        for (const CellRange& c : m_changed_cells) {
            std::fill_n(m_look.begin() + c.first, c.count, u8{1});
        }
        if (m_meshes_version_changed) {
            for (size_t i = 0; i < m_casters.size(); ++i) {
                if (stale(m_casters[i].instance.mesh)) {
                    m_look[i] = 1;
                }
            }
        }
    }
    // The casters' bounds, noting the bounds of each mesh first used.
    const auto caster = [&](const Instance& in) {
        // Ids past any real table (k_max_seen_mesh: a stale or bogus id)
        // are not noted: their casters are looked at again on every mesh
        // change instead (stale() says so).
        if (in.mesh < k_max_seen_mesh) {
            if (in.mesh >= m_mesh_seen.size()) {
                m_mesh_seen.resize(size_t{in.mesh} + 1);
            }
            MeshSeen& seen = m_mesh_seen[in.mesh];
            if (!seen.known) {
                const geometry::ClusterMesh* mesh = meshes ? meshes(in.mesh) : nullptr;
                seen = {true, mesh != nullptr, mesh != nullptr ? mesh->bounds : geometry::Sphere{}};
            }
        }
        return caster_of(in, meshes);
    };
    const size_t limit = std::max<size_t>(256, std::max(instances.size(), m_casters.size()) / 8);
    // Boxes to dirty; past the limit, everything is.
    auto& boxes = m_boxes;
    boxes.clear();
    bool all = false;
    const auto dirty = [&](const Caster& c) {
        if (!c.live || !c.casts || all) {
            return;
        }
        // A box wider than a square keeps to itself (merged, it would
        // widen the square's); the rest share their centre's square.
        u64 key = 0;
        if (c.max.x - c.min.x > k_dirty_square || c.max.z - c.min.z > k_dirty_square) {
            key = (u64{1} << 63) | boxes.size();
        } else {
            const auto at = [](double v) {
                return static_cast<u64>(static_cast<u32>(static_cast<i32>(std::floor(v / k_dirty_square))));
            };
            key = (at(0.5 * (c.min.x + c.max.x)) | at(0.5 * (c.min.z + c.max.z)) << 32) & ~(u64{1} << 63);
        }
        const auto [it, added] = boxes.try_emplace(key, c.min, c.max);
        if (!added) {
            DVec3& lo = it->second.first;
            DVec3& hi = it->second.second;
            lo = {std::min(lo.x, c.min.x), std::min(lo.y, c.min.y), std::min(lo.z, c.min.z)};
            hi = {std::max(hi.x, c.max.x), std::max(hi.y, c.max.y), std::max(hi.z, c.max.z)};
        } else if (boxes.size() > limit) {
            all = true;
            boxes.clear();
        }
    };
    const size_t common = std::min(instances.size(), m_casters.size());
    for (size_t i = 0; i < common; ++i) {
        Caster& c = m_casters[i];
        if (only && m_look[i] == 0) {
            const bool cast = casting(i);
            if (cast != c.casts) {
                c.casts = true;
                dirty(c);
                c.casts = cast;
            }
            continue;
        }
        const Instance& in = instances[i];
        const Instance& was = c.instance;
        const bool moved = !(in.position == was.position && in.rotation == was.rotation && in.scale == was.scale &&
                             in.mesh == was.mesh);
        const bool cast = casting(i);
        if (!moved && !stale(in.mesh)) {
            c.instance = in; // the material may differ: shadows do not care
            if (cast != c.casts) {
                c.casts = true; // dirties where it is, whichever way it changed
                dirty(c);
                c.casts = cast;
            }
            continue;
        }
        Caster now = caster(in);
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
        m_casters[i] = caster(instances[i]); // added
        m_casters[i].casts = casting(i);
        dirty(m_casters[i]);
    }
    if (all) {
        invalidate_all();
        return;
    }
    for (const auto& [key, box] : boxes) {
        invalidate(box.first, box.second);
    }
}

} // namespace giant::render
