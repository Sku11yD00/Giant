// World-anchored HUD pieces: projection, headings and bearings, markers.
#include "giant/ui/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace giant::ui {

namespace {

constexpr float k_pi = 3.14159265358979f;

float wrap_degrees(float d) {
    if (!std::isfinite(d)) return 0.0f;
    d = std::fmod(d, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d >= 360.0f ? 0.0f : d;
}

u32 faded(u32 color, float alpha) {
    const float a = static_cast<float>(color >> 24) * std::clamp(alpha, 0.0f, 1.0f);
    return (color & 0x00FFFFFFu) | (static_cast<u32>(std::lround(a)) << 24);
}

std::string distance_text(double metres) {
    char buffer[32];
    if (metres < 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f m", metres);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f km", metres / 1000.0);
    }
    return buffer;
}

// A diamond's outline (its corners `r` from the middle) and a dot in it.
void diamond(Canvas& canvas, Vec2 c, float r, float t, u32 color, u32 edge) {
    const Vec2 n{c.x, c.y - r}, e{c.x + r, c.y}, s{c.x, c.y + r}, w{c.x - r, c.y};
    for (const auto& [a, b] : {std::pair{n, e}, std::pair{e, s}, std::pair{s, w}, std::pair{w, n}}) {
        canvas.line(a, b, t + 2.0f, edge);
    }
    for (const auto& [a, b] : {std::pair{n, e}, std::pair{e, s}, std::pair{s, w}, std::pair{w, n}}) {
        canvas.line(a, b, t, color);
    }
    const float d = std::max(std::round(r * 0.35f), 1.0f);
    canvas.rect({c.x - d, c.y - d, 2.0f * d, 2.0f * d}, color);
}

} // namespace

float heading_degrees(const Quat& orientation) {
    const Vec3 f = rotate(orientation, Vec3{0.0f, 0.0f, -1.0f});
    if (std::fabs(f.x) < 1e-6f && std::fabs(f.z) < 1e-6f) {
        // Straight up or down: by where its top points instead.
        const Vec3 up = rotate(orientation, Vec3{0.0f, 1.0f, 0.0f});
        const float sign = f.y > 0.0f ? -1.0f : 1.0f;
        return wrap_degrees(std::atan2(sign * up.x, -sign * up.z) * 180.0f / k_pi);
    }
    return wrap_degrees(std::atan2(f.x, -f.z) * 180.0f / k_pi);
}

float bearing_degrees(const DVec3& from, const DVec3& to) {
    const double dx = to.x - from.x, dz = to.z - from.z;
    if (std::fabs(dx) < 1e-9 && std::fabs(dz) < 1e-9) return 0.0f;
    return wrap_degrees(static_cast<float>(std::atan2(dx, -dz) * 180.0 / 3.14159265358979323846));
}

float turn_degrees(float a, float b) {
    float d = wrap_degrees(b - a);
    if (d > 180.0f) d -= 360.0f;
    return d;
}

Projected project(const View& view, Vec2 size, const DVec3& point) {
    Projected out;
    const Vec3 rel = to_camera_relative(point, view.position);
    const Vec3 v = rotate(conjugate(normalize(view.orientation)), rel);
    out.depth = -v.z;
    const float fov = std::clamp(std::isfinite(view.vertical_fov_degrees) ? view.vertical_fov_degrees : 60.0f, 1.0f, 179.0f);
    const float t = std::tan(0.5f * fov * k_pi / 180.0f);
    if (!(size.x > 0.0f) || !(size.y > 0.0f) || !std::isfinite(out.depth)) return out;
    const float aspect = size.x / size.y;
    if (out.depth > 1e-4f) {
        out.in_front = true;
        const float nx = v.x / (out.depth * t * aspect), ny = v.y / (out.depth * t);
        out.at = {0.5f * size.x * (1.0f + nx), 0.5f * size.y * (1.0f - ny)};
        if (!std::isfinite(out.at.x) || !std::isfinite(out.at.y)) {
            out.in_front = false;
            out.at = {};
        }
    }
    return out;
}

bool Markers::set(Marker marker) {
    if (marker.id.empty() || marker.id.size() > 64) return false;
    if (marker.label.size() > 128) {
        usize n = 128;
        while (n > 0 && (static_cast<u8>(marker.label[n]) & 0xC0) == 0x80) --n;
        marker.label.resize(n);
    }
    if (!std::isfinite(marker.max_distance) || marker.max_distance < 0.0) marker.max_distance = 0.0;
    const auto ok = [](double v) { return std::isfinite(v) && std::fabs(v) <= 1e12; };
    if (!ok(marker.position.x) || !ok(marker.position.y) || !ok(marker.position.z)) return false;
    const auto it = m_markers.find(marker.id);
    if (it != m_markers.end()) {
        it->second = std::move(marker);
        return true;
    }
    if (m_markers.size() >= k_max) return false;
    std::string id = marker.id;
    m_markers.emplace(std::move(id), std::move(marker));
    return true;
}

bool Markers::erase(std::string_view id) {
    const auto it = m_markers.find(id);
    if (it == m_markers.end()) return false;
    m_markers.erase(it);
    return true;
}

const Marker* Markers::find(std::string_view id) const {
    const auto it = m_markers.find(id);
    return it != m_markers.end() ? &it->second : nullptr;
}

void Markers::update_marks(const View& view) {
    m_marks.clear();
    for (const auto& [id, m] : m_markers) {
        (void)id;
        const double d = length(m.position - view.position);
        if (!m.compass || (m.max_distance > 0.0 && d > m.max_distance)) continue;
        m_marks.push_back({bearing_degrees(view.position, m.position), m.color});
    }
}

