#include "scene_render.h"

#include <cstring>

namespace giant::editor {

namespace {

render::BuiltinMesh builtin(world::Shape shape) {
    switch (shape) {
    case world::Shape::sphere: return render::BuiltinMesh::sphere;
    case world::Shape::plane: return render::BuiltinMesh::plane;
    default: break;
    }
    return render::BuiltinMesh::cube;
}

u32 material_index(const world::Material& m, SceneFrame& out) {
    SceneFrame::MaterialKey key{};
    const float values[8] = {m.base_color.x, m.base_color.y, m.base_color.z, m.roughness,
                             m.metallic,     m.emissive.x,   m.emissive.y,   m.emissive.z};
    std::memcpy(key.bits, values, sizeof(values));
    const auto [it, added] = out.lookup.try_emplace(key, static_cast<u32>(out.materials.size()));
    if (added) {
        render::Material r;
        r.base_color = m.base_color;
        r.roughness = m.roughness;
        r.metallic = m.metallic;
        r.emissive = m.emissive;
        out.materials.push_back(r);
    }
    return it->second;
}

render::DecalShape decal_shape(world::DecalShape s) {
    switch (s) {
    case world::DecalShape::disc: return render::DecalShape::disc;
    case world::DecalShape::rectangle: return render::DecalShape::rectangle;
    case world::DecalShape::ring: return render::DecalShape::ring;
    case world::DecalShape::splat: return render::DecalShape::splat;
    case world::DecalShape::cracks: return render::DecalShape::cracks;
    }
    return render::DecalShape::disc;
}

// One drawable thing (an entity, or a piece of a placed kit).
void emit(world::Shape shape, const std::string& mesh_path, const world::Transform& t, const world::Material& m,
          const world::DecalSettings& d, const render::Renderer& renderer, const MeshLibrary& meshes,
          SceneFrame& out) {
    if (shape == world::Shape::decal) {
        render::Decal decal;
        decal.position = t.position;
        decal.rotation = t.rotation;
        decal.size = t.scale;
        decal.base_color = m.base_color;
        decal.roughness = m.roughness;
        decal.metallic = m.metallic;
        decal.emissive = m.emissive;
        decal.opacity = d.opacity;
        decal.softness = d.softness;
        decal.shape = decal_shape(d.shape);
        decal.seed = d.seed;
        out.decals.push_back(decal);
        return;
    }
    render::MeshId mesh = render::k_no_mesh;
    if (shape == world::Shape::mesh) {
        const MeshLibrary::Mesh* found = meshes.find_normalized(mesh_path);
        if (found == nullptr || found->id == render::k_no_mesh) {
            return;
        }
        mesh = found->id;
    } else if (shape == world::Shape::kit) {
        return; // kits do not nest
    } else {
        mesh = renderer.builtin(builtin(shape));
    }
    render::Instance i;
    i.position = t.position;
    i.rotation = t.rotation;
    i.scale = t.scale;
    i.mesh = mesh;
    i.material = material_index(m, out);
    out.instances.push_back(i);
}

} // namespace

usize SceneFrame::MaterialHash::operator()(const MaterialKey& k) const {
    u64 h = 1469598103934665603ull;
    for (const u32 b : k.bits) {
        h = (h ^ b) * 1099511628211ull;
    }
    return static_cast<usize>(h);
}

world::Transform place(const world::Transform& parent, const world::Transform& child) {
    world::Transform out;
    // Pieces sit within a few hundred metres of their kit: float is plenty.
    const Vec3 scaled{static_cast<float>(child.position.x) * parent.scale.x,
                      static_cast<float>(child.position.y) * parent.scale.y,
                      static_cast<float>(child.position.z) * parent.scale.z};
    const Vec3 offset = rotate(parent.rotation, scaled);
    out.position = parent.position + DVec3{offset.x, offset.y, offset.z};
    out.rotation = normalize(parent.rotation * child.rotation);
    out.scale = {parent.scale.x * child.scale.x, parent.scale.y * child.scale.y, parent.scale.z * child.scale.z};
    return out;
}

void scene_frame(const world::Scene& scene, const render::Renderer& renderer, const MeshLibrary& meshes,
                 SceneFrame& out) {
    out.instances.clear();
    out.materials.clear();
    out.decals.clear();
    out.lookup.clear();
    out.instances.reserve(scene.size());
    for (const world::Entity& e : scene.entities()) {
        if (!e.visible) {
            continue;
        }
        if (e.shape != world::Shape::kit) {
            emit(e.shape, e.mesh, e.transform, e.material, e.decal, renderer, meshes, out);
            continue;
        }
        const world::Kit* kit = scene.find_kit(e.kit);
        if (kit == nullptr) {
            continue;
        }
        for (const world::KitPiece& p : kit->pieces) {
            emit(p.shape, p.mesh, place(e.transform, p.transform), p.material, p.decal, renderer, meshes, out);
        }
    }
}

render::Lighting scene_lighting(const world::Lighting& l) {
    render::Lighting out;
    out.sun_direction = world::sun_direction(l);
    // Below the horizon the sun stops lighting (night); ambient remains.
    const float sun = l.sun_elevation_degrees > -2.0f ? l.sun_intensity : 0.0f;
    out.sun_light = l.sun_color * sun;
    out.sky_light = l.sky_color * l.ambient_intensity;
    out.ground_light = l.ground_color * l.ambient_intensity;
    out.background = l.sky_color;
    out.exposure = l.exposure;
    return out;
}

} // namespace giant::editor
