#include "hit_targets.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace giant::editor {

namespace {

Vec3 apply(const Mat4& m, const Vec3& p) {
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
            m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

// The most a matrix stretches a length (its largest column).
float stretch(const Mat4& m) {
    float most = 0.0f;
    for (int c = 0; c < 3; ++c) {
        most = std::max(most, std::sqrt(m.at(0, c) * m.at(0, c) + m.at(1, c) * m.at(1, c) + m.at(2, c) * m.at(2, c)));
    }
    return most;
}

DVec3 place(const world::Transform& t, const Vec3& model) {
    const Vec3 scaled{model.x * t.scale.x, model.y * t.scale.y, model.z * t.scale.z};
    const Vec3 r = rotate(t.rotation, scaled);
    return t.position + DVec3{r.x, r.y, r.z};
}

} // namespace

const sim::HitZoneMap& HitTargets::zones(const std::string& mesh) const {
    const auto it = m_zones.find(mesh);
    return it == m_zones.end() ? m_default : it->second;
}

void HitTargets::set_zones(const std::string& mesh, sim::HitZoneMap map) {
    m_zones[mesh] = std::move(map);
    ++m_zone_edits;
}

void HitTargets::reset_zones(const std::string& mesh) {
    if (m_zones.erase(mesh) != 0) ++m_zone_edits;
}

std::string HitTargets::zone_name(world::EntityId id, u16 zone) const {
    const auto it = m_fed.find(id);
    if (it == m_fed.end()) return {};
    const sim::HitZoneMap& map = zones(it->second.mesh);
    return zone < map.rules.size() ? map.rules[zone].zone : std::string{};
}

std::string HitTargets::bone_name(world::EntityId id, u32 bone) const {
    const auto it = m_fed.find(id);
    if (it == m_fed.end() || it->second.skinned == nullptr) return {};
    const auto& names = it->second.skinned->skeleton.names;
    return bone < names.size() ? names[bone] : std::string{};
}

void HitTargets::update(const world::Scene& scene, const MeshLibrary& meshes, const AnimationState& animation) {
    if (scene.identity() != m_scene) {
        m_scene = scene.identity();
        m_shapes.clear();
        m_fed.clear();
        m_seen = ~u64{0};
    }
    // Nothing skinned changed (no pose, edit or file): nothing to do.
    if (animation.serial() == m_seen && m_zone_edits == m_seen_zones) return;
    m_seen = animation.serial();
    m_seen_zones = m_zone_edits;
    const auto& skinned = animation.skinned();
    // Gone (or no longer skinned, visible): their shapes go.
    std::vector<world::EntityId> gone;
    for (const auto& [id, fed] : m_fed) {
        if (!skinned.contains(id)) gone.push_back(id);
    }
    for (const world::EntityId id : gone) {
        m_fed.erase(id);
        m_shapes.remove(id);
    }
    for (const auto& [id, serial] : skinned) {
        const world::Entity* e = scene.find(id);
        if (e == nullptr || e->body == world::BodyKind::none) { // decor is not hit
            if (m_fed.erase(id) != 0) m_shapes.remove(id);
            continue;
        }
        const MeshLibrary::Mesh* mesh = meshes.find_normalized(e->mesh);
        if (mesh == nullptr || mesh->skinned == nullptr) continue;
        const AnimationState::Animated* a = animation.find(id);
        // As drawn: its pose once its copy shows (the rest pose until then).
        const bool posed = a != nullptr && a->posed && (a->drawn || !animation.draws()) && a->skinned == mesh->skinned &&
                           a->model.size() == mesh->skinned->skeleton.size();
        const u64 pose = posed ? a->pose_serial : 0;
        Fed& fed = m_fed[id];
        if (fed.serial == serial && fed.pose == pose && fed.skinned == mesh->skinned && fed.zones == m_zone_edits) continue;
        fed = {serial, pose, mesh->skinned, e->mesh, m_zone_edits};
        const MeshLibrary::Skinned& s = *mesh->skinned;
        const std::vector<Mat4>& model = posed ? a->model : s.rest_model;
        const sim::HitZoneMap& map = zones(e->mesh);
        const float scale = std::max({std::fabs(e->transform.scale.x), std::fabs(e->transform.scale.y),
                                      std::fabs(e->transform.scale.z)});
        sim::HitCharacter c;
        c.capsules.reserve(s.capsules.size());
        for (const anim::BoneCapsule& b : s.capsules) {
            if (b.joint >= model.size()) continue;
            const Mat4& m = model[b.joint];
            sim::HitCapsule h;
            h.a = place(e->transform, apply(m, b.a));
            h.b = place(e->transform, apply(m, b.b));
            h.radius = b.radius * stretch(m) * scale;
            h.bone = b.joint;
            h.zone = b.joint < s.skeleton.names.size() ? map.zone_of(s.skeleton.names[b.joint]) : sim::k_no_zone;
            const bool finite = std::isfinite(h.a.x) && std::isfinite(h.a.y) && std::isfinite(h.a.z) &&
                                std::isfinite(h.b.x) && std::isfinite(h.b.y) && std::isfinite(h.b.z);
            if (finite && std::isfinite(h.radius) && h.radius > 0.0f) c.capsules.push_back(h);
        }
        m_shapes.set(id, std::move(c));
        ++m_updates;
    }
}

void character_occluders(const world::Scene& scene, const MeshLibrary& meshes, const AnimationState& animation,
                         TextureLibrary* textures, const DrawnPoses* moving, const DrawnPoses* walking,
                         const DrawnPoses* swinging, const DVec3& camera, std::vector<render::Occluder>& out) {
    out.clear();
    constexpr double k_reach_from_origin = 5.0; // metres at scale 1: a body's capsules from its origin, at most
    for (const auto& [id, serial] : animation.skinned()) {
        const world::Entity* e = scene.find(id);
        if (e == nullptr || !e->visible) continue;
        // Where it is drawn (as the scene frame's drawn_at), and near enough.
        const world::Transform* drawn = &e->transform;
        for (const DrawnPoses* poses : {moving, walking, swinging}) {
            if (poses == nullptr) continue;
            if (const auto it = poses->find(id); it != poses->end()) {
                drawn = &it->second;
                break;
            }
        }
        const world::Transform& t = *drawn;
        const float scale = std::max({std::fabs(t.scale.x), std::fabs(t.scale.y), std::fabs(t.scale.z)});
        if (!(length(t.position - camera) - k_reach_from_origin * static_cast<double>(scale) <=
              static_cast<double>(render::k_occluder_distance))) {
            continue;
        }
        const MeshLibrary::Mesh* mesh = meshes.find_normalized(e->mesh);
        if (mesh == nullptr || mesh->skinned == nullptr) continue;
        const MeshLibrary::Skinned& s = *mesh->skinned;
        const AnimationState::Animated* a = animation.find(id);
        const bool posed = a != nullptr && a->posed && (a->drawn || !animation.draws()) && a->skinned == mesh->skinned &&
                           a->model.size() == s.skeleton.size();
        const std::vector<Mat4>& model = posed ? a->model : s.rest_model;
        Vec3 albedo = e->material.base_color;
        if (textures != nullptr && mesh->has_uvs && !e->material.maps.base_color.empty()) {
            // (The scene frame asks for the same texture to draw it.)
            TextureLibrary::Key key;
            key.path = e->material.maps.base_color;
            key.slot = TextureLibrary::Slot::base_color;
            const TextureLibrary::Entry* map = textures->entry(textures->request(key));
            if (map != nullptr && map->status == TextureLibrary::Status::ready) {
                const Vec3 m = map->summary.average_at(e->material.alpha_cutoff);
                albedo = {albedo.x * m.x, albedo.y * m.y, albedo.z * m.z};
            }
        }
        for (const anim::BoneCapsule& b : s.capsules) {
            if (b.joint >= model.size()) continue;
            const Mat4& m = model[b.joint];
            render::Occluder o;
            o.a = place(t, apply(m, b.a));
            o.b = place(t, apply(m, b.b));
            o.radius = b.radius * stretch(m) * scale;
            o.albedo = albedo;
            out.push_back(o); // (the renderer leaves out what is not finite)
        }
    }
}

} // namespace giant::editor
