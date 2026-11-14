// Effects (effects plan FX2): what particles look like (ParticleLook, the
// renderer's ParticleStyle by name), and named effects made of emitters
// that give birth to them: chimney smoke, a torch's fire, sparks, mist off
// a waterfall. Plain data, saved with the scene and the world; an entity
// runs one where it stands (EntityEffect), and the editor spawns them.
//
// A built-in library (smoke, fire (with its glow), sparks, dust_kick, spray, mist, embers,
// debris_chips, splinters, leaves, rain_splash, grit) is always there, a look and an effect of each name; a
// scene's own of the same name stands in for the built-in one.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace giant::world {

// Puffs are soft round blots (smoke, dust, spray, fire); streaks stretch
// along their motion (sparks, chips).
enum class ParticleForm : u8 { puff, streak };
const char* to_string(ParticleForm form);
std::optional<ParticleForm> particle_form_from_string(std::string_view name);

constexpr usize k_max_particle_looks = 192; // a scene's own (with the built-ins, within the renderer's 256 styles)
constexpr usize k_max_effects = 1024;
constexpr usize k_max_emitters = 16;        // an effect's
constexpr usize k_max_effect_name_bytes = 128;
constexpr float k_max_look_life = 60.0f;      // seconds (render::k_max_particle_life)
constexpr float k_max_look_size = 1000.0f;    // metres: sizes, wobble and softness
constexpr float k_max_look_light = 1.0e4f;    // emissive
constexpr float k_max_look_pull = 100.0f;     // gravity and wind shares; buoyancy: that many g
constexpr float k_max_look_buoyancy = k_max_look_pull * 9.81f; // m/s^2, either way
constexpr float k_max_look_drag = 100.0f;     // 1/s
constexpr float k_max_emitter_rate = 10000.0f;  // particles a second
constexpr u32 k_max_emitter_burst = 16384;      // particles at once (render::k_max_particles_born)
constexpr float k_max_emitter_speed = 1000.0f;  // m/s
constexpr float k_max_emitter_reach = 500.0f;   // metres: an emitter's size (a box's corner within the renderer's 1 km spread) and offset
constexpr float k_max_effect_seconds = 3600.0f; // how long a spawned one runs its rates
constexpr float k_max_effect_rate_scale = 100.0f;
constexpr float k_max_effect_light_intensity = 1.0e4f;
constexpr float k_max_effect_light_range = 1000.0f; // metres

// How particles look and move (render::ParticleStyle, field for field).
struct ParticleLook {
    std::string name;      // unique among the scene's looks
    float life_min = 1.0f; // seconds, each between (0 < min <= max <= k_max_look_life)
    float life_max = 2.0f;
    float size_start = 0.2f; // metres across at birth and at the end
    float size_end = 0.6f;
    Vec3 color_start{0.5f, 0.5f, 0.5f}; // what it gives back of the light, 0 to 1
    Vec3 color_end{0.5f, 0.5f, 0.5f};
    float opacity_start = 1.0f; // 0 to 1
    float opacity_end = 0.0f;
    Vec3 emissive{};            // light of its own, 0 to k_max_look_light
    float emissive_end = 1.0f;  // the share of it left at the end, 0 to 1
    float gravity = 1.0f;       // the share of gravity that pulls it, -100 to 100
    float buoyancy = 0.0f;      // m/s^2 up (smoke, fire)
    float drag = 0.0f;          // 1/s: how fast it takes the air's speed
    float wind = 1.0f;          // the share of the wind that air moves with
    float wobble = 0.0f;        // metres it wanders off its path
    ParticleForm form = ParticleForm::puff;
    bool lit = true;            // lit by sun, sky and lamps (false: its emissive only)
    float soft = 0.3f;          // metres it fades into what it meets
    bool operator==(const ParticleLook&) const = default;
};

// Where an emitter's particles start: at its point, inside a sphere
// (size.x the radius), inside a box (size the half extents) or on a flat
// disc across the effect's own X and Z (size.x the radius).
enum class EmitterShape : u8 { point, sphere, box, disc };
const char* to_string(EmitterShape shape);
std::optional<EmitterShape> emitter_shape_from_string(std::string_view name);

