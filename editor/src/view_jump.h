// A view that jumped: where it looks from is suddenly far from where it was (set_camera, a person
// far off followed, the player travelled with no loading screen to stand for it). Flying or walking
// brings the world in a slab at a time; a jump leaves all of it to come at once, and at play's
// shares of a frame the ground, the plants and the voxel world round the new place take many
// seconds to fill in. Until they are in, the world comes with the shares a game's loading screen
// gives it (main.cpp): heavier frames for a moment rather than a bare place for long.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <optional>

namespace giant::editor {

class ViewJump {
public:
    // Farther than this between two frames is a jump: a cell's width...
    static constexpr double k_metres = 64.0;
    // ...and faster than anything flies, so that a flight is no jump in a frame that took long
    // (90 m/s crosses 64 m in a frame of 0.7 s): farther than this speed covers in the frame, a
    // frame counting for no more than k_most_stall (after a stall a real jump is still one).
    static constexpr double k_speed = 400.0;
    static constexpr double k_most_stall = 1.0;
    // The world is in once nothing was coming for this many frames running (the first of them
    // say nothing: what a jump brings is asked for a frame or two after it)...
    static constexpr u32 k_quiet_frames = 10;
    // ...and it is not waited for longer than this, of frames (some of it may never come), a
    // frame counting for no more than k_most_frame (a window held is no time passed).
    static constexpr double k_most_seconds = 20.0;
    static constexpr double k_most_frame = 0.1;

    // Each frame, with where the view is. `seconds`: the frame's. `held`: something else stands
    // for the world's coming this frame (a loading screen), or it is not wanted (a game being
    // played: heavier frames there are hitches): no jump is counted, one under way ends.
    void see(const DVec3& view, double seconds, bool held) {
        const double frame = seconds > 0.0 ? seconds : 0.0;
        const double moved = m_view ? length(view - *m_view) : 0.0;
        const bool jumped = moved > k_metres && moved > k_speed * (frame < k_most_stall ? frame : k_most_stall); // (false of a view that is no number)
        m_view = view;
        if (held) {
            m_on = false;
            return;
        }
        if (jumped) {
            m_on = true;
            m_quiet = 0;
            m_seconds = 0.0;
            ++m_jumps;
        } else if (m_on) {
            m_seconds += frame < k_most_frame ? frame : k_most_frame;
            if (m_seconds > k_most_seconds) m_on = false;
        }
    }
    // Each frame while on(), after see: how much of the world is still coming (cells, ground,
    // plants, voxel work: any count that is 0 when all is in; not asked for otherwise, it costs
    // a walk over the tiles).
    void wait(u64 coming) {
        if (!m_on) return;
        m_quiet = coming == 0 ? m_quiet + 1 : 0;
        if (m_quiet >= k_quiet_frames) m_on = false;
    }
    // The world is coming in round a view that jumped.
    bool on() const { return m_on; }
    u64 jumps() const { return m_jumps; } // ever

private:
    std::optional<DVec3> m_view; // last frame's
    bool m_on = false;
    u32 m_quiet = 0;
    double m_seconds = 0.0;
    u64 m_jumps = 0;
};

} // namespace giant::editor
