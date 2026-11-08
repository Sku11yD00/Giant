// Following a path (nav.h's PathSearch) as a person walks it: towards the
// next corner at a walking speed, slowing to stop at the end, and keeping
// clear of others walking near (each steers a little to its right of one
// coming at it, and away from any too close), so a crowd passes in a
// corridor without a planner of its own. Pure: the caller moves the walker
// (a physics character) with the velocity, and says where it is now.
//
//   nav::PathFollower f;
//   f.set_path(search.path());
//   const nav::Steering s = f.steer(position, neighbours, settings); // each step
//   character.velocity = s.velocity;
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <span>
#include <vector>

namespace giant::nav {

struct FollowSettings {
    float speed = 1.4f;         // metres a second (a walk; 3 a jog; 6 a run)
    float radius = 0.35f;       // the walker's own, kept from others' (theirs added)
    float corner = 0.6f;        // metres from a corner it turns for the next
    float arrive = 0.25f;       // metres from the end it stops
    float slow = 1.2f;          // metres from the end it starts to slow
    float avoid = 1.5f;         // metres past touching others are steered round
};

// Another walker near.
struct Neighbour {
    DVec3 position{};
    Vec3 velocity{};
    float radius = 0.35f;
};

struct Steering {
    Vec3 velocity{}; // over the ground (y 0)
    bool arrived = false;
};

class PathFollower {
public:
    // Corners (PathSearch::path): the first where it starts.
    void set_path(std::vector<DVec3> path);
    void clear() { m_path.clear(); m_corner = 0; }
    // The velocity to walk at, from `at` (its feet), keeping clear of `near`
    // (at most the few nearest are looked at: rule 9).
    Steering steer(const DVec3& at, std::span<const Neighbour> near, const FollowSettings& settings);
    const std::vector<DVec3>& path() const { return m_path; }
    usize corner() const { return m_corner; } // the one walked to now
    bool empty() const { return m_path.empty(); }
    // Metres left along the path from `at`.
    double remaining(const DVec3& at) const;

    static constexpr usize k_max_neighbours = 8;

private:
    std::vector<DVec3> m_path;
    usize m_corner = 0;
};

} // namespace giant::nav
