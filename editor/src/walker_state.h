// The editor's walkers: entities sent walking somewhere (walk_to) while the
// physics simulates, each a physics character (sim/physics.h) that follows
// a path NavState finds (nav/follow.h: corners in turn, slowing at the end,
// stepping round other walkers near). They walk up steps and slopes and
// stop at walls as the player does, and are drawn where they walk (handed
// to the scene's drawing with the swings' poses: moved in place, kept out
// of the voxel world); the scene keeps where they were put.
//
// An entity walks with no body of its own (body "none": else its body
// would stand where it was, and the map would count it in the way). Its
// feet are its origin for meshes and kits, its bottom for cubes and
// spheres; its character is as wide as it (0.2 to 0.45 m round) and, for
// a cube or sphere, as tall. One stuck (moving under a third of what it
// was steered to in 2 s of simulated time) looks for the way again (three
// times; then it has failed). A shot or a ray that meets one names its
// entity (its character's body carries it). Stopping the simulation (or another
// scene) lets every walker go.
//
// View state: nothing of it is saved or undone.
#pragma once

#include "nav_state.h"
#include "physics_state.h"

#include "giant/nav/follow.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace giant::editor {

class WalkerState {
public:
    WalkerState(PhysicsState& physics, NavState& nav);
    ~WalkerState();
    WalkerState(const WalkerState&) = delete;
    WalkerState& operator=(const WalkerState&) = delete;

    enum class Phase : u8 { searching, walking, arrived, failed };
    static const char* to_string(Phase phase);

    struct Walker {
        world::EntityId entity = 0;
        sim::CharacterId character = sim::k_no_character;
        NavState::SearchId search = 0; // while searching
        nav::PathFollower follow;
        nav::FollowSettings settings;
        DVec3 target{};
        Phase phase = Phase::searching;
        std::string reason; // why it failed
        double lift = 0.0;  // its origin above its feet
        Quat rotation{};    // as the scene has it, its turn about y taken out (`yaw` puts one back)
        Vec3 scale{1.0f, 1.0f, 1.0f};
        float yaw = 0.0f;   // radians about +y, where it faces
        Vec3 velocity{};    // as steered
        DVec3 feet{};       // where its feet are (last step)
        DVec3 stuck_from{};
        double stuck_for = 0.0;   // simulated seconds since stuck_from
        double expected = 0.0;    // metres it was steered to walk meanwhile
        float height = 1.8f;      // its character's
        u32 searches = 0; // the way looked for (1 + times stuck)
    };

    static constexpr usize k_max_walkers = 512;
    static constexpr u32 k_max_searches = 4;

    // Sends `id` walking to `to` at `speed` (m/s): along `path` (corners
    // from where it stands, a found search's) when given, else along the
    // way NavState finds over the next frames. Empty: done; else why not.
    std::string walk_to(const world::Scene& scene, world::EntityId id, const DVec3& to, float speed,
                        const std::vector<DVec3>* path = nullptr);
    // Where its feet are now (its character's; else where the scene has it).
    std::optional<DVec3> feet_of(const world::Scene& scene, world::EntityId id) const;
    // Lets it go (drawn where the scene has it again). False: not walking.
    bool stop(world::EntityId id);
    void clear();
    const Walker* find(world::EntityId id) const;
    usize size() const { return m_walkers.size(); }
    const std::unordered_map<world::EntityId, Walker>& walkers() const { return m_walkers; }

    // Each frame (and fixed step) before the physics steps: searches taken,
    // each walker steered. Walkers whose entity went (or the simulation
    // stopped, or another scene) are let go.
    void before_step(const world::Scene& scene, double seconds);
    // After it steps: where each is drawn (between the last two steps).
    void after_step();

    const std::unordered_map<world::EntityId, world::Transform>& poses() const { return m_poses; }
    // Moved since the last call (moved in place); posed or let go since
    // (their cells built again: drawn as moving, or not).
    std::vector<world::EntityId> take_moved();
    std::vector<world::EntityId> take_redraw();

    // Others to keep clear of besides the walkers (the player), set each
    // frame by its owner.
    std::vector<nav::Neighbour> others;

private:
    void drop(world::EntityId id);
    bool make_character(Walker& w);

    PhysicsState& m_physics;
    NavState& m_nav;
    u64 m_scene = 0;
    u64 m_run = 0;   // the physics' run they were made in
    u64 m_steps = 0; // the physics' fixed steps, as last seen
    std::unordered_map<world::EntityId, Walker> m_walkers;
    std::unordered_map<world::EntityId, world::Transform> m_poses;
    std::vector<world::EntityId> m_moved;
    std::vector<world::EntityId> m_redraw;
    std::vector<nav::Neighbour> m_near; // scratch
    std::unordered_map<world::CellCoord, std::vector<world::EntityId>, world::CellCoordHash> m_grid; // scratch: by 2 m square
};

} // namespace giant::editor
