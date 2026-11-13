// The scene's effects given birth to (effects plan FX2b): entities whose
// effect is on run its emitters' rates where they stand, and spawned
// effects (spawn_effect) give their bursts at once and run their rates a
// while. Each update turns them into this frame's particle bursts and
// styles for the renderer (render::FrameInput::particle_bursts).
//
// Within budgets, nearest first: effects farther than `reach` from the
// camera run nothing, and past `births_per_frame` particles a frame (or
// `grounded_per_frame` that stop on the ground) the farther ones' are left
// out (counted). Births are deterministic: a rate's fractions carry from
// frame to frame, and each burst's seed comes from the entity (or spawn),
// the emitter and a count of updates. Nothing is made on the frame path:
// the lists are made once, at their caps.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "giant/world/effects.h"
#include "giant/world/scene.h"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace giant::editor {

// A look as the renderer draws it.
render::ParticleStyle to_style(const world::ParticleLook& look);

class EffectsState {
public:
    static constexpr u32 k_max_bursts = 4096;             // a frame's (past it: left out)
    static constexpr u32 k_max_spawned = 256;             // spawned effects running at once
    static constexpr u32 k_max_bursts_per_emitter = 64;   // a box or disc emitter's a frame (each its own point)
    static constexpr double k_default_reach = 1000.0;     // metres

    // The ground's height at (x, z), or none (particles that stop on the ground then fall on).
    using Ground = std::function<std::optional<double>(double x, double z)>;

    struct Stats {
        u32 running = 0;      // effects that gave birth this update (entities' and spawned)
        u32 far = 0;          // entities' left out: farther than reach
        u32 spawned = 0;      // spawned ones still running
        u32 bursts = 0;
        u32 born = 0;         // particles asked for
        u32 cut = 0;          // particles left out: past the budgets or the burst cap
        u32 missing = 0;      // emitters (or entity effects) naming a look (an effect) found nowhere; spawned ones whose effect went
    };

    // Effects the world's events set off (event_effects.h), in all.
    struct EventCounts {
        u64 hits = 0;     // shots' hits
        u64 splashes = 0; // things going into water
        u64 dust = 0;     // collapses, felled trees, craters
        u64 steps = 0;    // the player's running feet
        u64 refused = 0;  // left out: the spawned cap
    };
    EventCounts events;
    double walked = 0.0; // metres the player ran since its last step kicked dust
    bool events_connected = false; // connect_event_effects done (once only)

    double reach = k_default_reach;
    u32 births_per_frame = render::k_max_particles_born / 2;       // the rest for events (FX3)
    u32 grounded_per_frame = render::k_max_particles_grounded / 2;

    EffectsState();

    // The scene's changes followed (entities whose effect is on, visible,
    // added, changed or gone). Call before update with the scene's untaken changes.
    void follow(const world::Scene& scene, const world::SceneChanges& changed);
    // Steps `seconds` (0: nothing born, as paused) and builds this frame's
    // bursts and styles from the scene's entity effects and the spawned ones.
    void update(const world::Scene& scene, const DVec3& camera, double seconds, const Ground& ground);
    // A one-shot effect (the scene's, else built-in) at `position`, turned
    // by `rotation`, its rates times `rate` and its bursts times `bursts`
    // (0 to k_max_effect_rate_scale): its bursts on the next update, then
    // its rates for its duration. Null: no such effect, bad numbers, or the cap.
    std::optional<u32> spawn(const world::Scene& scene, std::string_view effect, const DVec3& position,
                             const Quat& rotation = {}, float rate = 1.0f, float bursts = 1.0f);
    // Everything spawned forgotten, carries reset; the next follow looks over
    // every entity again.
    void clear();

    // This frame's bursts, marked taken: births not taken (a frame not
    // drawn) are kept and given with the next update's.
    std::span<const render::ParticleBurst> take() {
        m_taken = true;
        return m_bursts;
    }
    std::span<const render::ParticleBurst> bursts() const { return m_bursts; }
    // A look's style index stays its own while the look is there (alive
    // particles keep theirs): changed in place, new names in free slots, a
    // removed name's slot reused only when the table is full.
    std::optional<u32> style_index(std::string_view look) const;
    std::span<const render::ParticleStyle> styles() const { return m_styles; }
    const Stats& stats() const { return m_stats; }

private:
    struct Runner {
        std::array<float, world::k_max_emitters> carry{}; // fractions of a particle owed, by emitter
    };
    struct Spawned {
        u32 id = 0;
        // Its effect, by name (an edit while it runs is followed; gone, it
        // ends): held in place, so spawning never allocates.
        std::array<char, world::k_max_effect_name_bytes> name{};
        u8 name_bytes = 0;
        std::string_view effect() const { return {name.data(), name_bytes}; }
        DVec3 position{};
        Quat rotation{};
        float rate = 1.0f;
        float burst_scale = 1.0f;
        double left = 0.0; // seconds of its rates left
        bool burst = true; // its bursts still owed
        std::array<float, world::k_max_emitters> carry{};
    };
    struct Job { // one effect to run this update
        double distance2 = 0.0;
        const world::Effect* effect = nullptr;
        DVec3 position{};
        Quat rotation{};
        float rate = 1.0f;
        double rate_seconds = 0.0; // of its rates this update (a spawn's last: what is left)
        float burst_scale = 1.0f;
        float* carry = nullptr;
        u64 key = 0;       // the entity's id, or a spawn's
        bool bursts = false;
        bool rates = true;
    };

    void follow_one(const world::Scene& scene, world::EntityId id);
    void spawned_in(const world::Scene& scene); // another scene's spawned ones forgotten
    void restyle(const world::Scene& scene);
    void reindex_effects(const world::Scene& scene);
    const world::Effect* effect_named(const world::Scene& scene, std::string_view name) const;
    void run(const Job& job, const Ground& ground);
    void emit(const world::Emitter& e, u32 style, u32 count, const Job& job, u32 emitter, const Ground& ground);

    std::vector<render::ParticleBurst> m_bursts;
    std::vector<render::ParticleStyle> m_styles;
    std::vector<world::ParticleLook> m_looks;           // the scene's, as the styles were made from
    struct NameHash {
        using is_transparent = void;
        usize operator()(std::string_view name) const { return std::hash<std::string_view>{}(name); }
    };
    std::unordered_map<std::string, u32, NameHash, std::equal_to<>> m_style_index; // look name -> style (removed ones' kept)
    std::vector<u64> m_style_gone;                      // by style: the update its look went (0: there)
    std::vector<world::Effect> m_effects;               // the scene's, as the index was made from
    std::unordered_map<std::string, i32, NameHash, std::equal_to<>> m_effect_index; // effect name -> the scene's (>= 0) or built-in (-1 - i)
    std::unordered_map<world::EntityId, Runner> m_runners;
    std::vector<Spawned> m_spawned;
    std::vector<Job> m_jobs;
    u64 m_scene = 0;       // identity the runners follow
    u64 m_spawn_scene = 0; // identity the spawned ones were spawned in
    u64 m_updates = 0;
    u32 m_next_spawn = 1;
    u32 m_born = 0;
    u32 m_grounded = 0;
    bool m_taken = true;
    Stats m_stats;
};

} // namespace giant::editor
