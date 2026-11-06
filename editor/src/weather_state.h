// The weather the viewport shows: the scene's (world::Weather), reached
// over a transition when a command asks for one, and the clouds carried
// along on its wind. View state: the scene holds only where the weather is
// going, so saving mid-transition saves the end of it.
#pragma once

#include "giant/core/math.h"
#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <optional>

namespace giant::editor {

class WeatherState {
public:
    // Once a frame, `seconds` after the last: follows the scene's weather
    // (blending when a transition was asked for) and moves the clouds.
    void update(const std::optional<world::Weather>& target, double seconds);
    // The scene's weather (changed or not) is reached over `seconds` from
    // the next update (0: at once), from what shows then.
    void transition(double seconds);

    const std::optional<world::Weather>& shown() const { return m_shown; }
    // 1: not blending.
    double progress() const;
    DVec3 cloud_offset() const { return m_offset; }

private:
    std::optional<world::Weather> m_target; // the scene's, as last seen
    std::optional<world::Weather> m_shown;
    std::optional<world::Weather> m_from;
    double m_pending = 0.0; // seconds the next change takes
    bool m_requested = false; // transition() called since the last update
    double m_duration = 0.0;
    double m_elapsed = 0.0;
    DVec3 m_offset{};
};

// The renderer's lighting for a scene: its own, or under its weather (the
// sun through the air, the sky's light greyed by clouds).
render::Lighting weather_lighting(const world::Lighting& lighting, const std::optional<world::Weather>& weather);
// The renderer's sky for a weather (off without one).
render::Sky weather_sky(const world::Lighting& lighting, const std::optional<world::Weather>& weather,
                        const DVec3& cloud_offset);

} // namespace giant::editor
