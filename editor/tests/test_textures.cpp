// Material textures: image files cooked in the background, kept in a disk
// cache, uploaded under a budget, and drawn by the materials naming them.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/scatter_placer.h"
#include "../src/scene_render.h"
#include "../src/texture_library.h"

#include "giant_test.h"

#include "giant/core/png.h"
#include "giant/geometry/mesh.h"
#include "giant/rhi/rhi.h"

#include "gltf_test_files.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

std::filesystem::path fresh_dir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

// A width x height PNG of one colour.
std::string write_png(const std::filesystem::path& path, u32 width, u32 height, u8 r, u8 g, u8 b) {
    std::vector<u8> rgba(usize{width} * height * 4);
    for (usize i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = 255;
    }
    const std::string png = encode_png_rgba(width, height, rgba);
    std::ofstream(path, std::ios::binary).write(png.data(), static_cast<std::streamsize>(png.size()));
    return path.generic_string();
}

// Writes a mesh as a GLB (positions, normals, uvs if `uvs`, 32-bit indices).
void write_glb(const std::filesystem::path& path, const geometry::TriangleMesh& mesh, bool uvs) {
    std::vector<u8> bin(mesh.vertices.size() * sizeof(geometry::Vertex) + mesh.indices.size() * 4);
    std::memcpy(bin.data(), mesh.vertices.data(), mesh.vertices.size() * sizeof(geometry::Vertex));
    std::memcpy(bin.data() + mesh.vertices.size() * sizeof(geometry::Vertex), mesh.indices.data(),
                mesh.indices.size() * 4);
    const usize vbytes = mesh.vertices.size() * sizeof(geometry::Vertex);
    std::string json = R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                       R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1)" +
                       std::string(uvs ? R"(,"TEXCOORD_0":3)" : "") + R"(},"indices":2}]}],)"
                       R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + "}]," +
                       R"("bufferViews":[{"buffer":0,"byteLength":)" + std::to_string(vbytes) +
                       R"(,"byteStride":)" + std::to_string(sizeof(geometry::Vertex)) +
                       R"(},{"buffer":0,"byteOffset":)" + std::to_string(vbytes) +
                       R"(,"byteLength":)" + std::to_string(mesh.indices.size() * 4) + "}]," +
                       R"("accessors":[{"bufferView":0,"componentType":5126,"count":)" +
                       std::to_string(mesh.vertices.size()) +
                       R"(,"type":"VEC3","min":[-1,-1,-1],"max":[1,1,1]},{"bufferView":0,"byteOffset":12,"componentType":5126,"count":)" +
                       std::to_string(mesh.vertices.size()) +
                       R"(,"type":"VEC3"},{"bufferView":1,"componentType":5125,"count":)" +
                       std::to_string(mesh.indices.size()) + R"(,"type":"SCALAR"})" +
                       (uvs ? R"(,{"bufferView":0,"byteOffset":24,"componentType":5126,"count":)" +
                                  std::to_string(mesh.vertices.size()) + R"(,"type":"VEC2"})"
                            : std::string()) +
                       "]}";
    while (json.size() % 4 != 0) json += ' ';
    while (bin.size() % 4 != 0) bin.push_back(0);
    std::vector<u8> out;
    const auto u32le = [&out](u32 v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
    };
    u32le(0x46546C67);
    u32le(2);
    u32le(static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
    u32le(static_cast<u32>(json.size()));
    u32le(0x4E4F534A);
    out.insert(out.end(), json.begin(), json.end());
    u32le(static_cast<u32>(bin.size()));
    u32le(0x004E4942);
    out.insert(out.end(), bin.begin(), bin.end());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

usize files_in(const std::filesystem::path& dir) {
    usize n = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) n += entry.is_regular_file() ? 1u : 0u;
    return n;
}

} // namespace

GIANT_TEST("textures: cooked in the background, cached on disk, read back from the cache") {
    const std::filesystem::path dir = fresh_dir("giant_texture_library");
    const std::filesystem::path cache = dir / "cache";
    const std::string albedo = write_png(dir / "albedo.png", 64, 32, 200, 100, 50);
    const TextureLibrary::Key key{albedo, TextureLibrary::Slot::base_color, false, {}};
    {
        TextureLibrary textures(nullptr, cache);
        GIANT_CHECK(textures.request({"", TextureLibrary::Slot::normal, false, {}}) == TextureLibrary::k_no_ref);
        const TextureLibrary::Ref ref = textures.request(key);
        GIANT_CHECK(ref != TextureLibrary::k_no_ref);
        GIANT_CHECK(textures.request(key) == ref); // one texture per key
        GIANT_CHECK(textures.pending() == 1);
        const u64 version = textures.version();
        textures.finish();
        GIANT_CHECK(textures.pending() == 0 && textures.version() != version);
        const TextureLibrary::Entry* e = textures.entry(ref);
        GIANT_CHECK(e != nullptr && e->status == TextureLibrary::Status::ready);
        GIANT_CHECK(e->width == 64 && e->height == 32 && e->mips == 7 && e->bytes > 0 && !e->from_cache);
        GIANT_CHECK(textures.id(ref) == render::k_no_texture); // no renderer
        GIANT_CHECK(files_in(cache) == 1);
        GIANT_CHECK(std::filesystem::exists(textures.cache_file(key)));
        // The same file as a normal map is another texture.
        const TextureLibrary::Ref normal = textures.request({albedo, TextureLibrary::Slot::normal, false, {}});
        GIANT_CHECK(normal != ref);
        textures.finish();
        GIANT_CHECK(textures.entry(normal)->status == TextureLibrary::Status::ready);
        GIANT_CHECK(files_in(cache) == 2);
    }
    {
        // A later session reads it back.
        TextureLibrary textures(nullptr, cache);
        const TextureLibrary::Ref ref = textures.request(key);
        textures.finish();
        const TextureLibrary::Entry* e = textures.entry(ref);
        GIANT_CHECK(e != nullptr && e->status == TextureLibrary::Status::ready && e->from_cache);
        GIANT_CHECK(e->width == 64 && e->height == 32);
        // An edited file cooks again (its size changed, so its cache name).
        write_png(dir / "albedo.png", 128, 32, 10, 20, 30);
        textures.reload();
        GIANT_CHECK(textures.pending() == 1);
        textures.finish();
        GIANT_CHECK(e->status == TextureLibrary::Status::ready && !e->from_cache && e->width == 128);
        GIANT_CHECK(files_in(cache) == 2); // the edited file's older version went
        GIANT_CHECK(std::filesystem::exists(textures.cache_file(key)));
    }
}

