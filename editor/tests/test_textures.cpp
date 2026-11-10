// Material textures: image files cooked in the background, kept in a disk
// cache, uploaded under a budget, and drawn by the materials naming them.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/scene_render.h"
#include "../src/texture_library.h"

#include "giant_test.h"

#include "giant/core/png.h"
#include "giant/geometry/mesh.h"
#include "giant/rhi/rhi.h"

#include "gltf_test_files.h"

#include <algorithm>
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
            for (int i = 0; i < 2000 && !done(); ++i) {
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
        // finish() puts it up whole (a screenshot that must be sharp).
        textures.finish();
        draw();
        GIANT_CHECK(until([&] { return at(0); }) && textures.pending() == 0);
        if (cached) {
            // Its cooked file gone: the finer read fails, it is cooked again
            // and draws what it has meanwhile.
            wanted = 9;
            GIANT_CHECK(until([&] { return at(3); }));
            std::error_code ec;
            std::filesystem::remove_all(dir / "cache", ec);
            wanted = 0;
            GIANT_CHECK(until([&] { return textures.id(a) != id; }));
            GIANT_CHECK(e()->status == TextureLibrary::Status::ready && !e()->from_cache);
            GIANT_CHECK(renderer->texture(id) == nullptr);
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
    // Rust covers two of three triangles: under three quarters, so no maps.
    GIANT_CHECK(look["maps"].empty());
    bool split_warning = false;
    for (const Json& w : imported.value["warnings"]) {
        split_warning = split_warning || w.get<std::string>().find("without its maps") != std::string::npos;
    }
    GIANT_CHECK(split_warning);
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
    // Another file given with a colour: its look under the colour, the old file's maps gone.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"mesh", quad}, {"base_color", {1, 0, 0}}}).ok);
    GIANT_CHECK(state.scene.find(id)->material.maps.empty() && state.scene.find(id)->material.base_color.x == 1.0f &&
                std::fabs(state.scene.find(id)->material.roughness - 0.3f) < 1e-6f);
    // Another file without materials: the default look comes.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"mesh", bare}}).ok);
    GIANT_CHECK(state.scene.find(id)->material.maps.empty() && state.scene.find(id)->material.roughness == 0.5f &&
                state.scene.find(id)->material.surface == world::Surface::metal);
    // A scatter item given the mesh takes the look too.
    const auto layer = run("add_scatter_layer", {{"name", "crates"}, {"items", {{{"mesh", only}}}}});
    GIANT_CHECK(layer.ok && state.scene.scatter.size() == 1 &&
                state.scene.scatter[0].items[0].material.maps.occlusion == only + "#image2");
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
