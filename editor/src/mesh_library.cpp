#include "mesh_library.h"

#include "giant/core/log.h"
#include "giant/geometry/clusters.h"
#include "giant/geometry/gltf.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace giant::editor {

namespace {

std::filesystem::path from_utf8(std::string_view path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string s = path.generic_u8string();
    return std::string(s.begin(), s.end());
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
    geometry::ClusterMesh clusters = geometry::build_clusters(imported.mesh);
    imported.mesh = {}; // free the source triangles before uploading
    if (clusters.empty()) {
        return {nullptr, path + ": the file has no usable triangles"};
    }
    std::shared_ptr<Skinned> skinned;
    if (imported.skinned) {
        skinned = std::make_shared<Skinned>();
        skinned->clusters = geometry::build_skinned_clusters(imported.skinned->mesh);
        imported.skinned->mesh = {};
        if (skinned->clusters.mesh.empty() || imported.skinned->skeleton.size() > render::k_max_skin_joints) {
            return {nullptr, path + ": the file's skinned mesh could not be made into clusters"};
        }
        skinned->skeleton = std::move(imported.skinned->skeleton);
        skinned->clips = std::move(imported.skinned->clips);
        skinned->joint_radius = std::move(imported.skinned->joint_radius);
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
    if (m_renderer != nullptr) {
        mesh->id = m_renderer->add_mesh(std::move(clusters));
        if (mesh->id == render::k_no_mesh) {
            return {nullptr, path + ": could not upload the mesh to the GPU"};
        }
    }
    mesh->import_seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.2f", static_cast<double>(mesh->import_seconds));
    log_info("editor", "imported " + path + ": " + std::to_string(mesh->triangles) + " triangles, " +
                           std::to_string(mesh->clusters) + " clusters in " +
                           std::to_string(mesh->levels) + " levels (" + seconds + " s)" +
                           (mesh->skinned ? ", skinned: " + std::to_string(mesh->skinned->skeleton.size()) +
                                                " joints, " + std::to_string(mesh->skinned->clips.size()) + " clips"
                                          : std::string()));
    for (const std::string& w : mesh->warnings) {
        log_warning("editor", path + ": " + w);
    }
    if (replace != nullptr) {
        if (m_renderer != nullptr) {
            m_renderer->remove_mesh(replace->id);
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