GIANT_TEST("textures: a missing or bad file fails with why; a texture that was good keeps drawing") {
    const std::filesystem::path dir = fresh_dir("giant_texture_failures");
    TextureLibrary textures(nullptr, {}); // no cache: always cooked
    const TextureLibrary::Ref missing =
        textures.request({(dir / "nope.png").generic_string(), TextureLibrary::Slot::base_color, false, {}});
    std::ofstream(dir / "junk.png", std::ios::binary) << "not a png";
    const TextureLibrary::Ref junk =
        textures.request({(dir / "junk.png").generic_string(), TextureLibrary::Slot::occlusion, false, {}});
    // Roughness with a normal map that cannot be read: cooked without it.
    const std::string orm = write_png(dir / "orm.png", 16, 16, 255, 128, 0);
    const TextureLibrary::Ref rough = textures.request(
        {orm, TextureLibrary::Slot::roughness_metal, false, (dir / "gone.png").generic_string()});
    textures.finish();
    GIANT_CHECK(textures.entry(missing)->status == TextureLibrary::Status::failed);
    GIANT_CHECK(textures.entry(missing)->error.find("nope.png") != std::string::npos);
    GIANT_CHECK(textures.entry(junk)->status == TextureLibrary::Status::failed && !textures.entry(junk)->error.empty());
    GIANT_CHECK(textures.entry(rough)->status == TextureLibrary::Status::ready);
    GIANT_CHECK(textures.entry(rough)->warning.find("gone.png") != std::string::npos);
    // A good file broken and reloaded: still ready (its last good version), with the error.
    std::ofstream(dir / "orm.png", std::ios::binary | std::ios::trunc) << "broken";
    textures.reload();
    textures.finish();
    GIANT_CHECK(textures.entry(rough)->status == TextureLibrary::Status::ready);
    GIANT_CHECK(!textures.entry(rough)->error.empty());
    // A fixed file is tried again on reload.
    write_png(dir / "nope.png", 8, 8, 1, 2, 3);
    textures.reload();
    textures.finish();
    GIANT_CHECK(textures.entry(missing)->status == TextureLibrary::Status::ready);
    GIANT_CHECK(textures.entry(missing)->error.empty());
}

GIANT_TEST("textures: uploads keep to the budget, at least one a frame") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    const std::filesystem::path dir = fresh_dir("giant_texture_budget");
    TextureLibrary textures(renderer.get(), {});
    std::vector<TextureLibrary::Ref> refs;
    for (int i = 0; i < 5; ++i) {
        const std::string path =
            write_png(dir / ("t" + std::to_string(i) + ".png"), 64, 64, static_cast<u8>(i * 50), 0, 0);
        refs.push_back(textures.request({path, TextureLibrary::Slot::base_color, false, {}}));
    }
    // Two cook at once (one a worker), the rest wait.
    GIANT_CHECK(textures.pending() == 5);
    textures.wait_cooked();
    GIANT_CHECK(textures.pending() == 5); // all cooked, none uploaded
    const auto uploaded = [&] {
        usize n = 0;
        for (const auto ref : refs) n += textures.id(ref) != render::k_no_texture ? 1u : 0u;
        return n;
    };
    GIANT_CHECK(uploaded() == 0);
    textures.upload_bytes_per_update = 1; // less than one: still one a call
    textures.update();
    GIANT_CHECK(uploaded() == 1);
    u64 one = 0; // all are the same size: the one uploaded (whichever finished first)
    for (const auto ref : refs) one = std::max(one, textures.entry(ref)->bytes);
    GIANT_CHECK(one > 0);
    textures.upload_bytes_per_update = 2 * one; // exactly two
    textures.update();
    GIANT_CHECK(uploaded() == 3);
    textures.update();
    GIANT_CHECK(uploaded() == 5 && textures.pending() == 0);
    for (const auto ref : refs) {
        const rhi::TextureDesc* d = renderer->texture(textures.id(ref));
        GIANT_CHECK(d != nullptr && d->width == 64 && d->format == rhi::Format::bc7_srgb);
    }
    // A reload replaces each texture: a new id, the old one gone.
    const render::TextureId before = textures.id(refs[0]);
    textures.reload();
    textures.finish();
    GIANT_CHECK(textures.id(refs[0]) != render::k_no_texture && textures.id(refs[0]) != before);
    GIANT_CHECK(renderer->texture(before) == nullptr);
    // A reload superseded by another while cooking: only the newer answer uploads.
    textures.reload();
    textures.reload();
    textures.finish();
    GIANT_CHECK(textures.pending() == 0 && uploaded() == 5);
}

GIANT_TEST("textures: unused ones leave the GPU after a while and come back when used") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    const std::filesystem::path dir = fresh_dir("giant_texture_unload");
    TextureLibrary textures(renderer.get(), dir / "cache");
    textures.unload_after_updates = 3;
    const std::string path = write_png(dir / "a.png", 16, 16, 9, 9, 9);
    const TextureLibrary::Ref a = textures.request({path, TextureLibrary::Slot::base_color, false, {}});
    textures.finish();
    const render::TextureId first = textures.id(a);
    GIANT_CHECK(first != render::k_no_texture);
    textures.set_used(std::vector<TextureLibrary::Ref>{a});
    for (int i = 0; i < 5; ++i) textures.update();
    GIANT_CHECK(textures.id(a) == first); // used: kept
    textures.set_used({});
    textures.update();
    textures.update();
    GIANT_CHECK(textures.id(a) == first); // not yet
    textures.update();
    GIANT_CHECK(textures.id(a) == render::k_no_texture && textures.entry(a)->status == TextureLibrary::Status::unloaded);
    GIANT_CHECK(renderer->texture(first) == nullptr);
    // Used again: loads from the cache.
    textures.set_used(std::vector<TextureLibrary::Ref>{a});
    GIANT_CHECK(textures.entry(a)->status == TextureLibrary::Status::cooking);
    textures.finish();
    GIANT_CHECK(textures.id(a) != render::k_no_texture && textures.entry(a)->from_cache);
    // Another spelling of the path finds the same texture.
    const std::string spelled = (dir / "." / "a.png").string();
    GIANT_CHECK(textures.request({spelled, TextureLibrary::Slot::base_color, false, {}}) == a);
}

