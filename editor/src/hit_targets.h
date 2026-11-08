// What bullets meet on characters: each visible skinned entity's bone
// capsules (fitted at import, MeshLibrary::Skinned::capsules), posed as it
// is drawn (its animation's pose, or the rest pose when it stands frozen)
// and placed where it is, named by zone from its file's zone map (the
// default rules, or set_hit_zones's for that file). Kept in step each frame
// after the animation, for only what changed (a new pose, an edit), into
// the sim::HitShapes the bullets test (PhysicsState::set_hit_shapes).
//
// View state: zone maps set by command last until the editor closes (the
// game's data will set them as it loads).
#pragma once

#include "animation_state.h"
#include "mesh_library.h"

#include "giant/sim/hit_shapes.h"
#include "giant/world/scene.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace giant::editor {

class HitTargets {
public:
    void update(const world::Scene& scene, const MeshLibrary& meshes, const AnimationState& animation);

    const sim::HitShapes& shapes() const { return m_shapes; }
    // The zone map a file's characters use (normalized path; none set: the default).
    const sim::HitZoneMap& zones(const std::string& mesh) const;
    void set_zones(const std::string& mesh, sim::HitZoneMap map);
    void reset_zones(const std::string& mesh);
    // Names of an entity's zone and bone (empty: unknown).
    std::string zone_name(world::EntityId id, u16 zone) const;
    std::string bone_name(world::EntityId id, u32 bone) const;
    u64 updates() const { return m_updates; } // characters' shapes made again, ever

private:
    struct Fed {
        u64 serial = 0;
        u64 pose = 0;
        std::shared_ptr<const MeshLibrary::Skinned> skinned;
        std::string mesh;
        u64 zones = 0; // m_zone_edits as made
    };
    sim::HitShapes m_shapes;
    std::unordered_map<world::EntityId, Fed> m_fed;
    std::map<std::string, sim::HitZoneMap> m_zones;
    sim::HitZoneMap m_default = sim::default_hit_zones();
    u64 m_zone_edits = 0;
    u64 m_scene = 0;
    u64 m_seen = ~u64{0};   // the animation's serial as last followed
    u64 m_seen_zones = ~u64{0};
    u64 m_updates = 0;
};

} // namespace giant::editor
