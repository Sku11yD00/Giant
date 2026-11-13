// What went in the water looks wet a while after (water plan W5f): each
// body and character the physics finds with some of it under water is
// remembered with how far up it got wet, and drawn with a wet decal (a
// box in its own frame, from its bottom to that line, following it) that
// fades as it dries.
//
// A body's wet line is along the side of it that was up when it went in
// (a crate floating level is wet to its waterline); one turned over in
// the water (that side no longer up) is wet all over. Out of the water
// it dries in dry_seconds of game time; asleep it stays as it was (a
// body at rest on the bed, or floating in a lake, stays in the water).
#pragma once

#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "giant/sim/physics.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace giant::editor {

class WetThings {
public:
    static constexpr usize k_most = 256;        // remembered at once (the driest out of the water make way)
    static constexpr float k_dry_seconds = 90.0f; // from wet to dry, out of the water

    struct Wet {
        sim::BodyId body = sim::k_no_body;
        sim::CharacterId character = sim::k_no_character;
        u8 axis = 2;         // a body's side that was up: +x, -x, +y, -y, +z, -z (characters: +y)
        float line = 0.0f;   // how far up it is wet, 0 (none) to 1 (all of it)
        float amount = 0.0f; // how wet: 1 in the water, down to 0 (dry: forgotten)
        bool in_water = false;
        u64 seen = 0; // the after_step call it was last in the water in
    };

    // After a step() call that ran `seconds` of game time: what is in the
    // water now, what came out, and what dried.
    void after_step(const sim::PhysicsWorld& physics, double seconds);
    // A wet decal for each, where its body or character is drawn now (a
    // character only if it has a body to see: not the player).
    void decals(const sim::PhysicsWorld& physics, std::vector<render::Decal>& out) const;
    void clear() { m_things.clear(); }

    usize size() const { return m_things.size(); }
    usize in_water() const;
    // (Unordered.)
    const std::unordered_map<u64, Wet>& things() const { return m_things; }

private:
    void soak(const sim::PhysicsWorld& physics, const sim::InWater& w);

    std::unordered_map<u64, Wet> m_things; // by body, or character in the top half
    u64 m_calls = 0;
    bool m_full = false;              // this call: full of things in the water
    std::vector<sim::InWater> m_list; // (kept: no allocation each call)
};

} // namespace giant::editor