GIANT_TEST("textures: the small levels go up first, sharper ones stream in where wanted and leave again") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    rhi::TextureDesc t;
    t.width = 16;
    t.height = 16;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle image = device->create_texture(t);
    const auto draw = [&] { // (queued level changes reach the GPU in a frame)
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd == nullptr) return;
        render::FrameInput frame;
        renderer->render(*cmd, image, frame);
        device->end_frame();
    };
    const std::filesystem::path dir = fresh_dir("giant_texture_stream");
    const std::string path = write_png(dir / "big.png", 512, 512, 90, 120, 30);
    constexpr u32 not_seen = render::TextureResidency::k_not_seen;

    for (const bool cached : {true, false}) { // levels read from the .gtex, or kept in memory
        TextureLibrary textures(renderer.get(), cached ? dir / "cache" : std::filesystem::path{});
        textures.tail_size = 64; // 512 -> 256 -> 128 -> 64: level 3
        textures.trim_after_updates = 3;
        u32 wanted = not_seen;
        textures.wanted_override = [&wanted](TextureLibrary::Ref) { return wanted; };
        const TextureLibrary::Ref a = textures.request({path, TextureLibrary::Slot::base_color, false, {}});
        textures.set_used(std::vector<TextureLibrary::Ref>{a});
        const auto e = [&] { return textures.entry(a); };
        const auto until = [&](auto done) {
            const auto start = std::chrono::steady_clock::now();
            while (!done() && std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
                textures.update();
                draw();
                if (!done()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return done();
        };
        const auto at = [&](u32 mip) {
            const auto r = renderer->texture_residency(textures.id(a));
            return r && r->first_mip == mip && r->target_mip == mip && e()->first_mip == mip && !e()->reading;
        };
        textures.wait_cooked();
        textures.update();
        draw();
        // Only its tail went up.
        GIANT_CHECK(e()->status == TextureLibrary::Status::ready && e()->tail_mip == 3 && e()->mips == 10);
        const render::TextureId id = textures.id(a);
        GIANT_CHECK(id != render::k_no_texture);
        GIANT_CHECK(until([&] { return at(3); }));
        const rhi::TextureDesc* whole = renderer->texture(id);
        GIANT_CHECK(whole != nullptr && whole->width == 512 && whole->mips == 10);
        if (whole == nullptr) return;
        const rhi::TextureDesc full = *whole;
        const auto bytes_from = [&](u32 mip) { return rhi::texture_bytes(render::texture_from_mip(full, mip)); };
        GIANT_CHECK(e()->full_bytes == rhi::texture_bytes(full));
        GIANT_CHECK(e()->bytes == bytes_from(3) && e()->bytes < e()->full_bytes);
        // Not drawn: it stays at its tail.
        for (int i = 0; i < 5; ++i) {
            textures.update();
            draw();
        }
        GIANT_CHECK(at(3) && e()->wanted_mip == not_seen);
        // Drawn close up: finer levels stream in, the same texture id throughout.
        wanted = 0;
        GIANT_CHECK(until([&] { return at(0); }));
        GIANT_CHECK(textures.id(a) == id && e()->bytes == e()->full_bytes && e()->wanted_mip == 0);
        // Wanted less: kept a while, then trimmed to what is wanted.
        wanted = 2;
        textures.update();
        draw();
        GIANT_CHECK(at(0)); // not yet
        GIANT_CHECK(until([&] { return at(2); }));
        GIANT_CHECK(e()->bytes == bytes_from(2));
        // Not drawn at all: back to its tail, never coarser.
        wanted = 9;
        GIANT_CHECK(until([&] { return at(3); }));
        // A budget: finer levels only as far as they fit.
        textures.gpu_budget_bytes = bytes_from(1);
        wanted = 0;
        GIANT_CHECK(until([&] { return at(1); }));
        for (int i = 0; i < 10; ++i) {
            textures.update();
            draw();
        }
        GIANT_CHECK(at(1) && textures.id(a) == id);
        // The budget lowered under what it holds: trimmed at once, wanted or not.
        textures.trim_after_updates = 1000000;
        textures.gpu_budget_bytes = bytes_from(2);
        GIANT_CHECK(until([&] { return at(2); }));
        textures.gpu_budget_bytes = u64{1} << 30;
        textures.trim_after_updates = 3;
        // finish() puts it up whole at once, wanted or not (a screenshot
        // that must be sharp), and it stays so a while.
        wanted = not_seen;
        GIANT_CHECK(until([&] { return at(3); }));
        textures.finish();
        GIANT_CHECK(at(0) && textures.pending() == 0 && e()->bytes == e()->full_bytes);
        const render::TextureId sharp = textures.id(a);
        GIANT_CHECK(sharp != id && renderer->texture(id) == nullptr);
        textures.update();
        draw();
        GIANT_CHECK(at(0));
        // A reload's new version is as sharp as the one it replaces.
        wanted = 0;
        textures.reload();
        textures.wait_cooked();
        textures.update();
        draw();
        GIANT_CHECK(textures.id(a) != sharp && at(0));
        const render::TextureId reloaded = textures.id(a);
        if (cached) {
            // Its cooked file gone: the finer read fails, it is cooked again
            // and draws what it has meanwhile.
            wanted = 9;
            GIANT_CHECK(until([&] { return at(3); }));
            std::error_code ec;
            std::filesystem::remove_all(dir / "cache", ec);
            wanted = 0;
            GIANT_CHECK(until([&] { return textures.id(a) != reloaded; }));
            GIANT_CHECK(e()->status == TextureLibrary::Status::ready && !e()->from_cache);
            GIANT_CHECK(renderer->texture(reloaded) == nullptr);
            GIANT_CHECK(until([&] { return at(0); }));
        }
    }
}

GIANT_TEST("textures: closing with cooks queued returns without cooking them") {
    const std::filesystem::path dir = fresh_dir("giant_texture_close");
    const std::string path = write_png(dir / "big.png", 512, 512, 1, 2, 3);
    {
        TextureLibrary textures(nullptr, {}, 1);
        for (int i = 0; i < 64; ++i) {
            textures.request({path, i % 2 == 0 ? TextureLibrary::Slot::base_color : TextureLibrary::Slot::normal,
                              (i / 2) % 2 == 1, std::to_string(i)}); // distinct keys
        }
        GIANT_CHECK(textures.pending() == 64);
    } // at most the one cooking finishes
    GIANT_CHECK(true);
}

GIANT_TEST("textures: a mesh's material draws its maps once they are ready; a file without uvs never does") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    const std::filesystem::path dir = fresh_dir("giant_texture_scene");
    const std::string glb = (dir / "ball.glb").generic_string();
    write_glb(glb, geometry::make_sphere(16, 8), true);
    const std::string bare = (dir / "bare.glb").generic_string();
    write_glb(bare, geometry::make_sphere(16, 8), false);
    const std::string albedo = write_png(dir / "albedo.png", 32, 32, 200, 100, 50);
    const std::string normal = write_png(dir / "normal.png", 32, 32, 128, 128, 255);
    const std::string orm = write_png(dir / "orm.png", 32, 32, 255, 200, 0);
    MeshLibrary meshes(renderer.get());
    GIANT_CHECK(meshes.load(glb).mesh != nullptr && meshes.find(glb)->has_uvs);
    GIANT_CHECK(meshes.load(bare).mesh != nullptr && !meshes.find(bare)->has_uvs);
    TextureLibrary textures(renderer.get(), {});
    SceneFrame frame;
    frame.textures = &textures;
    world::Scene scene;
    world::Entity ball;
    ball.shape = world::Shape::mesh;
    ball.mesh = glb;
    ball.material.maps.base_color = albedo;
    ball.material.maps.normal = normal;
    ball.material.maps.roughness_metal = orm;
    ball.material.maps.normal_green_down = true;
    scene.add(ball);
    world::Entity plain = ball; // the same look on a file without uvs: no textures
    plain.mesh = bare;
    plain.transform.position = {3.0, 0.0, 0.0};
    scene.add(plain);
    // A kit placed twice whose piece is a cube (built-in shapes have uvs)
    // with the ball's base colour alone.
    world::Kit kit;
    kit.name = "crate";
    world::KitPiece piece;
    piece.shape = world::Shape::cube;
    piece.material.maps.base_color = albedo;
    kit.pieces.push_back(piece);
    scene.kits.push_back(kit);
    for (double x : {10.0, 12.0}) {
        world::Entity placed;
        placed.shape = world::Shape::kit;
        placed.kit = "crate";
        placed.transform.position = {x, 0.0, 0.0};
        scene.add(placed);
    }
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() == 3); // the ball's, the bare file's, the crate's
    GIANT_CHECK(textures.entries().size() == 3);
    for (const render::Material& m : frame.materials) {
        GIANT_CHECK(m.base_color_texture == render::k_no_texture); // cooking still
    }
    // Requested with the normal map's settings.
    bool rough_found = false;
    for (const TextureLibrary::Entry& e : textures.entries()) {
        if (e.key.slot == TextureLibrary::Slot::roughness_metal) {
            rough_found = e.key.normal == normal && e.key.green_down;
        }
        if (e.key.slot == TextureLibrary::Slot::normal) GIANT_CHECK(e.key.green_down);
    }
    GIANT_CHECK(rough_found);
    textures.finish();
    scene_frame(scene, *renderer, meshes, frame); // nothing in the scene changed
    GIANT_CHECK(frame.cells_rebuilt == 0);
    usize textured = 0;
    for (const render::Material& m : frame.materials) {
        if (m.base_color_texture != render::k_no_texture) {
            ++textured;
            GIANT_CHECK((m.normal_texture != render::k_no_texture) == (m.roughness_metal_texture != render::k_no_texture));
            GIANT_CHECK(m.occlusion_texture == render::k_no_texture);
            GIANT_CHECK(renderer->texture(m.base_color_texture) != nullptr);
        }
    }
    GIANT_CHECK(textured == 2); // the ball's material and the crate's
    // The voxel world's colour: the texture's average (sRGB 200, 100, 50
    // in linear light), solid; untextured ones white.
    for (const render::Material& m : frame.materials) {
        GIANT_CHECK(m.coverage == 1.0f);
        if (m.base_color_texture == render::k_no_texture) {
            GIANT_CHECK(m.base_color_average.x == 1.0f && m.base_color_average.y == 1.0f);
        } else {
            GIANT_CHECK(std::abs(m.base_color_average.x - 0.578f) < 0.03f);
            GIANT_CHECK(std::abs(m.base_color_average.y - 0.127f) < 0.02f);
            GIANT_CHECK(std::abs(m.base_color_average.z - 0.032f) < 0.02f);
        }
    }
    usize crate = 0;
    for (const render::Material& m : frame.materials) {
        crate += m.base_color_texture != render::k_no_texture && m.normal_texture == render::k_no_texture ? 1u : 0u;
    }
    GIANT_CHECK(crate == 1);
    // Another mesh entity with the same maps shares the material.
    world::Entity twin = ball;
    twin.transform.position = {0.0, 0.0, 3.0};
    scene.add(twin);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() == 3);
}

