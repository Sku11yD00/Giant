#include "mesh_library.h"

#include "giant/core/log.h"
#include "giant/geometry/clusters.h"
#include "giant/geometry/gltf.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

std::filesystem::path from_utf8(std::string_view path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string s = path.generic_u8string();
    return std::string(s.begin(), s.end());
}

// An image a material uses, as a map names it: "<mesh>#image<N>" for one
// inside the file (the texture cooker reads it from there), else the file
// beside it, relative to where the mesh's path is relative to. "" (with a
// warning) if it is missing or too long a path.
std::string image_path(const geometry::ImportedImage& image, const std::string& mesh_path,
                       std::vector<std::string>& warnings) {
    if (image.empty()) return {};
    std::string path;
    if (image.embedded >= 0) {
        path = mesh_path + "#image" + std::to_string(image.embedded);
    } else {
        const std::filesystem::path folder = from_utf8(mesh_path).parent_path();
        path = MeshLibrary::normalize(to_utf8(folder / from_utf8(image.path)));
        std::error_code ec;
        if (!std::filesystem::is_regular_file(from_utf8(path), ec)) {
            warnings.push_back("the image " + path + " it names is missing; that map was left out");
            return {};
        }
    }
    if (path.size() > world::k_max_mesh_path_bytes) {
        warnings.push_back("an image path over " + std::to_string(world::k_max_mesh_path_bytes) +
                           " bytes was left out");
        return {};
    }
    return path;
}

world::Material file_material(const geometry::ImportedMaterial& m, const std::string& mesh_path, bool maps,
                              std::vector<std::string>& warnings) {
    world::Material out;
    out.base_color = m.base_color;
    out.roughness = m.roughness;
    out.metallic = m.metallic;
    out.emissive = m.emissive;
    if (maps) {
        out.maps.base_color = image_path(m.base_color_image, mesh_path, warnings);
        out.maps.normal = image_path(m.normal_image, mesh_path, warnings);
        out.maps.roughness_metal = image_path(m.roughness_metal_image, mesh_path, warnings);
        out.maps.occlusion = image_path(m.occlusion_image, mesh_path, warnings);
    }
    // glTF's metallic factor defaults to 1 and exporters leave it so when
    // the map says where the metal is: without the map, all of it would be.
    if (m.names_roughness_metal && out.maps.roughness_metal.empty() && out.metallic > 0.0f) {
        out.metallic = 0.0f;
        warnings.push_back("\"" + m.name + "\"'s metallic-roughness map was left out, so it is drawn as a non-metal");
    }
    return out;
}

} // namespace

u32 MeshLibrary::Skinned::find_clip(std::string_view name) const {
    for (u32 i = 0; i < clips.size(); ++i) {
        if (clips[i].name == name) return i;
    }
    return anim::k_no_clip;
}

MeshLibrary::~MeshLibrary() {
    if (m_renderer != nullptr) {
        for (const auto& mesh : m_meshes) {
            m_renderer->remove_mesh(mesh->id);
            for (const Part& part : mesh->parts) m_renderer->remove_mesh(part.id);
        }
    }
}

std::string MeshLibrary::normalize(std::string_view path) {
    std::string slashes(path);
    std::replace(slashes.begin(), slashes.end(), '\\', '/');
    return to_utf8(from_utf8(slashes).lexically_normal());
}

const MeshLibrary::Mesh* MeshLibrary::find(std::string_view path) const {
    return find_normalized(normalize(path));
}

const MeshLibrary::Mesh* MeshLibrary::find_normalized(const std::string& path) const {
    const auto it = m_by_path.find(path);
    return it == m_by_path.end() ? nullptr : it->second;
}

MeshLibrary::Loaded MeshLibrary::load(std::string_view path) {
    if (const Mesh* mesh = find(path)) {
        return {mesh, {}};
    }
    return import(normalize(path), nullptr);
}

MeshLibrary::Loaded MeshLibrary::reload(std::string_view path) {
    Mesh* existing = const_cast<Mesh*>(find(path));
    return import(normalize(path), existing);
}

