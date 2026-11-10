// A material's image files (world::Material::maps, TerrainLayer::maps).
#pragma once

#include "giant/core/types.h"

#include <string>

namespace giant::world {

// Longest path a scene stores for a file (a mesh, a map).
constexpr usize k_max_mesh_path_bytes = 1024;

// Image files a material draws over its mesh's uvs (paths as Entity::mesh,
// k_max_mesh_path_bytes each; empty for none), cooked into textures by the
// editor (giant/texture) and combined as glTF combines its maps:
// base_color (sRGB) multiplies Material::base_color; normal is a tangent-
// space normal map, green up unless normal_green_down (DirectX-authored);
// roughness_metal is a glTF metallic-roughness image (roughness in G,
// metallic in B) multiplying roughness and metallic, filtered with the
// normal map's detail; occlusion is a glTF occlusion image (R), dimming
// ambient and reflected light. Drawn on built-in shapes and on meshes whose
// file has uvs; others draw the plain values.
struct MaterialMaps {
    std::string base_color;
    std::string normal;
    std::string roughness_metal;
    std::string occlusion;
    bool normal_green_down = false;
    bool empty() const {
        return base_color.empty() && normal.empty() && roughness_metal.empty() && occlusion.empty();
    }
    bool operator==(const MaterialMaps&) const = default;
};

} // namespace giant::world