GIANT_TEST("textures: skin looks go to the renderer, one per skin material; their detail maps cooked as normal maps") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    const std::filesystem::path dir = fresh_dir("giant_skin_scene");
    const std::string pores = write_png(dir / "pores.png", 32, 32, 128, 140, 255);
    MeshLibrary meshes(renderer.get());
    TextureLibrary textures(renderer.get(), {});
    SceneFrame frame;
    frame.textures = &textures;
    world::Scene scene;
    world::Entity face;
    face.shape = world::Shape::sphere;
    world::SkinLook look;
    look.scatter = 0.02f;
    look.transmission = 0.4f;
    look.detail_normal = pores;
    look.detail_tiling = 8.0f;
    look.detail_green_down = true;
    face.material.skin = look;
    scene.add(face);
    world::Entity twin = face; // the same skin: the same material and look
    twin.transform.position = {2.0, 0.0, 0.0};
    scene.add(twin);
    world::Entity arm = face; // another skin
    arm.transform.position = {4.0, 0.0, 0.0};
    arm.material.skin->scatter = 0.03f;
    arm.material.skin->detail_normal.clear();
    scene.add(arm);
    world::Entity rock;
    rock.transform.position = {6.0, 0.0, 0.0};
    scene.add(rock);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() == 3 && frame.skin_looks.size() == 2);
    usize skins = 0;
    for (const render::Material& m : frame.materials) {
        if (m.skin_look == render::k_no_skin_look) continue;
        ++skins;
        GIANT_CHECK(m.skin_look < frame.skin_looks.size());
    }
    GIANT_CHECK(skins == 2);
    const auto find_look = [&](float scatter) -> const render::SkinLook* {
        for (const render::SkinLook& l : frame.skin_looks) {
            if (l.scatter == scatter) return &l;
        }
        return nullptr;
    };
    const render::SkinLook* face_look = find_look(0.02f);
    GIANT_CHECK(face_look != nullptr && face_look->transmission == 0.4f && face_look->detail_tiling == 8.0f &&
                face_look->strength == 1.0f && face_look->lobe_roughness[1] == 1.3f);
    GIANT_CHECK(face_look != nullptr && face_look->detail_normal == render::k_no_texture); // cooking still
    // Asked for once, as a DirectX-authored normal map.
    GIANT_CHECK(textures.entries().size() == 1);
    if (!textures.entries().empty()) {
        const TextureLibrary::Entry& e = textures.entries()[0];
        GIANT_CHECK(e.key.slot == TextureLibrary::Slot::normal && e.key.green_down && e.key.path == pores);
    }
    textures.finish();
    scene_frame(scene, *renderer, meshes, frame);
    face_look = find_look(0.02f);
    GIANT_CHECK(face_look != nullptr && face_look->detail_normal != render::k_no_texture &&
                renderer->texture(face_look->detail_normal) != nullptr);
    const render::SkinLook* arm_look = find_look(0.03f);
    GIANT_CHECK(arm_look != nullptr && arm_look->detail_normal == render::k_no_texture);
    // The same skin in another colour: its own material, the same look.
    world::Entity tinted = arm;
    tinted.material.base_color = {0.5f, 0.3f, 0.2f};
    tinted.transform.position = {8.0, 0.0, 0.0};
    const world::EntityId tinted_id = scene.add(tinted);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() == 4 && frame.skin_looks.size() == 2);
    // Unused, its material is not skin for the renderer (skin's pass would
    // run for it); used again, it is.
    const auto skin_materials = [&] {
        usize n = 0;
        for (const render::Material& m : frame.materials) n += m.skin_look != render::k_no_skin_look ? 1u : 0u;
        return n;
    };
    GIANT_CHECK(skin_materials() == 3);
    scene.remove(tinted_id);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() == 4 && skin_materials() == 2);
    const world::EntityId tinted_again = scene.add(tinted);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(skin_materials() == 3);
    scene.remove(tinted_again);
    // A full table: later skin draws as plain, until a look is freed (its
    // last user gone) and the table made again.
    std::vector<world::EntityId> crowd;
    for (u32 i = 0; i < render::k_max_skin_looks + 4; ++i) {
        world::Entity e = arm;
        e.material.skin->scatter = 0.001f + 0.0001f * static_cast<float>(i);
        e.transform.position = {10.0 + i, 0.0, 0.0};
        crowd.push_back(scene.add(e));
    }
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.skin_looks.size() == render::k_max_skin_looks);
    const auto plain = [&] {
        usize n = 0;
        for (usize i = 0; i < frame.materials.size(); ++i) {
            n += frame.materials[i].skin_look == render::k_no_skin_look && frame.users[i] > 0 ? 1u : 0u;
        }
        return n;
    };
    GIANT_CHECK(plain() == 7); // the rock's and the six skins past the table's end (two looks were there)
    for (usize i = 0; i < 8; ++i) scene.remove(crowd[i]);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(plain() == 1); // the rock's alone
    // Full again, with a look two materials share: one of them going frees
    // no look, so the table stays as it is (no cell is drawn again for it).
    world::Entity shared = arm;
    shared.material.skin->scatter = 0.001f + 0.0001f * 20.0f; // crowd[20]'s look
    shared.material.base_color = {0.1f, 0.2f, 0.3f};
    const world::EntityId tint = scene.add(shared);
    for (u32 i = 0; i < 4; ++i) {
        world::Entity e = arm;
        e.material.skin->scatter = 0.05f + 0.001f * static_cast<float>(i);
        crowd.push_back(scene.add(e));
    }
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.skin_looks.size() == render::k_max_skin_looks && plain() == 3);
    const usize rows = frame.materials.size();
    scene.remove(tint);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() == rows && plain() == 3); // (its row lingers, unused)
    scene.remove(crowd[20]); // the look's last user: one refused skin takes its place
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() < rows && plain() == 2);
}

