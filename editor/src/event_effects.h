// Effects the world's events set off (effects plan FX3), as spawned
// effects (EffectsState::spawn), so within its cap and budgets: a shot's
// hit throws up what its surface is made of (sparks off metal, chips and
// dust off stone and brick, splinters off wood, leaves off foliage, dust
// off dirt; nothing off flesh), turned to the surface; things going into
// water throw spray, more the bigger and faster; a collapse throws chips,
// a felled tree leaves, a crater earth (with the dust clouds sim::Dust
// draws); the player running on dry ground kicks dust every few strides.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/sim/physics.h"
#include "giant/world/scene.h"
#include "giant/world/weather.h"

#include <optional>

namespace giant::editor {

struct EditorState;

// What a bullet leaving `energy` joules in a surface throws up: up to two
// effects (null: none) and how much of their bursts.
struct HitEffect {
    const char* effect = nullptr;
    const char* second = nullptr;
    float bursts = 1.0f;
    float second_bursts = 1.0f;
};
HitEffect hit_effect(world::Surface surface, float energy);

// The turn that takes +y to `normal` (unit; the effects' up to the surface's).
Quat turn_up_to(const Vec3& normal);

// Chained after what is there on the physics' on_hits and on_splashes
// (after connect_blows, which sets on_hits, and connect_sounds; with the
// effects there; once only: called again it does nothing).
// The player's running feet: `walked` (metres since the last step that
// kicked dust) runs on with its speed over the ground while running (3 m/s
// or more) on dry bare ground (`under`, what it stands on, dirt: the
// terrain, or a thing made of it); each 1.6 m, where the dust goes (its
// feet). Off the ground, on anything else, carried, climbing or in water
// starts it over.
std::optional<DVec3> running_step(double& walked, const sim::CharacterState& player, world::Surface under,
                                  const world::Ground& ground, double seconds);

void connect_event_effects(EditorState& state);
// Each frame, before the effects update: the dust thrown up since the last
// (taken), and the player's running feet over `seconds` of the game.
void event_effects_frame(EditorState& state, double seconds);

} // namespace giant::editor
