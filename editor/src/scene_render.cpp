#include "scene_render.h"

namespace giant::editor {

namespace {
render::BuiltinMesh builtin(world::Shape shape) {
    switch (shape) {
    case world::Shape::cube: return render::BuiltinMesh::cube;
    case world::Shape::sphere: return render::BuiltinMesh::sphere;
    case world::Shape::plane: return render::BuiltinMesh::plane;
    case world::Shape::mesh: break; // handled by the caller
    }
    return render::BuiltinMesh::cube;
}
} // namespace

void scene_instances(const world::Scene& scene, const render::Renderer& renderer,
                     const MeshLibrary& meshes, std::vector<render::Instance>& instances) {
    instances.clear();
    instances.reserve(scene.size());
    for (const world::Entity& e : scene.entities()) {
        if (!e.visible) {
            continue;
        }
        render::MeshId mesh = render::k_no_mesh;
        if (e.shape == world::Shape::mesh) {
            const MeshLibrary::Mesh* m = meshes.find(e.mesh);
            if (m == nullptr || m->id == render::k_no_mesh) {
                continue;
            }
            mesh = m->id;
        } else {
            mesh = renderer.builtin(builtin(e.shape));
        }
        render::Instance i;
        i.position = e.transform.position;
        i.rotation = e.transform.rotation;
        i.scale = e.transform.scale;
        i.mesh = mesh;
        i.base_color = e.material.base_color;
        i.roughness = e.material.roughness;
        i.metallic = e.material.metallic;
        i.emissive = e.material.emissive;
        instances.push_back(i);
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