void Markers::draw(Canvas& canvas, const Font& font, const View& view, Vec2 size, float scale) {
    m_placed.clear();
    update_marks(view);
    if (!(size.x > 0.0f) || !(size.y > 0.0f) || !std::isfinite(scale) || scale <= 0.0f) return;
    const Quat& q = view.orientation;
    if (!std::isfinite(view.position.x) || !std::isfinite(view.position.y) || !std::isfinite(view.position.z) ||
        !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
        return;
    const float inset = 48.0f * scale, r = 9.0f * scale, t = std::max(2.0f * scale, 1.0f);
    const Vec2 centre{0.5f * size.x, 0.5f * size.y};
    const u32 edge = rgba(0, 0, 0, 150);
    TextStyle label;
    label.size = 20.0f * scale;
    label.align = Align::center;
    label.shadow = std::max(1.0f, 1.5f * scale);
    TextStyle small = label;
    small.size = 17.0f * scale;
    for (const auto& [id, m] : m_markers) {
        PlacedMarker p;
        p.id = id;
        p.distance = length(m.position - view.position);
        p.bearing = bearing_degrees(view.position, m.position);
        if (!std::isfinite(p.distance)) {
            m_placed.push_back(std::move(p));
            continue;
        }
        float alpha = 1.0f;
        if (m.max_distance > 0.0) {
            if (p.distance > m.max_distance) {
                m_placed.push_back(std::move(p));
                continue;
            }
            alpha = static_cast<float>(std::clamp((m.max_distance - p.distance) / (0.1 * m.max_distance), 0.0, 1.0));
        }
        const Projected pr = project(view, size, m.position);
        p.on_screen = pr.in_front && pr.at.x >= inset && pr.at.x <= size.x - inset && pr.at.y >= inset &&
                      pr.at.y <= size.y - inset;
        if (!p.on_screen && !m.clamp) {
            m_placed.push_back(std::move(p));
            continue;
        }
        Vec2 at = pr.at;
        Vec2 dir{};
        if (!p.on_screen) {
            // The way to it on the screen: from the middle, out to the edge.
            const Vec3 v = rotate(conjugate(normalize(view.orientation)), to_camera_relative(m.position, view.position));
            dir = pr.in_front ? Vec2{pr.at.x - centre.x, pr.at.y - centre.y} : Vec2{v.x, -v.y};
            if (!pr.in_front && std::fabs(dir.x) < 1e-3f * std::fabs(dir.y)) dir.x = 0.0f;
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            // Right behind (or ahead, on a screen too small to have a middle): down.
            dir = len > 1e-6f && std::isfinite(len) ? Vec2{dir.x / len, dir.y / len} : Vec2{0.0f, 1.0f};
            const float hx = std::max(centre.x - inset, 1.0f), hy = std::max(centre.y - inset, 1.0f);
            const float k = std::min(std::fabs(dir.x) > 1e-6f ? hx / std::fabs(dir.x) : 1e9f,
                                     std::fabs(dir.y) > 1e-6f ? hy / std::fabs(dir.y) : 1e9f);
            at = {centre.x + dir.x * k, centre.y + dir.y * k};
        }
        at = {std::round(at.x), std::round(at.y)};
        p.at = at;
        p.shown = true;
        const u32 color = faded(m.color, alpha);
        diamond(canvas, at, r, t, color, faded(edge, alpha));
        if (!p.on_screen) {
            // An arrow beyond it, the way to turn.
            const Vec2 tip{at.x + dir.x * (r + 10.0f * scale), at.y + dir.y * (r + 10.0f * scale)};
            const Vec2 back{at.x + dir.x * (r + 3.0f * scale), at.y + dir.y * (r + 3.0f * scale)};
            const Vec2 side{-dir.y * 6.0f * scale, dir.x * 6.0f * scale};
            canvas.line({back.x + side.x, back.y + side.y}, tip, t, color);
            canvas.line({back.x - side.x, back.y - side.y}, tip, t, color);
        }
        // Its label and distance under it (over it when it points down), kept on the screen.
        const std::string far = m.distance ? distance_text(p.distance) : std::string();
        const Vec2 ls = m.label.empty() ? Vec2{} : measure(font, m.label, label);
        const Vec2 ds = far.empty() ? Vec2{} : measure(font, far, small);
        const float gap = 4.0f * scale;
        const bool above = !p.on_screen && dir.y > 0.3f;
        float y = above ? at.y - r - gap - ls.y - ds.y : at.y + r + gap;
        if (y + ls.y + ds.y > size.y - gap) y = at.y - r - gap - ls.y - ds.y; // (no room under it: over it)
        y = std::max(y, gap);
        const auto keep_x = [&](float w) { return std::clamp(at.x, 0.5f * w + gap, std::max(size.x - 0.5f * w - gap, 0.5f * w + gap)); };
        if (!m.label.empty()) {
            label.color = faded(k_white, alpha);
            label.shadow_color = faded(rgba(0, 0, 0, 160), alpha);
            canvas.text({keep_x(ls.x), y}, m.label, label);
            y += ls.y;
        }
        if (!far.empty()) {
            small.color = faded(rgba(255, 255, 255, 200), alpha);
            small.shadow_color = faded(rgba(0, 0, 0, 160), alpha);
            canvas.text({keep_x(ds.x), y}, far, small);
        }
        m_placed.push_back(std::move(p));
    }
}

} // namespace giant::ui
