// A material's image files over MCP (world::MaterialMaps): the "maps"
// argument entities and scatter items share, and how it reads back.
#pragma once

#include "mesh_library.h"
#include "texture_library.h"

#include "giant/ai/commands.h"
#include "giant/world/scene.h"

#include <filesystem>
#include <optional>
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
             "Image files drawn over the uvs (paths from the editor's working folder, or an image "
             "inside a glTF as \"file.glb#image2\"; \"\" "
             "removes one; fields left out are kept). Built-in shapes and meshes whose file has "
             "uvs (list_meshes: uvs) show them; others draw the plain values. Cooked in the "
             "background the first time they are drawn (at most 4096 texels a side; sharp levels "
             "stream in where drawn up close) and kept "
             "cooked in giant_cache/textures under the working folder; the plain values show "
             "until then (get_textures)."}};
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
        std::string file = path; // an image inside a glTF ("x.glb#image2"): the glTF
        u32 image = 0;
        TextureLibrary::split_image_ref(path, file, image);
        std::error_code ec;
        const std::u8string utf8(file.begin(), file.end());
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

// A material's skin over MCP (world::SkinLook): the "skin" argument
// entities and scatter items share, and how it reads back.
inline ai::Json skin_schema() {
    const auto number = [](double lo, double hi, const char* description) {
        return ai::Json{{"type", "number"}, {"minimum", lo}, {"maximum", hi}, {"description", description}};
    };
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"on", {{"type", "boolean"},
                      {"description", "false: not skin any more (its values dropped). Default true."}}},
              {"scatter", number(0.0005, 0.1, "How far light spreads under it at its widest, metres (red "
                                              "furthest, blue least). Default 0.012: a face.")},
              {"strength", number(0, 1, "How much of its diffuse light spreads. Default 1.")},
              {"lobe_roughness",
               {{"type", "array"},
                {"items", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 4}}},
                {"minItems", 2},
                {"maxItems", 2},
                {"description", "Its highlight's two lobes, each the roughness times this: a sharp sheen "
                                "and a broad soft one. Default [0.75, 1.3]."}}},
              {"lobe_mix", number(0, 1, "How much of the highlight the second (broad) lobe is. Default 0.15.")},
              {"transmission", number(0, 1, "How much light from the sun or a lamp casting shadows passes "
                                            "through its thin parts (ears, fingers), reddened. Default 1.")},
              {"detail_normal", {{"type", "string"},
                                 {"maxLength", world::k_max_mesh_path_bytes},
                                 {"description", "A fine normal map (pores, wrinkles; green up unless "
                                                 "detail_green_down) tiled over the uvs on top of the "
                                                 "material's own normal map; \"\" for none (the default)."}}},
              {"detail_tiling", number(1, 256, "How many times the detail map repeats across the uvs. "
                                               "Default 16.")},
              {"detail_green_down", {{"type", "boolean"},
                                     {"description", "The detail map was made for DirectX. Default false."}}}}},
            {"description",
             "Skin (characters): its light spreads under the surface and passes through thin parts, "
             "with a two-lobe highlight (render setting subsurface_scattering spreads it). Fields "
             "left out keep their value (the defaults when it was not skin). Not on the ground."}};
}

// Applies `json` (skin_schema) to `skin`; returns what is wrong (a detail
// map file that does not exist), or "".
inline std::string apply_skin(const ai::Json& json, std::optional<world::SkinLook>& skin) {
    if (!json.value("on", true)) {
        skin.reset();
        return {};
    }
    world::SkinLook s = skin.value_or(world::SkinLook{});
    s.scatter = json.value("scatter", s.scatter);
    s.strength = json.value("strength", s.strength);
    if (const auto lobes = json.find("lobe_roughness"); lobes != json.end()) {
        s.lobe_roughness[0] = (*lobes)[0].get<float>();
        s.lobe_roughness[1] = (*lobes)[1].get<float>();
    }
    s.lobe_mix = json.value("lobe_mix", s.lobe_mix);
    s.transmission = json.value("transmission", s.transmission);
    s.detail_tiling = json.value("detail_tiling", s.detail_tiling);
    s.detail_green_down = json.value("detail_green_down", s.detail_green_down);
    if (const auto detail = json.find("detail_normal"); detail != json.end()) {
        const std::string given = detail->get<std::string>();
        if (given.empty()) {
            s.detail_normal.clear();
        } else {
            const std::string path = MeshLibrary::normalize(given);
            std::string file = path;
            u32 image = 0;
            TextureLibrary::split_image_ref(path, file, image);
            std::error_code ec;
            const std::u8string utf8(file.begin(), file.end());
            if (!std::filesystem::is_regular_file(std::filesystem::path(utf8), ec)) {
                return "skin.detail_normal: no file " + path;
            }
            s.detail_normal = path;
        }
    }
    skin = std::move(s);
    return {};
}

inline ai::Json skin_json(const world::SkinLook& s) {
    ai::Json j = {{"scatter", s.scatter},
                  {"strength", s.strength},
                  {"lobe_roughness", ai::Json::array({s.lobe_roughness[0], s.lobe_roughness[1]})},
                  {"lobe_mix", s.lobe_mix},
                  {"transmission", s.transmission},
                  {"detail_tiling", s.detail_tiling}};
    if (!s.detail_normal.empty()) j["detail_normal"] = s.detail_normal;
    if (s.detail_green_down) j["detail_green_down"] = true;
    return j;
}