MeshLibrary::Loaded MeshLibrary::import(const std::string& path, Mesh* replace) {
    if (path.empty() || path == ".") {
        return {nullptr, "no file given"};
    }
    const auto start = std::chrono::steady_clock::now();
    geometry::ImportResult imported = geometry::import_gltf_file(from_utf8(path));
    if (!imported.ok()) {
        return {nullptr, path + ": " + imported.error};
    }
    // Its parts: with two or more materials in use, each one's triangles as a mesh of its own.
    struct PartSource {
        u16 material = geometry::k_default_material;
        u64 triangles = 0;
    };
    std::vector<PartSource> sources;
    for (usize m = 0; m < imported.materials.size() && m < geometry::k_default_material; ++m) {
        sources.push_back({static_cast<u16>(m), imported.materials[m].triangles});
    }
    if (imported.default_material_triangles > 0) {
        sources.push_back({geometry::k_default_material, imported.default_material_triangles});
    }
    // Most triangles first (a named material before the default at a tie; else the file's order).
    std::stable_sort(sources.begin(), sources.end(),
                     [](const PartSource& a, const PartSource& b) { return a.triangles > b.triangles; });
    const bool in_parts = sources.size() >= 2 && sources.size() <= k_max_parts &&
                          imported.triangle_materials.size() == imported.mesh.indices.size() / 3;
    std::vector<geometry::ClusterMesh> part_clusters;
    std::vector<std::string> seam_warnings;
    if (in_parts) {
        const std::vector<u8> seam_vertices = geometry::seam_vertices(imported.mesh, imported.triangle_materials);
        for (const PartSource& source : sources) {
            // Where it meets the other parts it keeps every corner at every level, so
            // the parts never crack apart however coarse each is drawn.
            const std::vector<u8> seams =
                geometry::part_seams(imported.mesh, imported.triangle_materials, source.material, seam_vertices);
            part_clusters.push_back(geometry::build_clusters(
                geometry::part_of(imported.mesh, imported.triangle_materials, source.material), seams));
            // Much of a big part on seams (materials woven through each other): it cannot
            // thin with distance, which the file's author should hear of.
            const usize on_seams = static_cast<usize>(std::count_if(seams.begin(), seams.end(), [](u8 s) { return s != 0; }));
            if (source.triangles >= k_seam_warning_triangles && on_seams * 4 >= seams.size()) {
                const std::string name = source.material == geometry::k_default_material
                                             ? std::string("the triangles with no material")
                                             : "\"" + imported.materials[source.material].name + "\"";
                seam_warnings.push_back(name + ": " + std::to_string(on_seams * 100 / seams.size()) +
                                        "% of its corners lie where it meets other materials, and those stay at every "
                                        "distance: it will hardly thin far away (fewer, larger areas of one material fix it)");
            }
            if (part_clusters.back().empty()) {
                return {nullptr, path + ": a material's triangles could not be made into clusters"};
            }
        }
    }
    geometry::ClusterMesh clusters = geometry::build_clusters(imported.mesh);
    imported.mesh = {}; // free the source triangles before uploading
    if (clusters.empty()) {
        return {nullptr, path + ": the file has no usable triangles"};
    }
    std::shared_ptr<Skinned> skinned;
    if (imported.skinned) {
        skinned = std::make_shared<Skinned>();
        if (in_parts) {
            const std::vector<u8> seam_vertices =
                geometry::seam_vertices(imported.skinned->mesh.mesh, imported.triangle_materials);
            for (const PartSource& source : sources) {
                skinned->parts.push_back(geometry::build_skinned_clusters(
                    geometry::part_of(imported.skinned->mesh, imported.triangle_materials, source.material), true,
                    geometry::part_seams(imported.skinned->mesh.mesh, imported.triangle_materials, source.material,
                                         seam_vertices)));
                if (skinned->parts.back().mesh.empty()) {
                    return {nullptr, path + ": a material's skinned triangles could not be made into clusters"};
                }
            }
        }
        // With its levels of detail: far characters draw (and pose) coarse.
        skinned->clusters = geometry::build_skinned_clusters(imported.skinned->mesh, true);
        imported.skinned->mesh = {};
        if (skinned->clusters.mesh.empty() || imported.skinned->skeleton.size() > render::k_max_skin_joints) {
            return {nullptr, path + ": the file's skinned mesh could not be made into clusters"};
        }
        skinned->skeleton = std::move(imported.skinned->skeleton);
        skinned->clips = std::move(imported.skinned->clips);
        skinned->joint_radius = std::move(imported.skinned->joint_radius);
        skinned->capsules = std::move(imported.skinned->capsules);
        skinned->rest_model.resize(skinned->skeleton.size());
        anim::model_matrices(skinned->skeleton, skinned->skeleton.rest, skinned->rest_model);
    }
    auto mesh = std::make_unique<Mesh>();
    mesh->skinned = std::move(skinned);
    mesh->path = path;
    mesh->triangles = clusters.source_triangles;
    mesh->clusters = static_cast<u32>(clusters.clusters.size());
    mesh->levels = clusters.levels;
    for (const geometry::Cluster& c : clusters.clusters) {
        if (clusters.groups[c.group].simplified.error >= geometry::k_terminal_error) {
            mesh->coarsest_triangles += c.triangle_count;
        }
    }
    mesh->warnings = std::move(imported.warnings);
    for (std::string& w : seam_warnings) mesh->warnings.push_back(std::move(w));
    mesh->has_uvs = imported.has_uvs;
    if (!imported.curves.empty()) {
        mesh->curves = std::make_shared<const std::vector<std::vector<Vec3>>>(std::move(imported.curves));
    }
    // The file's look: its busiest material, if that covers more than glTF's
    // default does; its maps only if it covers most of the mesh (else they
    // would land on the other parts' uvs).
    mesh->file_materials = static_cast<u32>(imported.materials.size());
    if (in_parts) {
        // Each part by a name of its own (entities name parts to give them looks): its
        // material's, and where a part before it has that name already (materials alike in
        // name, or with none), with a number no material of the file and no part has.
        std::vector<std::string> names(sources.size());
        for (usize p = 0; p < sources.size(); ++p) {
            if (sources[p].material != geometry::k_default_material) names[p] = imported.materials[sources[p].material].name;
        }
        const std::vector<std::string> given = names;
        for (usize p = 1; p < names.size(); ++p) {
            const auto had = [&](const std::string& name) {
                return std::find(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(p), name) !=
                       names.begin() + static_cast<std::ptrdiff_t>(p);
            };
            if (!had(names[p])) continue;
            for (usize n = 2;; ++n) {
                std::string numbered = given[p] + " (" + std::to_string(n) + ")";
                if (!had(numbered) && std::find(given.begin(), given.end(), numbered) == given.end()) {
                    names[p] = std::move(numbered);
                    break;
                }
            }
        }
        for (usize p = 0; p < sources.size(); ++p) {
            Part& part = mesh->parts.emplace_back();
            part.triangles = part_clusters[p].source_triangles;
            part.name = names[p];
            if (sources[p].material != geometry::k_default_material) {
                // (Told of by the part's name, which is what its look is given by.)
                geometry::ImportedMaterial m = imported.materials[sources[p].material];
                m.name = part.name;
                part.material = file_material(m, path, true, mesh->warnings);
            }
        }
        // Entities given the file take its biggest part's look for their own fields.
        mesh->material = mesh->parts.front().material;
        if (sources.front().material != geometry::k_default_material) {
            const geometry::ImportedMaterial& busiest = imported.materials[sources.front().material];
            u64 total = imported.default_material_triangles;
            for (const geometry::ImportedMaterial& m : imported.materials) total += m.triangles;
            // (Mostly the same warnings as the part's look gave: only the new ones told.)
            std::vector<std::string> told;
            mesh->whole_material = file_material(busiest, path, busiest.triangles * 4 >= total * 3, told);
            for (std::string& w : told) {
                if (std::find(mesh->warnings.begin(), mesh->warnings.end(), w) == mesh->warnings.end()) {
                    mesh->warnings.push_back("drawn whole (scattered): " + std::move(w));
                }
            }
        }
    } else if (!imported.materials.empty()) {
        const geometry::ImportedMaterial& busiest = imported.materials[0];
        u64 total = imported.default_material_triangles;
        for (const geometry::ImportedMaterial& m : imported.materials) total += m.triangles;
        if (busiest.triangles <= imported.default_material_triangles) {
            mesh->warnings.push_back("most of the file has no material; entities given it take the default look unless they give one");
        } else {
            const bool maps = busiest.triangles * 4 >= total * 3;
            mesh->material = file_material(busiest, path, maps, mesh->warnings);
            if (imported.materials.size() > 1 || imported.default_material_triangles > 0) {
                mesh->warnings.push_back(
                    "the file has " + std::to_string(imported.materials.size() + (imported.default_material_triangles > 0 ? 1 : 0)) +
                    " materials; entities take the one with the most triangles (\"" + busiest.name + "\")" +
                    (maps ? "" : " without its maps (it covers under three quarters of the mesh)") +
                    ": it has more than " + std::to_string(k_max_parts) + " materials, too many to draw each as a part");
            }
        }
    }
    if (mesh->parts.empty()) mesh->whole_material = mesh->material;
    if (m_renderer != nullptr) {
        mesh->id = m_renderer->add_mesh(std::move(clusters));
        if (mesh->id == render::k_no_mesh) {
            return {nullptr, path + ": could not upload the mesh to the GPU"};
        }
        for (usize p = 0; p < mesh->parts.size(); ++p) {
            mesh->parts[p].id = m_renderer->add_mesh(std::move(part_clusters[p]));
            if (mesh->parts[p].id == render::k_no_mesh) {
                m_renderer->remove_mesh(mesh->id);
                for (usize made = 0; made < p; ++made) m_renderer->remove_mesh(mesh->parts[made].id);
                return {nullptr, path + ": could not upload the mesh's parts to the GPU"};
            }
        }
    }
    mesh->import_seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.2f", static_cast<double>(mesh->import_seconds));
    log_info("editor", "imported " + path + ": " + std::to_string(mesh->triangles) + " triangles, " +
                           std::to_string(mesh->clusters) + " clusters in " +
                           std::to_string(mesh->levels) + " levels (" + seconds + " s)" +
                           (mesh->parts.empty() ? std::string() : ", " + std::to_string(mesh->parts.size()) + " parts") +
                           (mesh->skinned ? ", skinned: " + std::to_string(mesh->skinned->skeleton.size()) +
                                                " joints, " + std::to_string(mesh->skinned->clips.size()) + " clips"
                                          : std::string()));
    for (const std::string& w : mesh->warnings) {
        log_warning("editor", path + ": " + w);
    }
    if (replace != nullptr) {
        if (m_renderer != nullptr) {
            m_renderer->remove_mesh(replace->id);
            for (const Part& part : replace->parts) m_renderer->remove_mesh(part.id);
        }
        *replace = std::move(*mesh);
        ++m_version;
        return {replace, {}};
    }
    m_meshes.push_back(std::move(mesh));
    m_by_path[m_meshes.back()->path] = m_meshes.back().get();
    ++m_version;
    return {m_meshes.back().get(), {}};
}

} // namespace giant::editor
