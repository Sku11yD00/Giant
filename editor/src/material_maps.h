// A material's image files over MCP (world::MaterialMaps): the "maps"
// argument entities and scatter items share, and how it reads back.
#pragma once

#include "mesh_library.h"

#include "giant/ai/commands.h"
#include "giant/world/scene.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace giant::editor {

inline ai::Json maps_schema() {
    const auto path = [](const char* description) {
        return ai::Json{{"type", "string"},
                        {"maxLength", world::k_max_mesh_path_bytes},
                        {"description", description}};
    };
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"base_color", path("Colour image (PNG, JPEG, TGA or BMP), multiplying base_color: set "
                                  "base_color to [1, 1, 1] to show it as painted.")},
              {"normal", path("Tangent-space normal map, green up (OpenGL/glTF) unless normal_green_down.")},
              {"roughness_metal", path("glTF metallic-roughness image: roughness in green, metallic in blue, "
                                       "multiplying roughness and metallic (set both to 1 to use it as "
                                       "painted). Filtered with the normal map's detail, so distant "
                                       "bumps do not sparkle.")},
              {"occlusion", path("Ambient occlusion image (red): dims sky, bounce and reflected light in "
                                 "creases, not the sun.")},
              {"normal_green_down", {{"type", "boolean"},
                                     {"description", "The normal map was made for DirectX (green "
                                                     "pointing down the image). Default false."}}}}},
            {"description",
             "Image files drawn over the mesh's uvs (paths from the editor's working folder; \"\" "
             "removes one; fields left out are kept). Only meshes show them (built-in shapes have "
             "no uvs). Cooked in the background the first time they are used, and kept cooked in "
             "giant_cache/textures; the plain values show until then (get_textures)."}};
}

// Applies `json` (maps_schema) to `maps`; returns what is wrong (a file
// that does not exist), or "".
inline std::string apply_maps(const ai::Json& json, world::MaterialMaps& maps) {
    const auto field = [&](const char* key, std::string& out) -> std::string {
        if (!json.contains(key)) return {};
        const std::string given = json[key].get<std::string>();
        if (given.empty()) {
            out.clear();
            return {};
        }
        const std::string path = MeshLibrary::normalize(given);
        std::error_code ec;
        const std::u8string utf8(path.begin(), path.end());
        if (!std::filesystem::is_regular_file(std::filesystem::path(utf8), ec)) {
            return std::string("maps.") + key + ": no file " + path;
        }
        out = path;
        return {};
    };
    for (const auto& [key, out] : {std::pair<const char*, std::string*>{"base_color", &maps.base_color},
                                   {"normal", &maps.normal},
                                   {"roughness_metal", &maps.roughness_metal},
                                   {"occlusion", &maps.occlusion}}) {
        if (std::string bad = field(key, *out); !bad.empty()) return bad;
    }
    maps.normal_green_down = json.value("normal_green_down", maps.normal_green_down);
    return {};
}

inline ai::Json maps_json(const world::MaterialMaps& maps) {
    ai::Json j = ai::Json::object();
    if (!maps.base_color.empty()) j["base_color"] = maps.base_color;
    if (!maps.normal.empty()) j["normal"] = maps.normal;
    if (!maps.roughness_metal.empty()) j["roughness_metal"] = maps.roughness_metal;
    if (!maps.occlusion.empty()) j["occlusion"] = maps.occlusion;
    if (maps.normal_green_down) j["normal_green_down"] = true;
    return j;
}

} // namespace giant::editor