// A material's eye over MCP (world::EyeLook): the "eye" argument entities
// and scatter items share, and how it reads back.
inline ai::Json eye_schema() {
    const auto number = [](double lo, double hi, const char* description) {
        return ai::Json{{"type", "number"}, {"minimum", lo}, {"maximum", hi}, {"description", description}};
    };
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"on", {{"type", "boolean"}, {"description", "false: not an eye any more. Default true."}}},
              {"iris_radius", number(0.02, 0.5, "The iris's radius in uv, about (0.5, 0.5). Default 0.15.")},
              {"pupil_radius", number(0, 0.95, "The drawn pupil's radius, of the iris's. Default 0.3.")},
              {"iris_depth", number(0, 0.45, "How far the iris lies under the cornea, of its radius: it "
                                             "shifts as the eye turns. Default 0.35.")},
              {"limbal", number(0, 0.5, "The dark ring round the iris: its width, of the iris radius. "
                                        "Default 0.15.")},
              {"limbal_darkness", number(0, 1, "How dark that ring is. Default 0.7.")},
              {"iris_color", {{"type", "array"},
                              {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                              {"minItems", 3},
                              {"maxItems", 3},
                              {"description", "The drawn iris, linear RGB. Default a grey blue."}}},
              {"painted", {{"type", "boolean"},
                           {"description", "The base_color map holds the iris and pupil (an eye texture): "
                                           "none are drawn. Default false."}}},
              {"cornea_roughness", number(0.045, 1, "The wet highlight's roughness. Default 0.06.")},
              {"bulge", number(0, 1, "How far the cornea bulges over the iris. Default 0.3.")},
              {"shade", number(0, 1, "Darkens the white toward its edge, where the lids shade it. "
                                     "Default 0.5.")}}},
            {"description",
             "An eye (characters): a mesh with uvs whose iris is centred at uv (0.5, 0.5). The iris and "
             "pupil show through a clear, wet cornea and shift under it as the eye turns; base_color is "
             "the white. Fields left out keep their value (the defaults when it was not an eye). A "
             "material is skin or an eye: setting one takes the other off. Not on the ground."}};
}

// Nothing here can fail: eye_schema's ranges are the only check, and the
// renderer clamps again (render::to_gpu).
inline void apply_eye(const ai::Json& json, std::optional<world::EyeLook>& eye) {
    if (!json.value("on", true)) {
        eye.reset();
        return;
    }
    world::EyeLook e = eye.value_or(world::EyeLook{});
    e.iris_radius = json.value("iris_radius", e.iris_radius);
    e.pupil_radius = json.value("pupil_radius", e.pupil_radius);
    e.iris_depth = json.value("iris_depth", e.iris_depth);
    e.limbal = json.value("limbal", e.limbal);
    e.limbal_darkness = json.value("limbal_darkness", e.limbal_darkness);
    if (const auto c = json.find("iris_color"); c != json.end()) {
        e.iris_color = {(*c)[0].get<float>(), (*c)[1].get<float>(), (*c)[2].get<float>()};
    }
    e.painted = json.value("painted", e.painted);
    e.cornea_roughness = json.value("cornea_roughness", e.cornea_roughness);
    e.bulge = json.value("bulge", e.bulge);
    e.shade = json.value("shade", e.shade);
    eye = e;
}

inline ai::Json eye_json(const world::EyeLook& e) {
    ai::Json j = {{"iris_radius", e.iris_radius},
                  {"pupil_radius", e.pupil_radius},
                  {"iris_depth", e.iris_depth},
                  {"limbal", e.limbal},
                  {"limbal_darkness", e.limbal_darkness},
                  {"iris_color", ai::Json::array({e.iris_color.x, e.iris_color.y, e.iris_color.z})},
                  {"cornea_roughness", e.cornea_roughness},
                  {"bulge", e.bulge},
                  {"shade", e.shade}};
    if (e.painted) j["painted"] = true;
    return j;
}

// Skin and eye arguments onto a material: a material is one or the
// other, the one given last here (the eye, given both) taking the other
// off. Returns what is wrong, or "".
inline std::string apply_skin_and_eye(const ai::Json& args, world::Material& material) {
    if (args.contains("skin")) {
        if (std::string bad = apply_skin(args["skin"], material.skin); !bad.empty()) return bad;
        if (material.skin) material.eye.reset();
    }
    if (args.contains("eye")) {
        apply_eye(args["eye"], material.eye);
        if (material.eye) material.skin.reset();
    }
    return {};
}

// A mesh's file's own look (its material's values and maps) onto
// `material`, keeping what files do not say (surface, wind), then what
// `args` give over it (base colour, roughness, metallic, emissive, maps:
// already applied to `material`, kept). The default look if the file has
// none (so a look taken from another file does not stay).
inline void take_file_look(const MeshLibrary::Mesh& mesh, world::Material& material, const ai::Json& args) {
    const world::Material given = material;
    const world::Material file = mesh.material.value_or(world::Material{});
    material.base_color = args.contains("base_color") ? given.base_color : file.base_color;
    material.roughness = args.contains("roughness") ? given.roughness : file.roughness;
    material.metallic = args.contains("metallic") ? given.metallic : file.metallic;
    material.emissive = args.contains("emissive") ? given.emissive : file.emissive;
    material.maps = file.maps;
    if (const auto maps = args.find("maps"); maps != args.end()) {
        apply_maps(*maps, material.maps); // (checked when first applied)
    }
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