GIANT_TEST("textures: eye looks go to the renderer, shared by eyes alike; an eye is not skin; a full table draws plain") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    MeshLibrary meshes(renderer.get());
    SceneFrame frame;
    world::Scene scene;
    world::Entity left;
    left.shape = world::Shape::sphere;
    world::EyeLook look;
    look.iris_radius = 0.2f;
    look.iris_color = {0.1f, 0.3f, 0.9f};
    look.painted = true;
    left.material.eye = look;
    left.material.skin = world::SkinLook{}; // (an eye all the same)
    const world::EntityId left_id = scene.add(left);
    world::Entity right = left; // the same eye: the same material and look
    right.transform.position = {0.07, 0.0, 0.0};
    scene.add(right);
    world::Entity bloodshot = left; // the same look on another white: its own material
    bloodshot.transform.position = {2.0, 0.0, 0.0};
    bloodshot.material.base_color = {0.9f, 0.6f, 0.6f};
    const world::EntityId bloodshot_id = scene.add(bloodshot);
    world::Entity brown = left; // another look
    brown.transform.position = {4.0, 0.0, 0.0};
    brown.material.eye->iris_color = {0.3f, 0.2f, 0.1f};
    scene.add(brown);
    world::Entity rock;
    rock.transform.position = {6.0, 0.0, 0.0};
    scene.add(rock);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() == 4 && frame.eye_looks.size() == 2 && frame.skin_looks.empty());
    const auto eyes = [&] {
        usize n = 0;
        for (const render::Material& m : frame.materials) {
            if (m.eye_look == render::k_no_eye_look) continue;
            ++n;
            GIANT_CHECK(m.eye_look < frame.eye_looks.size() && m.skin_look == render::k_no_skin_look);
        }
        return n;
    };
    GIANT_CHECK(eyes() == 3);
    bool found = false;
    for (const render::EyeLook& l : frame.eye_looks) {
        if (l.iris_color.z != 0.9f) continue;
        found = true;
        GIANT_CHECK(l.iris_radius == 0.2f && l.painted && l.iris_depth == 0.35f && l.cornea_roughness == 0.06f);
    }
    GIANT_CHECK(found);
    // Unused, a material is not an eye for the renderer; used again, it is.
    scene.remove(bloodshot_id);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() == 4 && eyes() == 2);
    scene.add(bloodshot);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(eyes() == 3 && frame.eye_looks.size() == 2);
    // A full table: later eyes draw plain until a look is freed.
    std::vector<world::EntityId> crowd;
    for (u32 i = 0; i < render::k_max_eye_looks; ++i) {
        world::Entity e = brown;
        e.material.eye->bulge = 0.001f * static_cast<float>(i + 1);
        e.transform.position = {10.0 + i, 0.0, 0.0};
        crowd.push_back(scene.add(e));
    }
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.eye_looks.size() == render::k_max_eye_looks);
    const auto plain = [&] {
        usize n = 0;
        for (usize i = 0; i < frame.materials.size(); ++i) {
            n += frame.materials[i].eye_look == render::k_no_eye_look && frame.users[i] > 0 ? 1u : 0u;
        }
        return n;
    };
    GIANT_CHECK(plain() == 3); // the rock's and the two eyes past the table's end
    scene.remove(crowd[0]);
    scene.remove(crowd[1]);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(plain() == 1); // the rock's alone
    eyes(); // every look still in the table
    (void)left_id;
    // A kit's piece is an eye too, and a change to its eye alone is drawn.
    scene.remove(crowd[2]);
    scene.remove(crowd[3]);
    world::Kit kit;
    kit.name = "head";
    world::KitPiece ball;
    ball.shape = world::Shape::sphere;
    ball.material.eye = world::EyeLook{};
    ball.material.eye->shade = 0.875f;
    kit.pieces.push_back(ball);
    scene.kits.push_back(kit);
    world::Entity placed;
    placed.shape = world::Shape::kit;
    placed.kit = "head";
    placed.transform.position = {-20.0, 0.0, 0.0};
    scene.add(placed);
    scene_frame(scene, *renderer, meshes, frame);
    const auto shaded = [&](float shade) {
        usize n = 0;
        for (usize i = 0; i < frame.materials.size(); ++i) {
            const u32 at = frame.materials[i].eye_look;
            n += frame.users[i] > 0 && at != render::k_no_eye_look && frame.eye_looks[at].shade == shade ? 1u : 0u;
        }
        return n;
    };
    GIANT_CHECK(shaded(0.875f) == 1 && shaded(0.625f) == 0);
    scene.kits[0].pieces[0].material.eye->shade = 0.625f;
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(shaded(0.875f) == 0 && shaded(0.625f) == 1);
    eyes();
}

