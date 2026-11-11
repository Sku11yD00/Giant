// The weather the viewport shows: the scene's (world::Weather), reached
// over a transition when a command asks for one, and the clouds carried
// along on its wind. View state: the scene holds only where the weather is
// going, so saving mid-transition saves the end of it.
#pragma once

#include "giant/core/math.h"
#include "giant/render/renderer.h"
#include "giant/world/scene.h"
#include "giant/world/weather.h"
#include "giant/world/wind.h"

#include <optional>

namespace giant::editor {

class WeatherState {
public:
    // Once a frame, `seconds` after the last: follows the scene's weather
    // (blending when a transition was asked for) and moves the clouds.
    // `camera`: a change of cloud size scales the pattern about it (the
    // clouds overhead stay put).
    // `scene`: the scene's generation (a new one starts the ground again).
    void update(const std::optional<world::Weather>& target, double seconds, const DVec3& camera = {},
                u64 scene = 0);
    // The scene's weather (changed or not) is reached over `seconds` from
    // the next update (0: at once), from what shows then.
    void transition(double seconds);

    const std::optional<world::Weather>& shown() const { return m_shown; }
    // 1: not blending.
    double progress() const;
    // Seconds left of a blend under way (0: none).
    double remaining() const;
    DVec3 cloud_offset() const { return m_offset; }
    // The ground under the shown weather (wet, snow-covered): from the
    // scene's start values when weather first shows or the scene is
    // replaced, then wetted, dried, covered and thawed as time passes
    // (another weather goes on from the ground as it is).
    world::Ground ground() const { return m_ground; }
    // Whether the ground follows a weather yet (else the next update starts
    // it from the target's start values).
    bool ground_on() const { return m_ground_on; }
    // Puts the ground there now (set_weather given wetness or snow_cover).
    void set_ground(const world::Ground& ground);
    // Seconds the weather has been shown (what falls moves with it).
    double time() const { return m_time; }
    // The wind near the ground under the shown weather (without one, the
    // default weather's light breeze), its gusts and clocks moved on:
    // what sways moves in it (FrameInput::wind).
    WindState wind() const { return m_wind.state(); }

private:
    std::optional<world::Weather> m_target; // the scene's, as last seen
    std::optional<world::Weather> m_shown;
    std::optional<world::Weather> m_from;
    double m_pending = 0.0; // seconds the next change takes
    bool m_requested = false; // transition() called since the last update
    double m_duration = 0.0;
    double m_elapsed = 0.0;
    DVec3 m_offset{};
    double m_tile = 0.0; // the cloud tile m_offset was last used with (0: none)
    world::Ground m_ground;
    bool m_ground_on = false; // m_ground follows a weather (else it starts at the next update)
    u64 m_scene = 0;          // the scene generation last seen
    double m_time = 0.0;
    WindClock m_wind;
};

// Metres the cloud pattern repeats over.
double cloud_tile_metres(const world::Weather& weather);

// The renderer's lighting for a scene: its own, or under its weather (the
// sun through the air, the sky's light greyed by clouds). `daylight`: the
// sky's light against the day's (a clock's, ClockLight); none: by the sun.
render::Lighting weather_lighting(const world::Lighting& lighting, const std::optional<world::Weather>& weather,
                                  std::optional<float> daylight = std::nullopt);
// The renderer's sky for a weather (off without one), with what falls
// and the ground (`ground`, `time`: WeatherState's).
render::Sky weather_sky(const world::Lighting& lighting, const std::optional<world::Weather>& weather,
                        const DVec3& cloud_offset, const world::Ground& ground = {}, double time = 0.0);

} // namespace giant::editor
