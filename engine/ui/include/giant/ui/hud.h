// HUD pieces that need the world: where a point in the world is on the
// screen (or which edge it is off), headings and bearings for a compass,
// and world-anchored markers drawn at their place or clamped to the
// screen's edge pointing the way. Screens draw the compass (screen.h,
// "compass"); markers are drawn here, under the screens.
//
// Directions: north is -Z, east +X (the editor's yaw 0 looks north, 90
// west); headings and bearings are degrees clockwise from north, 0 to 360.
#pragma once

#include "giant/core/math.h"
#include "giant/ui/canvas.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace giant::ui {

// Where the eye is and how it looks (as the renderer's view).
struct View {
    DVec3 position{};
    Quat orientation{}; // looks down -Z, +Y up, before rotation
    float vertical_fov_degrees = 60.0f;
};

// Degrees clockwise from north of where it looks (its forward, flattened).
float heading_degrees(const Quat& orientation);
// Degrees clockwise from north from `from` to `to` (on the ground).
float bearing_degrees(const DVec3& from, const DVec3& to);
// The shortest turn from `a` to `b` (-180 to 180: negative to the left).
float turn_degrees(float a, float b);

// Where `point` is on a `size` pixel screen, if in front of the eye.
struct Projected {
    Vec2 at{};           // pixels (may be off the screen)
    bool in_front = false;
    float depth = 0.0f;  // metres along the view
};
Projected project(const View& view, Vec2 size, const DVec3& point);

// A mark on a compass: a bearing and a colour.
struct CompassMark {
    float bearing = 0.0f;
    u32 color = k_white;
};

struct Marker {
    std::string id;
    DVec3 position{};
    std::string label;          // under it (UTF-8, at most 128 bytes)
    u32 color = rgba(255, 210, 90);
    bool clamp = true;          // off the screen or behind: at the edge, an arrow the way
    bool compass = true;        // also on the compasses screens draw
    bool distance = true;       // "123 m" under the label
    double max_distance = 0.0;  // further than this: not shown (0: always); fades over the last 10%
};

// Where a marker was drawn, last frame.
struct PlacedMarker {
    std::string id;
    Vec2 at{};
    bool shown = false;
    bool on_screen = false; // at its place (else clamped to the edge, or not shown)
    double distance = 0.0;
    float bearing = 0.0f;
};

class Markers {
public:
    // Added, or replaced by id (1 to 64 bytes). False if there would be more than k_max.
    bool set(Marker marker);
    bool erase(std::string_view id);
    void clear() { m_markers.clear(); }
    const Marker* find(std::string_view id) const;
    usize size() const { return m_markers.size(); }
    const std::map<std::string, Marker, std::less<>>& all() const { return m_markers; }
    static constexpr usize k_max = 256;

    // Drawn for this frame; their places kept (placed()).
    void draw(Canvas& canvas, const Font& font, const View& view, Vec2 size, float scale);
    const std::vector<PlacedMarker>& placed() const { return m_placed; }
    // For compasses: each shown marker that goes on one.
    const std::vector<CompassMark>& compass_marks() const { return m_marks; }
    // Without drawing: the compass marks for a view (as draw would give).
    void update_marks(const View& view);

private:
    std::map<std::string, Marker, std::less<>> m_markers;
    std::vector<PlacedMarker> m_placed;
    std::vector<CompassMark> m_marks;
};

} // namespace giant::ui
