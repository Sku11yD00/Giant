// Hands the scene to the renderer as plain data (the renderer never sees
// world types).
#pragma once

#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <vector>

namespace giant::editor {

// Fills `instances` (reused each frame) with the scene's visible entities.
void scene_instances(const world::Scene& scene, const render::Renderer& renderer,
                     std::vector<render::Instance>& instances);

render::Lighting scene_lighting(const world::Lighting& lighting);

} // namespace giant::editor
