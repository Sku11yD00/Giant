// The time of day the viewport shows: the scene's clock (world/clock.h),
// moved on by play (game seconds times its speed). View state: the scene
// holds the clock as authored, so saving the scene saves where it starts;
// a game save keeps the time play reached (GameSaves).
#pragma once

#include "giant/render/renderer.h"
#include "giant/world/clock.h"
#include "giant/world/scene.h"

#include <optional>

namespace giant::editor {

class ClockState {
public:
    // Once a frame: the scene's clock shows again when it changed (or the
    // scene was replaced, `scene` its generation); then, while `running`
    // (play), the time moves on `game_seconds` times the clock's speed.
    void update(const world::Clock& clock, double game_seconds, bool running, u64 scene = 0);
    // The clock shown (the scene's, at the time reached).
    const world::Clock& shown() const { return m_shown; }
    // Whether it shows `clock` (the scene's as last followed); else the
    // next update shows that clock again.
    bool follows(const world::Clock& clock) const { return m_seen && clock == m_scene; }
    // Puts the time there (a loaded game), until the scene's clock
    // changes or show() is called (set_time, close_game).
    // Out of range: unchanged.
    void set_time(double hours, i32 day);
    // Shows `clock` (the scene's, just set or put back) from now.
    void show(const world::Clock& clock);

private:
    world::Clock m_scene; // the scene's, as last seen
    world::Clock m_shown;
    bool m_seen = false;
    u64 m_generation = 0;
};

// The scene's light sources and sky placed by a clock (on): the sun or the
// moon as world::clock_lighting places them, and the sky's light (and the
// flat colours') scaled by world::ambient_scale. Off: none.
struct ClockLight {
    world::Lighting lighting;
    std::optional<float> daylight;
};
ClockLight clock_light(const world::Lighting& lighting, const world::Clock& clock);

// The night sky drawn by a clock (on): the stars turned to the hour and
// out as the dusk deepens, and the disc the moon's (by its phase) while it
// is the light. Off: the sky as it was.
void clock_sky(render::Sky& sky, const world::Clock& clock);

} // namespace giant::editor