GIANT_TEST("textures: set_entity takes maps, refuses missing files, and get_textures reports them") {
    const std::filesystem::path dir = fresh_dir("giant_texture_commands");
    const std::string albedo = write_png(dir / "albedo.png", 16, 16, 255, 0, 0);
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    TextureLibrary textures(nullptr, {});
    EditorState state;
    state.meshes = &meshes;
    state.textures = &textures;
    register_scene_commands(commands, state);
    const auto run = [&](const char* name, const Json& args) { return commands.execute(name, args, {}); };
    const auto made = run("create_entity", {{"name", "crate"}, {"maps", {{"base_color", albedo}}}});
    GIANT_CHECK(made.ok);
    if (!made.ok) {
        std::fprintf(stderr, "    %s\n", made.error.c_str());
        return;
    }
    const u64 id = made.value["id"].get<u64>();
    GIANT_CHECK(made.value["maps"]["base_color"] == albedo);
    const world::Entity* e = state.scene.find(id);
    GIANT_CHECK(e != nullptr && e->material.maps.base_color == albedo);
    const auto missing = run("set_entity", {{"id", id}, {"maps", {{"normal", (dir / "none.png").generic_string()}}}});
    GIANT_CHECK(!missing.ok && missing.error.find("none.png") != std::string::npos);
    const auto green = run("set_entity", {{"id", id}, {"maps", {{"normal_green_down", true}}}});
    GIANT_CHECK(green.ok && green.value["maps"]["base_color"] == albedo && green.value["maps"]["normal_green_down"] == true);
    // Undo puts the maps back as they were.
    GIANT_CHECK(commands.undo());
    GIANT_CHECK(!state.scene.find(id)->material.maps.normal_green_down);
    const auto cleared = run("set_entity", {{"id", id}, {"maps", {{"base_color", ""}}}});
    GIANT_CHECK(cleared.ok && !cleared.value.contains("maps"));
    // Skin: fields left out keep theirs (the defaults when new), a missing
    // detail map is refused, "on": false takes it off; undoable.
    const auto skinned = run("set_entity", {{"id", id}, {"skin", {{"scatter", 0.02}, {"lobe_roughness", {0.6, 1.5}}}}});
    GIANT_CHECK(skinned.ok && skinned.value["skin"]["scatter"].get<float>() == 0.02f &&
                skinned.value["skin"]["lobe_roughness"][1].get<float>() == 1.5f &&
                skinned.value["skin"]["transmission"].get<float>() == 1.0f);
    const auto detail = run("set_entity", {{"id", id}, {"skin", {{"detail_normal", albedo}, {"detail_tiling", 8}}}});
    GIANT_CHECK(detail.ok && detail.value["skin"]["detail_normal"] == albedo &&
                detail.value["skin"]["scatter"].get<float>() == 0.02f);
    const auto no_file = run("set_entity", {{"id", id}, {"skin", {{"detail_normal", (dir / "none.png").generic_string()}}}});
    GIANT_CHECK(!no_file.ok && no_file.error.find("none.png") != std::string::npos);
    GIANT_CHECK(!run("set_entity", {{"id", id}, {"skin", {{"scatter", 0.5}}}}).ok);
    GIANT_CHECK(!run("set_entity", {{"id", id}, {"skin", {{"lobe_roughness", {1.0}}}}}).ok);
    const auto off = run("set_entity", {{"id", id}, {"skin", {{"on", false}}}});
    GIANT_CHECK(off.ok && !off.value.contains("skin") && !state.scene.find(id)->material.skin);
    GIANT_CHECK(commands.undo());
    GIANT_CHECK(state.scene.find(id)->material.skin && state.scene.find(id)->material.skin->detail_tiling == 8.0f);
    GIANT_CHECK(run("set_entity", {{"id", id}, {"skin", {{"on", false}}}}).ok);
    // An eye: fields left out keep theirs; it takes skin off, and skin it.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"skin", {{"scatter", 0.02}}}}).ok);
    const auto eyed = run("set_entity", {{"id", id}, {"eye", {{"iris_radius", 0.2}, {"iris_color", {0.1, 0.3, 0.9}}}}});
    GIANT_CHECK(eyed.ok && !eyed.value.contains("skin") && eyed.value["eye"]["iris_radius"].get<float>() == 0.2f &&
                eyed.value["eye"]["iris_color"][2].get<float>() == 0.9f &&
                eyed.value["eye"]["iris_depth"].get<float>() == 0.35f && !eyed.value["eye"].contains("painted"));
    const auto painted = run("set_entity", {{"id", id}, {"eye", {{"painted", true}}}});
    GIANT_CHECK(painted.ok && painted.value["eye"]["painted"] == true &&
                painted.value["eye"]["iris_radius"].get<float>() == 0.2f);
    GIANT_CHECK(!run("set_entity", {{"id", id}, {"eye", {{"iris_depth", 0.9}}}}).ok);
    GIANT_CHECK(!run("set_entity", {{"id", id}, {"eye", {{"iris_color", {1.0, 1.0}}}}}).ok);
    const auto back = run("set_entity", {{"id", id}, {"skin", {{"scatter", 0.02}}}});
    GIANT_CHECK(back.ok && back.value.contains("skin") && !back.value.contains("eye"));
    GIANT_CHECK(commands.undo());
    GIANT_CHECK(state.scene.find(id)->material.eye && state.scene.find(id)->material.eye->painted &&
                !state.scene.find(id)->material.skin);
    const auto eye_off = run("set_entity", {{"id", id}, {"eye", {{"on", false}}}});
    GIANT_CHECK(eye_off.ok && !eye_off.value.contains("eye") && !eye_off.value.contains("skin"));
    // Skin given with the eye taken off is skin; skin taken off leaves an eye be.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"eye", {{"bulge", 0.5}}}}).ok);
    const auto swapped = run("set_entity", {{"id", id}, {"skin", {{"scatter", 0.02}}}, {"eye", {{"on", false}}}});
    GIANT_CHECK(swapped.ok && swapped.value.contains("skin") && !swapped.value.contains("eye"));
    GIANT_CHECK(run("set_entity", {{"id", id}, {"eye", {{"bulge", 0.5}}}}).ok);
    const auto kept = run("set_entity", {{"id", id}, {"skin", {{"on", false}}}});
    GIANT_CHECK(kept.ok && !kept.value.contains("skin") && kept.value["eye"]["bulge"].get<float>() == 0.5f);
    GIANT_CHECK(run("set_entity", {{"id", id}, {"eye", {{"on", false}}}}).ok);

    // Waiting asks for what the scene names, before any frame has drawn it.
    GIANT_CHECK(run("create_entity", {{"name", "ball"}, {"shape", "sphere"}, {"maps", {{"base_color", albedo}}}}).ok);
    GIANT_CHECK(textures.entries().empty());
    const auto listed = run("get_textures", {{"wait", true}});
    GIANT_CHECK(listed.ok && listed.value["pending"] == 0 && listed.value["textures"].size() == 1);
    if (listed.ok && listed.value["textures"].size() == 1) {
        const Json& t = listed.value["textures"][0];
        GIANT_CHECK(t["status"] == "ready" && t["slot"] == "base_color" && t["width"] == 16 && t["mips"] == 5);
    }
    GIANT_CHECK(run("reload_textures", Json::object()).ok);
}