// One stream of particles of one look. Directions and offsets are in the
// effect's own space (an entity's turn applies; its scale does not).
struct Emitter {
    std::string look;          // a ParticleLook by name (the scene's, else built-in)
    float rate = 10.0f;        // a second while it runs, 0 to k_max_emitter_rate
    u32 burst = 0;             // at once when it starts, 0 to k_max_emitter_burst
    EmitterShape shape = EmitterShape::point;
    Vec3 size{};               // metres (see EmitterShape), 0 to k_max_emitter_reach
    Vec3 offset{};             // metres from the effect's origin
    Vec3 direction{0.0f, 1.0f, 0.0f}; // not zero
    float cone_degrees = 30.0f;       // within this of `direction`, 0 to 180 (any way)
    float speed_min = 1.0f;           // m/s, 0 <= min <= max <= k_max_emitter_speed
    float speed_max = 2.0f;
    float size_scale = 1.0f;          // times the look's sizes, over 0 to 1000
    bool ground = true;               // stops where it falls to the ground
    bool operator==(const Emitter&) const = default;
};

// The light an effect gives while it runs (a fire's glow on what is round
// it), as a point lamp at `offset` in the effect's own space. `flicker`
// dims it by up to that share, wavering with the effects' running clock
// (game seconds summed while they run; not saved): the same at the same
// time (no noise frame to frame), each running one its own way.
struct EffectLight {
    bool on = false;
    Vec3 color{1.0f, 0.55f, 0.25f}; // linear, 0 to 1 (firelight)
    float intensity = 4.0f;         // its light 1 m away (as a lamp's), 0 to k_max_effect_light_intensity
    float range = 8.0f;             // metres, 0.1 to k_max_effect_light_range
    Vec3 offset{0.0f, 0.5f, 0.0f};  // metres, within k_max_emitter_reach
    float flicker = 0.3f;           // 0 (steady) to 1
    float source_radius = 0.2f;     // metres, 0 to 10
    bool shadows = false;           // casts shadows (with the lamps: the nearest few in view)
    float shadow_near = 0.1f;       // metres, 0.01 to 10
    bool operator==(const EffectLight&) const = default;
};

struct Effect {
    std::string name;              // unique among the scene's effects
    std::vector<Emitter> emitters; // 1 to k_max_emitters
    // Spawned (not on an entity): seconds it runs its rates after its
    // bursts (0: bursts only). An entity's runs while it is on.
    float duration = 0.0f;
    EffectLight light;             // off unless set
    bool operator==(const Effect&) const = default;
};

// The share of an effect light's intensity it gives at `seconds` of the
// effects' running clock: 1 - flicker * a waver from 0 to 1 made of three sines whose
// phases come from `seed` (the running effect's). From 1 - flicker to 1.
float effect_flicker(float flicker, double seconds, u64 seed);

// An entity running an effect where it stands (any shape; hidden entities
// run none). Kept while off.
struct EntityEffect {
    bool on = false;
    std::string effect;  // by name (the scene's, else built-in)
    float rate = 1.0f;   // times its emitters' rates, 0 to k_max_effect_rate_scale
    bool operator==(const EntityEffect&) const = default;
};

// What is wrong with one, for people and the AI (none: it is fine).
std::optional<std::string> check_particle_look(const ParticleLook& look);
std::optional<std::string> check_emitter(const Emitter& emitter);
std::optional<std::string> check_effect_light(const EffectLight& light);
std::optional<std::string> check_effect(const Effect& effect);
std::optional<std::string> check_entity_effect(const EntityEffect& effect);
// A name the scene's looks and effects may have: 1 to k_max_effect_name_bytes bytes.
bool effect_name_ok(std::string_view name);

// The built-in library, made once (stable for the process).
std::span<const ParticleLook> built_in_particle_looks();
std::span<const Effect> built_in_effects();

// By name: the scene's own first, else the built-in one (null: neither).
const ParticleLook* find_particle_look(std::span<const ParticleLook> scene, std::string_view name);
const Effect* find_effect(std::span<const Effect> scene, std::string_view name);

} // namespace giant::world
