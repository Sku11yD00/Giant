// The far ground: coarse chunks of a streamed world's terrain, written by
// the save, read near the camera, drawn where the full tiles are not.
#include "../src/far_ground.h"
#include "../src/mesh_library.h"
#include "../src/scene_render.h"
#include "../src/terrain_meshes.h"
#include "../src/world_streaming.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

using namespace giant;
using namespace giant::editor;
using world::CellCoord;
using world::Terrain;

namespace {

std::filesystem::path fresh_folder() {
    static std::atomic<int> n{0};
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("giant_far_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
         std::to_string(n++));
    std::filesystem::remove_all(p);
    return p;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// A tile whose height at sample k is base + k (so every sample is told apart).
Terrain::Heights tile(u32 samples, float base) {
    Terrain::Heights h(usize{samples} * samples);
    for (usize k = 0; k < h.size(); ++k) h[k] = base + static_cast<float>(k) * 0.01f;
    return h;
}

struct Far {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
    world::Scene scene;
    SceneFrame frame;
    WorldStreaming streaming;
    std::unique_ptr<TerrainMeshes> ground;
    std::unique_ptr<FarGround> far;
    std::filesystem::path folder = fresh_folder();
    u64 generation = 1;

    Far() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        renderer = render::Renderer::create(*device);
        meshes = std::make_unique<MeshLibrary>(renderer.get());
        ground = std::make_unique<TerrainMeshes>(renderer.get());
        far = std::make_unique<FarGround>(renderer.get());
        streaming.settings.load_radius = 200.0;
        streaming.settings.unload_margin = 64.0;
        streaming.settings.unload_every = 1;
        far->settings.radius = {3000.0, 20000.0};
        far->settings.margin = {100.0, 100.0};
    }
    ~Far() {
        far.reset();
        ground.reset();
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    void reopen() {
        ++generation;
        GIANT_CHECK(!streaming.open(folder, scene, generation).has_value());
    }
    void frame_at(double x, double z) {
        ground->update(scene.terrain);
        far->update({x, 10.0, z}, streaming, generation, *ground);
        scene_frame(scene, *renderer, *meshes, frame, ground.get(), far.get());
        streaming.update(scene, generation, {x, 10.0, z}, frame);
    }
    // Until cells, chunks and meshes are all in.
    void settle(double x, double z) {
        for (int i = 0; i < 40; ++i) {
            streaming.finish_reads();
            ground->finish(scene.terrain);
            far->finish({x, 10.0, z}, streaming, generation, *ground);
            frame_at(x, z);
        }
    }
    std::filesystem::path chunk_path(u32 level, CellCoord c) const {
        return folder / "ground" / std::to_string(level) / world::far_chunk_file_name(c);
    }
    std::optional<Terrain::Heights> chunk(u32 level, CellCoord c) const {
        Terrain::Heights h;
        if (!std::filesystem::exists(chunk_path(level, c))) return std::nullopt;
        if (world::load_far_chunk(read_file(chunk_path(level, c)), level, c, h)) return std::nullopt;
        return h;
    }
    // Instances in the frame at a position, of any mesh.
    usize instances_at(double x, double z) const {
        usize n = 0;
        for (const render::Instance& i : frame.instances) {
            n += i.mesh != render::k_no_mesh && i.position.x == x && i.position.z == z ? 1u : 0u;
        }
        return n;
    }
};

constexpr usize k_n = world::k_far_samples;

} // namespace

GIANT_TEST("far ground: a tile's surface leaves out triangles with no ground, and hangs walls from its edges") {
    Terrain t(512.0, true);
    t.set_samples(8);
    Terrain::Heights h(64, 2.0f);
    h[9] = std::numeric_limits<float>::quiet_NaN(); // sample (1, 1)
    t.set({0, 0}, h);
    TileHeights heights = tile_heights(t, {0, 0});
    const TileSurface plain = tile_surface(heights);
    // 8 x 8 squares, two triangles each, but the 6 triangles touching (1, 1).
    GIANT_CHECK(plain.mesh.indices.size() == (8u * 8u * 2u - 6u) * 3u);
    for (const u32 i : plain.mesh.indices) GIANT_CHECK(!(i == 9 + 1)); // vertex (1, 1) is unused (row of 9)
    for (const geometry::Vertex& v : plain.mesh.vertices) GIANT_CHECK(std::isfinite(v.position.y));
    heights.skirt = 10.0f;
    const TileSurface walled = tile_surface(heights);
    // 4 edges of 9 vertices: 8 quads each, both sides (4 triangles).
    GIANT_CHECK(walled.mesh.indices.size() == plain.mesh.indices.size() + 4u * 8u * 4u * 3u);
    GIANT_CHECK(walled.mesh.vertices.size() == plain.mesh.vertices.size() + 4u * 9u);
    GIANT_CHECK(walled.mesh.vertices.back().position.y == 2.0f - 10.0f && walled.edge.back() == 1);
}

GIANT_TEST("far ground: the save writes each level's chunks over the tiles it wrote") {
    Far f;
    if (f.renderer == nullptr) return;
    f.scene.terrain.set_samples(16);
    f.scene.terrain.set({0, 0}, tile(16, 1.0f));
    f.scene.terrain.set({1, 0}, tile(16, 2.0f));
    f.scene.terrain.set({-8, 8}, tile(16, 3.0f)); // chunk (-1, 1), at a level-2 sample point
    const auto saved = f.streaming.create(f.folder, f.scene, f.generation, {});
    GIANT_CHECK(saved.error.empty() && saved.chunks_written == 4); // 2 of level 1, 2 of level 2
    GIANT_CHECK(f.streaming.far_chunks(1).size() == 2 && f.streaming.far_chunks(2).size() == 2);
    const auto c = f.chunk(1, {0, 0});
    GIANT_CHECK(c.has_value());
    if (!c) return;
    // Cell (1, 0)'s block: chunk samples 4..7 along x, every 4th of the tile's.
    GIANT_CHECK((*c)[4] == f.scene.terrain.find({1, 0})->at(0) && (*c)[1 * k_n + 5] == f.scene.terrain.find({1, 0})->at(4 * 16 + 4));
    GIANT_CHECK((*c)[0] == 1.0f && std::isnan((*c)[8]) && std::isnan((*c)[4 * k_n]));
    const auto l2 = f.chunk(2, {0, 0});
    GIANT_CHECK(l2.has_value() && (*l2)[0] == 1.0f && std::isnan((*l2)[1])); // level 1's sample 8 is (2, 0): none
    GIANT_CHECK(f.chunk(2, {-1, 0}).has_value() && f.chunk(1, {-1, 1}).has_value());

    // An edit, saved: the chunk follows; a chunk left with no ground loses its file.
    (*f.scene.terrain.modify({0, 0}))[0] = 50.0f;
    f.scene.terrain.remove({-8, 8});
    const auto again = f.streaming.save(f.scene, {});
    GIANT_CHECK(again.error.empty() && again.chunks_written == 4);
    GIANT_CHECK(f.chunk(1, {0, 0}).value()[0] == 50.0f && f.chunk(2, {0, 0}).value()[0] == 50.0f);
    GIANT_CHECK(!std::filesystem::exists(f.chunk_path(1, {-1, 1})) && !std::filesystem::exists(f.chunk_path(2, {-1, 0})));
    GIANT_CHECK(f.streaming.far_chunks(1).size() == 1 && f.streaming.far_chunks(2).size() == 1);
    // Nothing of the ground changed: no chunk written.
    GIANT_CHECK(f.streaming.save(f.scene, {}).chunks_written == 0);
}

GIANT_TEST("far ground: a chunk with no file yet takes its tiles on disk from their cells' files") {
    Far f;
    if (f.renderer == nullptr) return;
    f.scene.terrain.set_samples(8);
    f.scene.terrain.set({0, 0}, tile(8, 1.0f));
    f.scene.terrain.set({1, 0}, tile(8, 2.0f));
    GIANT_CHECK(f.streaming.create(f.folder, f.scene, f.generation, {}).error.empty());
    f.reopen();
    std::filesystem::remove_all(f.folder / "ground"); // a world saved before chunks
    f.reopen();
    f.scene.terrain.set({2, 0}, tile(8, 3.0f)); // cells (0, 0) and (1, 0) stay on disk
    const auto saved = f.streaming.save(f.scene, {});
    GIANT_CHECK(saved.error.empty());
    const auto c = f.chunk(1, {0, 0});
    GIANT_CHECK(c.has_value() && (*c)[0] == 1.0f && (*c)[4] == 2.0f && (*c)[8] == 3.0f);
}

GIANT_TEST("far ground: ground cells load with their whole chunk; an edited chunk keeps its tiles") {
    Far f;
    if (f.renderer == nullptr) return;
    f.scene.terrain.set_samples(8);
    f.scene.terrain.set({0, 0}, tile(8, 1.0f));
    f.scene.terrain.set({7, 0}, tile(8, 2.0f)); // 448 m away: past the load radius, in the same chunk
    world::Entity e;
    e.transform.position = {6 * 64.0 + 5.0, 0.0, 5.0}; // cell (6, 0): no ground, goes by its own distance
    f.scene.add(e);
    GIANT_CHECK(f.streaming.create(f.folder, f.scene, f.generation, {}).error.empty());
    f.reopen();
    f.settle(10.0, 10.0);
    GIANT_CHECK(f.scene.terrain.size() == 2 && f.scene.size() == 0);
    // An edit at (0, 0), then away: (7, 0) stays with it, clean as it is.
    (*f.scene.terrain.modify({0, 0}))[0] = 9.0f;
    f.settle(5000.0, 10.0);
    GIANT_CHECK(f.scene.terrain.size() == 2);
    GIANT_CHECK(f.streaming.save(f.scene, {}).error.empty());
    f.settle(5000.0, 10.0);
    GIANT_CHECK(f.scene.terrain.size() == 0);
}

GIANT_TEST("far ground: full tiles near the camera, chunk meshes past them, the coarsest where a part is missing") {
    Far f;
    if (f.renderer == nullptr) return;
    f.scene.terrain.set_samples(8);
    // 4 x 4 cells in chunk (0, 0), and in chunk (3, 0) 1536 m away (its cells
    // not loaded): enough for level 2's samples (every other cell) to make squares.
    for (i32 z = 0; z < 4; ++z) {
        for (i32 x = 0; x < 4; ++x) {
            f.scene.terrain.set({x, z}, tile(8, 1.0f));
            f.scene.terrain.set({24 + x, z}, tile(8, 2.0f));
        }
    }
    GIANT_CHECK(f.streaming.create(f.folder, f.scene, f.generation, {}).error.empty());
    f.reopen();
    f.settle(10.0, 10.0);
    GIANT_CHECK(f.scene.terrain.size() == 16);
    const FarGroundStats s = f.far->stats();
    GIANT_CHECK(s.files[0] == 2 && s.loaded[0] == 2 && s.files[1] == 1 && s.loaded[1] == 1);
    GIANT_CHECK(s.drawn[0] == 1 && s.drawn[1] == 0 && s.hidden == 0);
    GIANT_CHECK(f.far->draws_tile({0, 0}) && f.far->draws_tile({1, 0}));
    // The tiles draw from their cells; chunk (3, 0) from its corner cell.
    GIANT_CHECK(f.instances_at(0.0, 0.0) == 1 && f.instances_at(64.0, 0.0) == 1);
    GIANT_CHECK(f.instances_at(1536.0, 0.0) == 1);

    // A level-1 chunk let go (out of its radius) cannot stand in: the level-2 chunk draws over all of it.
    f.far->settings.radius[0] = 600.0;
    f.settle(10.0, 10.0);
    const FarGroundStats t = f.far->stats();
    GIANT_CHECK(t.loaded[0] == 1 && t.drawn[1] == 1 && t.drawn[0] == 0 && t.hidden == 64);
    GIANT_CHECK(!f.far->draws_tile({0, 0}));
    GIANT_CHECK(f.instances_at(64.0, 0.0) == 0 && f.instances_at(1536.0, 0.0) == 0);
    GIANT_CHECK(f.instances_at(0.0, 0.0) == 1); // the level-2 chunk, from the same corner cell

    // Back in reach: the tiles again.
    f.far->settings.radius[0] = 3000.0;
    f.settle(10.0, 10.0);
    GIANT_CHECK(f.far->draws_tile({0, 0}) && f.far->stats().drawn[1] == 0);
    GIANT_CHECK(f.instances_at(64.0, 0.0) == 1 && f.instances_at(1536.0, 0.0) == 1);

    // Closed: no far ground, every tile draws.
    f.streaming.close();
    f.settle(10.0, 10.0);
    GIANT_CHECK(f.far->stats().loaded[0] == 0 && f.far->draws_tile({0, 0}) && f.instances_at(1536.0, 0.0) == 0);
}

GIANT_TEST("far ground: a save hands the far ground the chunks it wrote") {
    Far f;
    if (f.renderer == nullptr) return;
    f.scene.terrain.set_samples(8);
    f.scene.terrain.set({24, 0}, tile(8, 2.0f));
    f.scene.terrain.set({0, 0}, tile(8, 1.0f));
    GIANT_CHECK(f.streaming.create(f.folder, f.scene, f.generation, {}).error.empty());
    f.reopen();
    f.settle(10.0, 10.0);
    GIANT_CHECK(f.far->chunks(1).find({3, 0}) != nullptr);
    // A tile on the far side, made and saved: its chunk is the save's at once.
    f.scene.terrain.set({25, 0}, tile(8, 4.0f));
    const auto saved = f.streaming.save(f.scene, {});
    f.far->saved(saved);
    const Terrain::Heights* c = f.far->chunks(1).find({3, 0});
    GIANT_CHECK(c != nullptr && (*c)[4] == 4.0f);
    f.scene.terrain.remove({25, 0});
    f.far->saved(f.streaming.save(f.scene, {}));
    c = f.far->chunks(1).find({3, 0});
    GIANT_CHECK(c != nullptr && std::isnan((*c)[4]) && (*c)[0] == 2.0f);
    // Its ground all gone: the chunk too.
    f.scene.terrain.remove({0, 0});
    f.far->saved(f.streaming.save(f.scene, {}));
    GIANT_CHECK(f.far->chunks(1).find({0, 0}) == nullptr && f.far->chunks(1).find({3, 0}) != nullptr);
    f.settle(10.0, 10.0);
    GIANT_CHECK(f.far->stats().loaded[0] == 1 && f.instances_at(1536.0, 0.0) == 1);
}