GIANT_TEST("textures: a glTF's own look comes with it, its images named inside it and cooked from there") {
    const std::filesystem::path dir = fresh_dir("giant_gltf_look");
    std::filesystem::create_directories(dir / "maps");
    write_png(dir / "maps" / "rust normal.png", 8, 8, 128, 128, 255);
    // A real PNG inside the file.
    std::vector<u8> rgba(16 * 16 * 4, 200);
    const std::string png = encode_png_rgba(16, 16, rgba);
    const std::vector<u8> glb = geometry::testing::two_material_quad("maps/rust%20normal.png", png);
    std::ofstream(dir / "quad.glb", std::ios::binary)
        .write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    const std::string quad = (dir / "quad.glb").generic_string();
    // A file with no materials (the bare sphere).
    const std::string bare = (dir / "bare.glb").generic_string();
    write_glb(bare, geometry::make_sphere(16, 8), true);

    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    TextureLibrary textures(nullptr, dir / "cache");
    EditorState state;
    state.meshes = &meshes;
    state.textures = &textures;
    register_scene_commands(commands, state);
    register_scatter_commands(commands, state);
    const auto run = [&](const char* name, const Json& args) { return commands.execute(name, args, {}); };

    const auto imported = run("import_mesh", {{"path", quad}});
    GIANT_CHECK(imported.ok);
    if (!imported.ok) {
        std::fprintf(stderr, "    %s\n", imported.error.c_str());
        return;
    }
    GIANT_CHECK(imported.value["materials"] == 2 && imported.value["uvs"] == true);
    const Json& look = imported.value["material"];
    GIANT_CHECK(std::fabs(look["roughness"].get<float>() - 0.3f) < 1e-6f && look["emissive"][0] == 4.0f);
    // Two materials: drawn in parts, rust (two of three triangles) first, each with its own look and maps.
    GIANT_CHECK(imported.value["parts"].size() == 2 && imported.value["parts"][0]["name"] == "rust" &&
                imported.value["parts"][0]["triangles"] == 2 && imported.value["parts"][1]["name"] == "paint" &&
                imported.value["parts"][1]["triangles"] == 1);
    GIANT_CHECK(look["maps"]["base_color"] == quad + "#image0");
    {
        const MeshLibrary::Mesh* mesh = meshes.find(quad);
        GIANT_CHECK(mesh != nullptr && mesh->parts.size() == 2 && mesh->parts[1].material &&
                    mesh->parts[1].material->metallic == 0.0f && mesh->parts[1].material->maps.empty());
        // Drawn whole (scatter), rust covers under three quarters: its look without its maps.
        GIANT_CHECK(mesh != nullptr && mesh->whole_material && mesh->whole_material->maps.empty() &&
                    std::fabs(mesh->whole_material->roughness - 0.3f) < 1e-6f);
    }
    // The quad without "paint": rust alone, with its maps.
    const std::string only = (dir / "only.glb").generic_string();
    {
        const std::string json_from = R"("indices":2,"material":0})";
        std::string text(glb.begin(), glb.end());
        const usize at = text.find(json_from);
        text.replace(at, json_from.size(), R"("indices":3,"material":1})"); // the same length
        std::ofstream(dir / "only.glb", std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    const auto rust = run("import_mesh", {{"path", only}});
    GIANT_CHECK(rust.ok && rust.value["materials"] == 1);
    if (!rust.ok) return;
    const Json& maps = rust.value["material"]["maps"];
    GIANT_CHECK(maps["base_color"] == only + "#image0" && maps["occlusion"] == only + "#image2");
    GIANT_CHECK(maps["normal"].get<std::string>().ends_with("giant_gltf_look/maps/rust normal.png"));
    GIANT_CHECK(!std::filesystem::exists("giant_cache/gltf_images")); // nothing copied out

    // An entity given the mesh alone takes the file's look; look fields given lie over it.
    const auto made = run("create_entity", {{"mesh", only}, {"surface", "metal"}, {"sway", 1.0}});
    GIANT_CHECK(made.ok);
    if (!made.ok) return;
    const u64 id = made.value["id"].get<u64>();
    {
        const world::Entity* e = state.scene.find(id);
        GIANT_CHECK(e != nullptr && e->material.maps.base_color == only + "#image0" &&
                    std::fabs(e->material.metallic - 0.7f) < 1e-6f);
        GIANT_CHECK(e != nullptr && e->material.surface == world::Surface::metal && e->material.sway == 1.0f);
    }
    const auto own = run("create_entity", {{"mesh", only}, {"base_color", {0.1, 0.2, 0.3}}});
    const world::Entity* o = own.ok ? state.scene.find(own.value["id"].get<u64>()) : nullptr;
    GIANT_CHECK(o != nullptr && o->material.base_color.x == 0.1f && o->material.maps.base_color == only + "#image0" &&
                std::fabs(o->material.roughness - 0.3f) < 1e-6f);
    // A flag alone is not a look: the file's maps come, the flag kept.
    const auto flagged = run("create_entity", {{"mesh", only}, {"maps", {{"normal_green_down", true}}}});
    const world::Entity* f = flagged.ok ? state.scene.find(flagged.value["id"].get<u64>()) : nullptr;
    GIANT_CHECK(f != nullptr && !f->material.maps.base_color.empty() && f->material.maps.normal_green_down);
    // Edits and moves keep the look; the same file again keeps it too.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"roughness", 0.9}}).ok);
    GIANT_CHECK(run("set_entity", {{"id", id}, {"position", {1, 2, 3}}, {"mesh", only}}).ok);
    GIANT_CHECK(std::fabs(state.scene.find(id)->material.roughness - 0.9f) < 1e-6f);
    // Another file given with a colour: its look (its first part's, with its maps) under the colour, the
    // old file's maps gone.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"mesh", quad}, {"base_color", {1, 0, 0}}}).ok);
    GIANT_CHECK(state.scene.find(id)->material.maps.base_color == quad + "#image0" &&
                state.scene.find(id)->material.maps.occlusion != only + "#image2" &&
                state.scene.find(id)->material.base_color.x == 1.0f &&
                std::fabs(state.scene.find(id)->material.roughness - 0.3f) < 1e-6f);
    // Another file without materials: the default look comes.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"mesh", bare}}).ok);
    GIANT_CHECK(state.scene.find(id)->material.maps.empty() && state.scene.find(id)->material.roughness == 0.5f &&
                state.scene.find(id)->material.surface == world::Surface::metal);
    // A scatter item given the mesh takes the look too.
    const auto layer = run("add_scatter_layer", {{"name", "crates"}, {"items", {{{"mesh", only}}}}});
    GIANT_CHECK(layer.ok && state.scene.scatter.size() == 1 &&
                state.scene.scatter[0].items[0].material.maps.occlusion == only + "#image2");
    // One given the file with two materials draws it whole: rust's values without its maps.
    const auto mixed = run("add_scatter_layer", {{"name", "quads"}, {"items", {{{"mesh", quad}}}}});
    GIANT_CHECK(mixed.ok && state.scene.scatter.size() == 2 && state.scene.scatter[1].items[0].material.maps.empty() &&
                std::fabs(state.scene.scatter[1].items[0].material.roughness - 0.3f) < 1e-6f);
    // Maps may name an image inside a glTF directly.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"maps", {{"base_color", only + "#image0"}}}}).ok);
    GIANT_CHECK(!run("set_entity", {{"id", id}, {"maps", {{"base_color", (dir / "none.glb#image0").generic_string()}}}}).ok);

    // The textures cook from inside the file (and come back from the cache).
    const TextureLibrary::Ref base = textures.request({only + "#image0", TextureLibrary::Slot::base_color, false, {}});
    const TextureLibrary::Ref missing = textures.request({only + "#image1", TextureLibrary::Slot::occlusion, false, {}});
    textures.finish();
    GIANT_CHECK(textures.entry(base)->status == TextureLibrary::Status::ready && textures.entry(base)->width == 16);
    GIANT_CHECK(textures.entry(missing)->status == TextureLibrary::Status::failed); // image 1 is beside, not inside
    GIANT_CHECK(std::filesystem::exists(textures.cache_file(textures.entry(base)->key)));
    // A GLB by another name (a .vrm avatar): its images are named and cook the same way.
    std::filesystem::copy_file(dir / "only.glb", dir / "avatar.vrm");
    const std::string avatar = (dir / "avatar.vrm").generic_string();
    const auto vrm = run("import_mesh", {{"path", avatar}});
    GIANT_CHECK(vrm.ok && vrm.value["material"]["maps"]["base_color"] == avatar + "#image0");
    GIANT_CHECK(run("set_entity", {{"id", id}, {"maps", {{"base_color", avatar + "#image0"}}}}).ok);
    const TextureLibrary::Ref vrm_base = textures.request({avatar + "#image0", TextureLibrary::Slot::base_color, false, {}});
    textures.finish();
    GIANT_CHECK(textures.entry(vrm_base)->status == TextureLibrary::Status::ready);
}

GIANT_TEST("mesh parts: waiting for textures asks for every part's maps; a scattered kit's piece, drawn whole, leaves the first part's maps out") {
    const std::filesystem::path dir = fresh_dir("giant_parts_maps");
    std::vector<u8> rgba(16 * 16 * 4, 200);
    const std::string png = encode_png_rgba(16, 16, rgba);
    const std::vector<u8> glb = geometry::testing::two_material_quad("n.png", png);
    std::ofstream(dir / "quad.glb", std::ios::binary)
        .write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    const std::string quad = (dir / "quad.glb").generic_string(); // "rust" (with maps) first, "paint" second
    // The same with the materials' triangles swapped: "paint" first, "rust" and its maps the second part.
    const std::string flipped = (dir / "flipped.glb").generic_string();
    {
        std::string text(glb.begin(), glb.end());
        const std::string a = R"("indices":2,"material":0})";
        const std::string b = R"("indices":3,"material":1})";
        const usize at_a = text.find(a);
        const usize at_b = text.find(b);
        GIANT_CHECK(at_a != std::string::npos && at_b != std::string::npos);
        if (at_a == std::string::npos || at_b == std::string::npos) return;
        text.replace(at_a, a.size(), R"("indices":3,"material":0})");
        text.replace(at_b, b.size(), R"("indices":2,"material":1})");
        std::ofstream(dir / "flipped.glb", std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    {
        // "paint" given its metal map on the first uvs: its part keeps it, but drawn whole (it
        // covers two triangles of three) the map goes and with it the metal, which the file says.
        std::string text(glb.begin(), glb.end());
        const std::string a = R"("indices":2,"material":0})";
        const std::string b = R"("indices":3,"material":1})";
        const std::string uv = R"("texCoord":1)";
        const usize at_a = text.find(a);
        const usize at_b = text.find(b);
        const usize at_uv = text.find(uv);
        GIANT_CHECK(at_a != std::string::npos && at_b != std::string::npos && at_uv != std::string::npos);
        if (at_a == std::string::npos || at_b == std::string::npos || at_uv == std::string::npos) return;
        text.replace(at_a, a.size(), R"("indices":3,"material":0})");
        text.replace(at_b, b.size(), R"("indices":2,"material":1})");
        text.replace(at_uv, uv.size(), R"("texCoord":0)");
        std::ofstream(dir / "metal.glb", std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
        MeshLibrary meshes{nullptr};
        const MeshLibrary::Mesh* mesh = meshes.load((dir / "metal.glb").generic_string()).mesh;
        GIANT_CHECK(mesh != nullptr && mesh->parts.size() == 2 && mesh->parts[0].material && mesh->whole_material);
        if (mesh == nullptr || mesh->parts.size() != 2 || !mesh->parts[0].material || !mesh->whole_material) return;
        GIANT_CHECK(!mesh->parts[0].material->maps.roughness_metal.empty() && mesh->parts[0].material->metallic == 1.0f);
        GIANT_CHECK(mesh->whole_material->maps.empty() && mesh->whole_material->metallic == 0.0f);
        usize told = 0;
        for (const std::string& w : mesh->warnings) told += w.starts_with("drawn whole (scattered): ") ? usize{1} : usize{0};
        GIANT_CHECK(told == 1);
    }
    {
        ai::CommandRegistry commands;
        MeshLibrary meshes{nullptr};
        TextureLibrary textures(nullptr, dir / "cache");
        EditorState state;
        state.meshes = &meshes;
        state.textures = &textures;
        register_scene_commands(commands, state);
        const auto run = [&](const char* name, const Json& args) { return commands.execute(name, args, {}); };
        const auto made = run("create_entity", {{"mesh", flipped}});
        GIANT_CHECK(made.ok);
        const MeshLibrary::Mesh* mesh = meshes.find(flipped);
        GIANT_CHECK(mesh != nullptr && mesh->parts.size() == 2 && mesh->parts[0].name == "paint" &&
                    mesh->parts[1].material && mesh->parts[1].material->maps.base_color == flipped + "#image0");
        // The entity's own look ("paint") names no map: the second part's are asked for all the same.
        const world::Entity* e = made.ok ? state.scene.find(made.value["id"].get<u64>()) : nullptr;
        GIANT_CHECK(e != nullptr && e->material.maps.empty() && textures.entries().empty());
        const auto listed = run("get_textures", {{"wait", true}});
        GIANT_CHECK(listed.ok);
        bool base = false;
        if (listed.ok) {
            for (const Json& t : listed.value["textures"]) base = base || (t["path"] == flipped + "#image0" && t["status"] == "ready");
        }
        GIANT_CHECK(base);
    }

    // Scattered: a kit whose pieces are the quad with the look it took from the file (its first
    // part's, maps and all), and the quad given a map of its own.
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    const std::string albedo = write_png(dir / "albedo.png", 32, 32, 200, 100, 50);
    MeshLibrary meshes(renderer.get());
    const MeshLibrary::Mesh* mesh = meshes.load(quad).mesh;
    GIANT_CHECK(mesh != nullptr && mesh->parts.size() == 2 && mesh->material && !mesh->material->maps.empty() &&
                mesh->whole_material && mesh->whole_material->maps.empty());
    if (mesh == nullptr || !mesh->material) return;
    TextureLibrary textures(renderer.get(), {});
    SceneFrame frame;
    frame.textures = &textures;
    world::Scene scene;
    scene.terrain.set_samples(16);
    scene.terrain.set({0, 0}, world::Terrain::Heights(256, 3.0f));
    world::Kit kit;
    kit.name = "pair";
    kit.pieces.resize(2);
    kit.pieces[0].shape = world::Shape::mesh;
    kit.pieces[0].mesh = mesh->path;
    kit.pieces[0].material = *mesh->material;
    kit.pieces[1] = kit.pieces[0];
    kit.pieces[1].material.maps = {};
    kit.pieces[1].material.maps.base_color = albedo;
    kit.pieces[1].material.roughness = 0.9f; // (told apart below)
    scene.kits.push_back(kit);
    world::ScatterLayer layer;
    layer.name = "pairs";
    layer.spacing = 8.0f;
    world::ScatterItem item;
    item.shape = world::Shape::kit;
    item.kit = "pair";
    layer.items = {item};
    scene.scatter.push_back(layer);
    ScatterPlacer placer;
    placer.finish(scene, scene.terrain.take_changes());
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &placer);
    usize from_file = 0, own = 0;
    for (const render::Instance& i : frame.instances) {
        if (i.mesh == render::k_no_mesh) continue;
        GIANT_CHECK(i.mesh == mesh->id); // whole, not in parts
        const std::array<u32, 4>& refs = frame.material_textures[i.material];
        if (frame.materials[i.material].roughness == 0.9f) {
            ++own;
            GIANT_CHECK(refs[0] != TextureLibrary::k_no_ref); // a map given to the piece itself stays
        } else {
            ++from_file;
            GIANT_CHECK(refs[0] == TextureLibrary::k_no_ref && refs[1] == TextureLibrary::k_no_ref &&
                        refs[3] == TextureLibrary::k_no_ref); // rust's maps would lie over paint's triangle
        }
    }
    GIANT_CHECK(from_file > 0 && own == from_file);
    textures.finish();
}

GIANT_TEST("textures: a cut-out material tells the voxel world how much of it its cutoff leaves") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    const std::filesystem::path dir = fresh_dir("giant_texture_coverage");
    // Left quarter opaque green, the rest see-through at alpha 64 (a
    // quarter): a cutoff of 0.5 leaves 25%, of 0.2 all of it.
    std::vector<u8> rgba(usize{32} * 32 * 4);
    for (u32 y = 0; y < 32; ++y) {
        for (u32 x = 0; x < 32; ++x) {
            u8* t = &rgba[(usize{y} * 32 + x) * 4];
            t[0] = 20;
            t[1] = 180;
            t[2] = 20;
            t[3] = x < 8 ? 255 : 64;
        }
    }
    const std::string png = encode_png_rgba(32, 32, rgba);
    const std::filesystem::path leaf = dir / "leaf.png";
    std::ofstream(leaf, std::ios::binary).write(png.data(), static_cast<std::streamsize>(png.size()));
    MeshLibrary meshes(renderer.get());
    TextureLibrary textures(renderer.get(), {});
    SceneFrame frame;
    frame.textures = &textures;
    world::Scene scene;
    world::Entity card;
    card.shape = world::Shape::plane;
    card.material.maps.base_color = leaf.generic_string();
    card.material.alpha_cutoff = 0.5f;
    const world::EntityId id = scene.add(card);
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials.size() == 1 && frame.materials[0].coverage == 1.0f); // cooking: solid
    textures.finish();
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.materials[0].base_color_texture != render::k_no_texture);
    GIANT_CHECK(std::abs(frame.materials[0].coverage - 0.25f) < 0.03f);
    GIANT_CHECK(frame.materials[0].base_color_average.y > 0.3f);
    // A lower cutoff leaves more (a new material row, its texture ready).
    scene.modify(id)->material.alpha_cutoff = 0.2f;
    scene_frame(scene, *renderer, meshes, frame);
    bool found = false;
    for (usize i = 0; i < frame.materials.size(); ++i) {
        if (frame.users[i] > 0) {
            found = true;
            GIANT_CHECK(frame.materials[i].coverage > 0.97f);
        }
    }
    GIANT_CHECK(found);
}
