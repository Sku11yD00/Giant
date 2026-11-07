#include "giant/world/scene.h"

#include "giant_test.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::world;

namespace {
Entity named(const char* name, Shape shape = Shape::cube) {
    Entity e;
    e.name = name;
    e.shape = shape;
    return e;
}
bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
} // namespace

GIANT_TEST("world: entities get stable, never reused ids") {
    Scene scene;
    const EntityId a = scene.add(named("a"));
    const EntityId b = scene.add(named("b"));
    const EntityId c = scene.add(named("c"));
    GIANT_CHECK(a == 1 && b == 2 && c == 3);
    GIANT_CHECK(scene.find(b) != nullptr && scene.find(b)->name == "b");

    const auto removed = scene.remove(b);
    GIANT_CHECK(removed.has_value() && removed->index == 1 && removed->entity.name == "b");
    GIANT_CHECK(scene.find(b) == nullptr && scene.find(c)->name == "c");
    GIANT_CHECK(!scene.remove(b).has_value());
    GIANT_CHECK(scene.add(named("d")) == 4); // 2 is not handed out again

    // Undo of a delete puts the entity back where it was, with its id.
    scene.add(removed->entity, removed->index);
    GIANT_CHECK(scene.size() == 4);
    GIANT_CHECK(scene.entities()[1].id == b && scene.entities()[2].id == c);
    GIANT_CHECK(scene.find(c)->name == "c" && scene.find(4)->name == "d");

    scene.clear();
    GIANT_CHECK(scene.size() == 0 && scene.find(a) == nullptr);
    GIANT_CHECK(scene.add(named("e")) == 5);
}

GIANT_TEST("world: the sun direction follows elevation and compass heading") {
    Lighting l;
    l.sun_elevation_degrees = 90.0f;
    Vec3 d = sun_direction(l);
    GIANT_CHECK_NEAR(d.y, 1.0, 1e-6);
    l.sun_elevation_degrees = 0.0f;
    l.sun_azimuth_degrees = 0.0f; // north: -Z
    d = sun_direction(l);
    GIANT_CHECK_NEAR(d.z, -1.0, 1e-6);
    l.sun_azimuth_degrees = 90.0f; // east: +X
    d = sun_direction(l);
    GIANT_CHECK_NEAR(d.x, 1.0, 1e-6);
    GIANT_CHECK_NEAR(d.z, 0.0, 1e-6);
}

GIANT_TEST("world: scenes save and load without losing anything") {
    Scene scene;
    scene.lighting.sun_elevation_degrees = 12.5f;
    scene.lighting.sky_color = {0.1f, 0.2f, 0.3f};
    scene.lighting.exposure = 1.5f;
    Entity ground = named("Ground", Shape::plane);
    ground.transform.scale = {100.0f, 1.0f, 100.0f};
    ground.material.base_color = {0.3f, 0.5f, 0.2f};
    ground.material.roughness = 0.9f;
    scene.add(ground);
    Entity ball = named("Ball \"quoted\" ünïcode", Shape::sphere);
    ball.transform.position = {1'000'000.125, 2.5, -3.0}; // doubles survive
    ball.transform.rotation = quat_from_euler({10.0f, 45.0f, 0.0f});
    ball.material.metallic = 1.0f;
    ball.material.emissive = {4.0f, 2.0f, 0.0f};
    ball.visible = false;
    scene.add(ball);
    scene.remove(scene.add(named("deleted")));

    const std::string text = save_scene(scene);
    GIANT_CHECK(contains(text, "\"format\": \"giant-scene\""));
    Scene loaded;
    const auto error = load_scene(text, loaded);
    GIANT_CHECK(!error.has_value());
    if (error) {
        std::fprintf(stderr, "    %s\n", error->message.c_str());
        return;
    }
    GIANT_CHECK(loaded.size() == 2);
    GIANT_CHECK(loaded.next_id() == 4); // the deleted entity's id stays used
    const Entity* b = loaded.find(2);
    GIANT_CHECK(b != nullptr);
    if (b != nullptr) {
        GIANT_CHECK(b->name == ball.name && b->shape == Shape::sphere && !b->visible);
        GIANT_CHECK(b->transform.position == ball.transform.position);
        GIANT_CHECK_NEAR(b->transform.rotation.y, ball.transform.rotation.y, 1e-6);
        GIANT_CHECK(b->material.emissive == ball.material.emissive);
    }
    GIANT_CHECK(loaded.lighting.sun_elevation_degrees == 12.5f);
    GIANT_CHECK(loaded.lighting.sky_color == scene.lighting.sky_color);
    // Saving again gives the same text: diffs only show real changes.
    GIANT_CHECK(save_scene(loaded) == text);
}

GIANT_TEST("world: loading reports what is wrong and changes nothing") {
    Scene scene;
    scene.add(named("keep me"));
    const auto check_error = [&scene](const char* text, const char* expected) {
        const auto error = load_scene(text, scene);
        GIANT_CHECK(error.has_value());
        if (error.has_value() && !contains(error->message, expected)) {
            std::fprintf(stderr, "    got: %s\n    want: %s\n", error->message.c_str(), expected);
            GIANT_CHECK(false);
        }
        GIANT_CHECK(scene.size() == 1 && scene.entities()[0].name == "keep me");
    };
    check_error("{", "not valid JSON");
    check_error("[]", "must be an object");
    check_error(R"({"version": 1})", "format is missing");
    check_error(R"({"format": "other", "version": 1})", "format must be");
    check_error(R"({"format": "giant-scene", "version": 2})", "version must be 1");
    check_error(R"({"format": "giant-scene", "version": 1, "colour": 1})", "colour is not a known key");
    check_error(R"({"format": "giant-scene", "version": 1, "entities": [{"name": "x"}]})",
                "entities[0].id is missing");
    check_error(R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "shape": "cone"}]})",
                "entities[0].shape must be cube, sphere, plane, mesh, kit or decal");
    check_error(R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1}, {"id": 1}]})",
                "entities[1].id 1 is used twice");
    check_error(R"({"format": "giant-scene", "version": 1,
                    "entities": [{"id": 1, "position": [1, 2]}]})",
                "entities[0].position must be an array of 3 numbers");
    check_error(R"({"format": "giant-scene", "version": 1,
                    "entities": [{"id": 1, "material": {"roughnes": 1}}]})",
                "entities[0].material.roughnes is not a known key");
    check_error(R"({"format": "giant-scene", "version": 1, "lighting": {"exposure": "high"}})",
                "lighting.exposure must be a finite number");
    check_error(R"({"format": "giant-scene", "version": 1,
                    "entities": [{"id": 1, "rotation": [0, 0, 0, 0]}]})",
                "rotation must not be all zeros");
}

GIANT_TEST("world: many entities are removed and restored in one pass") {
    Scene scene;
    for (int i = 0; i < 20'000; ++i) {
        scene.add(named("tree"));
    }
    std::vector<EntityId> ids;
    for (EntityId id = 1; id <= 20'000; id += 2) {
        ids.push_back(id);
    }
    ids.push_back(3);      // listed twice
    ids.push_back(99'999); // not in the scene: skipped
    auto removed = scene.remove_many(ids);
    GIANT_CHECK(removed.size() == 10'000 && scene.size() == 10'000);
    GIANT_CHECK(scene.find(1) == nullptr && scene.find(2) != nullptr);
    GIANT_CHECK(scene.entities()[0].id == 2 && scene.find(20'000) == &scene.entities().back());
    scene.restore_many(std::move(removed));
    GIANT_CHECK(scene.size() == 20'000);
    bool in_order = true;
    for (usize i = 0; i < scene.size(); ++i) {
        in_order = in_order && scene.entities()[i].id == i + 1 && scene.find(i + 1) == &scene.entities()[i];
    }
    GIANT_CHECK(in_order);
}

GIANT_TEST("world: loading refuses values the editor could never make") {
    const auto refuses = [](const char* entity, const char* expected) {
        Scene scene;
        const std::string text = std::string(R"({"format": "giant-scene", "version": 1, "entities": [)") +
                                 entity + "]}";
        const auto error = load_scene(text, scene);
        GIANT_CHECK(error.has_value());
        if (error.has_value() && !contains(error->message, expected)) {
            std::fprintf(stderr, "    got: %s\n    want: %s\n", error->message.c_str(), expected);
            GIANT_CHECK(false);
        }
    };
    refuses(R"({"id": 1, "material": {"roughness": 1e300}})", "roughness must be from 0 to 1");
    refuses(R"({"id": 1, "scale": [1, -1, 1]})", "scale values must be from");
    refuses(R"({"id": 1, "scale": [1e300, 1, 1]})", "scale values must be from");
    refuses(R"({"id": 1, "material": {"base_color": [-0.1, 0, 0]}})", "base_color values");
    refuses(R"({"id": 1, "material": {"metallic": 2}})", "metallic must be from 0 to 1");
    refuses(R"({"id": 9007199254740992})", "id must be a whole number from 1 to 2^53 - 1");
    refuses(std::string(R"({"id": 1, "name": ")" + std::string(129, 'x') + R"("})").c_str(),
            "name must be at most 128 bytes");
    Scene scene;
    GIANT_CHECK(load_scene(R"({"format": "giant-scene", "version": 1, "next_id": 18446744073709551615})",
                           scene)
                    .has_value());
    GIANT_CHECK(!load_scene(R"({"format": "giant-scene", "version": 1,
                                "lighting": {"exposure": 0.5, "sun_elevation_degrees": -10}})",
                            scene)
                     .has_value());
}

GIANT_TEST("world: a minimal scene file loads with defaults") {
    Scene scene;
    const auto error = load_scene(
        R"({"format": "giant-scene", "version": 1, "next_id": 1, "entities": [{"id": 7}]})", scene);
    GIANT_CHECK(!error.has_value());
    GIANT_CHECK(scene.size() == 1 && scene.find(7) != nullptr);
    GIANT_CHECK(scene.find(7)->shape == Shape::cube && scene.find(7)->visible);
    // next_id below the largest id is corrected, so new ids stay unique.
    GIANT_CHECK(scene.add(named("new")) == 8);
}

GIANT_TEST("world: values at every limit survive a save and load") {
    Scene scene;
    Lighting& l = scene.lighting;
    l.sun_elevation_degrees = -90.0f;
    l.sun_azimuth_degrees = 360.0f;
    l.sun_intensity = 100.0f;
    l.ambient_intensity = 100.0f;
    l.exposure = 0.01f; // not exact in float: compared as stored
    l.sun_color = {1.0f, 0.0f, 1.0f};
    Entity tiny = named("tiny");
    tiny.transform.scale = {0.0001f, 1e6f, 0.0001f};
    tiny.transform.position = {-1e8, 1e8, 0.0};
    tiny.material.emissive = {1000.0f, 0.0f, 0.0f};
    tiny.material.roughness = 1.0f;
    scene.add(tiny);
    Entity last = named("last");
    last.id = k_max_entity_id;
    scene.add(last);

    const std::string text = save_scene(scene);
    Scene loaded;
    const auto error = load_scene(text, loaded);
    GIANT_CHECK(!error.has_value());
    if (error) {
        std::fprintf(stderr, "    %s\n", error->message.c_str());
        return;
    }
    GIANT_CHECK(loaded.find(k_max_entity_id) != nullptr);
    GIANT_CHECK(loaded.next_id() == k_max_entity_id + 1);
    GIANT_CHECK(loaded.lighting.exposure == 0.01f);
    GIANT_CHECK(loaded.find(1)->transform.scale.x == 0.0001f);
    GIANT_CHECK(save_scene(loaded) == text);
    // A full id space loads, but adding another entity reports it.
    GIANT_CHECK(loaded.add(named("one more")) == 0);
}

GIANT_TEST("world: imported meshes are saved by path") {
    Scene scene;
    Entity rock = named("Rock");
    rock.shape = Shape::mesh;
    rock.mesh = "assets/rocks/boulder é.glb";
    scene.add(rock);
    Entity was = named("Was a mesh", Shape::cube);
    was.mesh = "assets/tree.glb"; // kept so the shape can change back
    scene.add(was);
    const std::string text = save_scene(scene);
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->shape == Shape::mesh && loaded.find(1)->mesh == rock.mesh);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->shape == Shape::cube && loaded.find(2)->mesh == was.mesh);
    GIANT_CHECK(save_scene(loaded) == text);
    // A mesh entity needs its file.
    const std::string bare = R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "shape": "mesh"}]})";
    Scene bad;
    const auto error = load_scene(bare, bad);
    GIANT_CHECK(error.has_value() && contains(error->message, "mesh"));
    const std::string too_long = R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "shape": "mesh", "mesh": ")" +
                                 std::string(k_max_mesh_path_bytes + 1, 'a') + R"("}]})";
    GIANT_CHECK(load_scene(too_long, bad).has_value());
}

GIANT_TEST("world: kits and decals save and load") {
    Scene scene;
    Kit kit;
    kit.name = "lamp";
    KitPiece pole;
    pole.name = "pole";
    pole.transform.scale = {0.1f, 3.0f, 0.1f};
    KitPiece mark;
    mark.shape = Shape::decal;
    mark.decal = {DecalShape::cracks, 0.5f, 0.25f, 77};
    kit.pieces = {pole, mark};
    scene.kits.push_back(kit);
    Entity placed = named("lamp 1", Shape::kit);
    placed.kit = "lamp";
    scene.add(placed);
    Entity stamp = named("scorch", Shape::decal);
    stamp.decal.shape = DecalShape::splat;
    stamp.decal.seed = 4294967295u;
    scene.add(stamp);
    const std::string text = save_scene(scene);
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(save_scene(loaded) == text);
    GIANT_CHECK(loaded.find_kit("lamp") != nullptr && loaded.find_kit("lamp")->pieces[1].decal.seed == 77);
    GIANT_CHECK(loaded.entities()[1].decal.shape == DecalShape::splat);
    GIANT_CHECK(loaded.entities()[1].decal.seed == 4294967295u);

    // A placement of a kit the file does not have, and nested kits, are refused.
    std::string missing = text;
    missing.replace(missing.find("\"kit\": \"lamp\""), 13, "\"kit\": \"lump\"");
    const auto error = load_scene(missing, loaded);
    GIANT_CHECK(error.has_value() && contains(error->message, "not one of the scene's kits"));
    std::string nested = text;
    nested.replace(nested.find("\"shape\": \"cube\""), 15, "\"shape\": \"kit\"");
    const auto nest_error = load_scene(nested, loaded);
    GIANT_CHECK(nest_error.has_value() && contains(nest_error->message, "kits do not nest"));
}

GIANT_TEST("world: decal settings are kept when the shape changes, and empty kits are refused") {
    Scene scene;
    Entity e = named("was a decal");
    e.decal = {DecalShape::ring, 1.0f, 0.1f, 7};
    scene.add(e);
    const std::string text = save_scene(scene);
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.entities()[0].decal.shape == DecalShape::ring && loaded.entities()[0].decal.seed == 7);
    const char* empty_kit = R"({"format": "giant-scene", "version": 1, "kits": [{"name": "k", "pieces": []}]})";
    GIANT_CHECK(load_scene(empty_kit, loaded).has_value());
    const char* empty_name = R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "kit": ""}]})";
    GIANT_CHECK(load_scene(empty_name, loaded).has_value());
}

GIANT_TEST("world: cells and what changed") {
    using world::cell_of;
    GIANT_CHECK((cell_of({0.0, 5.0, 63.9}) == world::CellCoord{0, 0}));
    GIANT_CHECK((cell_of({-0.1, 0.0, 64.0}) == world::CellCoord{-1, 1}));
    GIANT_CHECK((cell_of({1e300, 0.0, -1e300}) ==
                 world::CellCoord{std::numeric_limits<i32>::max(), std::numeric_limits<i32>::min()}));
    GIANT_CHECK(cell_of({std::nan(""), 0.0, 0.0}).x == std::numeric_limits<i32>::min());

    world::Scene scene;
    GIANT_CHECK(scene.take_changes().all); // a new scene: everything
    const world::EntityId a = scene.add({});
    const world::EntityId b = scene.add({});
    world::SceneChanges c = scene.take_changes();
    GIANT_CHECK(!c.all && (c.ids == std::vector<world::EntityId>{a, b}));
    GIANT_CHECK(scene.take_changes().ids.empty());
    const world::Scene& read = scene;
    GIANT_CHECK(read.find(a) != nullptr && scene.take_changes().ids.empty()); // looking changes nothing
    scene.modify(b)->name = "b";
    scene.remove(a);
    c = scene.take_changes();
    GIANT_CHECK((c.ids == std::vector<world::EntityId>{b, a}));
    std::vector<world::Scene::Removed> gone = scene.remove_many(std::vector<world::EntityId>{b});
    scene.restore_many(std::move(gone));
    GIANT_CHECK((scene.take_changes().ids == std::vector<world::EntityId>{b, b}));
    scene.clear();
    GIANT_CHECK(scene.take_changes().all);
    // Copies are new scenes.
    world::Scene copy = scene;
    GIANT_CHECK(copy.identity() != scene.identity() && copy.take_changes().all);
    // Nobody taking them: the list stops growing.
    for (int i = 0; i < 6000; ++i) {
        scene.modify(scene.add({}));
    }
    c = scene.take_changes();
    GIANT_CHECK(c.all && c.ids.empty());
}

GIANT_TEST("world: a world's index and cells save and load as text") {
    WorldIndex world;
    world.next_id = 42;
    world.lighting.exposure = 2.0f;
    world.kits.push_back({"house", {KitPiece{"wall", Shape::cube, {}, {}, {}, {}}}});
    world.meshes = {"b.glb", "a.glb", "b.glb"};
    world.cells = {{3, 1}, {-2, 0}, {0, 1}};
    WorldIndex back;
    const std::string text = save_world_index(world);
    GIANT_CHECK(!load_world_index(text, back).has_value());
    GIANT_CHECK(back.next_id == 42 && back.lighting.exposure == 2.0f && back.kits.size() == 1);
    GIANT_CHECK(back.meshes.size() == 2 && back.meshes[0] == "a.glb");
    GIANT_CHECK(back.cells.size() == 3 && back.cells[0] == (CellCoord{-2, 0}) && back.cells[2] == (CellCoord{3, 1}));
    GIANT_CHECK(save_world_index(back) == text);
    GIANT_CHECK(cell_file_name({-3, 12}) == "-3_12.cell.json");

    Entity a = named("a");
    a.id = 9;
    a.transform.position = {70.0, 5.0, -1.0}; // cell (1, -1)
    Entity b = named("b", Shape::sphere);
    b.id = 4;
    b.transform.position = {127.5, 0.0, -63.0};
    const std::vector<Entity> both{a, b};
    const std::string cell = save_cell({1, -1}, both);
    GIANT_CHECK(cell.find("\"id\": 4") < cell.find("\"id\": 9")); // sorted by id
    std::vector<Entity> loaded;
    GIANT_CHECK(!load_cell(cell, {1, -1}, loaded).has_value());
    GIANT_CHECK(loaded.size() == 2 && loaded[0].id == 4 && loaded[1].name == "a");
    GIANT_CHECK(save_cell({1, -1}, loaded) == cell);
}

GIANT_TEST("world: a world's terrain settings and a cell's tile save and load as text") {
    WorldIndex world;
    world.cells = {{0, 0}, {5, -2}, {1, 0}};
    GIANT_CHECK(save_world_index(world).find("terrain") == std::string::npos); // none, default: not written
    world.terrain_samples = 16;
    world.terrain_material.roughness = 0.25f;
    world.terrain_cells = {{5, -2}, {0, 0}};
    WorldIndex back;
    const std::string text = save_world_index(world);
    GIANT_CHECK(!load_world_index(text, back).has_value());
    GIANT_CHECK(back.terrain_samples == 16 && back.terrain_material.roughness == 0.25f);
    GIANT_CHECK(back.terrain_cells.size() == 2 && back.terrain_cells[0] == (CellCoord{5, -2}));
    GIANT_CHECK(save_world_index(back) == text);
    GIANT_CHECK(contains(load_world_index(R"({"format": "giant-world", "version": 1, "cells": [[0, 0]],
        "terrain": {"samples": 16, "cells": [[1, 0]]}})", back)->message,
                         "terrain.cells[0] is not in the world's cells"));
    GIANT_CHECK(contains(load_world_index(R"({"format": "giant-world", "version": 1, "cells": [],
        "terrain": {"samples": 12}})", back)->message,
                         "terrain.samples must be 8, 16, 32, 64 or 128"));

    Terrain::Heights tile(64);
    for (usize k = 0; k < tile.size(); ++k) tile[k] = 0.1f * static_cast<float>(k) - 3.0f;
    Entity a = named("a");
    a.id = 2;
    const std::string cell = save_cell({0, 0}, std::vector<Entity>{a}, &tile);
    std::vector<Entity> entities;
    std::optional<Terrain::Heights> heights;
    GIANT_CHECK(!load_cell(cell, {0, 0}, 8, entities, heights).has_value());
    GIANT_CHECK(entities.size() == 1 && heights && *heights == tile); // exact
    GIANT_CHECK(save_cell({0, 0}, entities, &*heights) == cell);
    // Another spacing, and the reader with no terrain, refuse it.
    std::optional<Terrain::Heights> kept = Terrain::Heights{1.0f};
    const auto wrong = load_cell(cell, {0, 0}, 16, entities, kept);
    GIANT_CHECK(wrong && contains(wrong->message, "heights must be 16 rows"));
    GIANT_CHECK(kept && kept->size() == 1); // unchanged on failure
    GIANT_CHECK(contains(load_cell(cell, {0, 0}, entities)->message, "read without terrain"));
    // A file without heights has no tile.
    GIANT_CHECK(!load_cell(save_cell({0, 0}, std::vector<Entity>{a}), {0, 0}, 8, entities, heights).has_value());
    GIANT_CHECK(!heights);
}

GIANT_TEST("world: bad world and cell files say what is wrong") {
    WorldIndex world;
    GIANT_CHECK(contains(load_world_index(R"({"format": "giant-world", "version": 1})", world)->message,
                         "cells is missing"));
    GIANT_CHECK(contains(load_world_index(R"({"format": "giant-world", "version": 1, "cells": [[0, 0], [0, 0]]})",
                                          world)->message,
                         "cells[1] is listed twice"));
    GIANT_CHECK(contains(load_world_index(R"({"format": "giant-scene", "version": 1, "cells": []})", world)->message,
                         "format must be \"giant-world\""));
    Entity far = named("far");
    far.id = 1;
    far.transform.position = {200.0, 0.0, 0.0}; // cell (3, 0)
    const std::string text = save_cell({3, 0}, std::vector<Entity>{far});
    std::vector<Entity> out{named("kept")};
    const auto wrong = load_cell(text, {2, 0}, out);
    GIANT_CHECK(wrong && contains(wrong->message, "cell must be [2, 0]"));
    GIANT_CHECK(out.size() == 1 && out[0].name == "kept");
    const std::string moved = R"({"format": "giant-cell", "version": 1, "cell": [2, 0], "entities": [
        {"id": 1, "position": [200, 0, 0]}]})";
    GIANT_CHECK(contains(load_cell(moved, {2, 0}, out)->message, "entities[0].position is not in cell [2, 0]"));
    const std::string twice = R"({"format": "giant-cell", "version": 1, "cell": [0, 0], "entities": [
        {"id": 1}, {"id": 1}]})";
    GIANT_CHECK(contains(load_cell(twice, {0, 0}, out)->message, "entities[1].id 1 is used twice"));
}

GIANT_TEST("world index: the far ground a save still owes reads back") {
    WorldIndex index;
    index.cells = {{0, 0}};
    index.far_owed = {{3, -2}, {0, 0}};
    index.far_owed_2 = {{-1, 4}};
    WorldIndex back;
    GIANT_CHECK(!load_world_index(save_world_index(index), back).has_value());
    GIANT_CHECK(back.far_owed.size() == 2 && back.far_owed_2.size() == 1 && (back.far_owed_2[0] == CellCoord{-1, 4}));
    std::string text = save_world_index(index);
    text.replace(text.find("\"far_owed_2\""), 12, "\"far_owed_2\": 3, \"x\"");
    GIANT_CHECK(load_world_index(text, back).has_value());
}

namespace {
CellDamage some_damage() {
    CellDamage d;
    KeptKit k;
    k.entity = 7;
    k.kit = "house";
    k.position = {70.5, 2.0, 10.25};
    k.rotation = normalize(Quat{0.0f, 0.38268343f, 0.0f, 0.92387953f});
    k.scale = {1.5f, 1.0f, 1.0f};
    k.pieces.push_back({3, {72.123456789, 0.1, 11.0}, normalize(Quat{0.1f, 0.2f, 0.3f, 0.9f}), {4.0f, 0.2f, 3.0f}});
    k.pieces.push_back({1, {69.0, 0.25, 9.0}, Quat{}, {0.2f, 3.0f, 3.6f}});
    d.kits.push_back(k);
    KeptTree t;
    t.x = 100.125;
    t.z = 33.3;
    t.height = 12.5f;
    t.trunk_radius = 0.3f;
    t.position = {100.125, 4.2, 33.3};
    t.rotation = normalize(Quat{0.0f, 0.0f, 0.70710677f, 0.70710677f});
    d.trees.push_back(t);
    KeptTree u = t;
    u.x = 90.0;
    d.trees.push_back(u);
    return d;
}
} // namespace

GIANT_TEST("world: kept damage saves and loads exactly in cells and scenes, sorted, and marks its placements changed") {
    const CellCoord c{1, 0};
    CellDamage d = some_damage();
    Entity house;
    house.id = 7;
    house.shape = Shape::kit;
    house.kit = "house";
    house.transform.position = {70.5, 2.0, 10.25};
    const std::string text = save_cell(c, std::vector<Entity>{house}, nullptr, &d);
    GIANT_CHECK(contains(text, "\"damage\""));
    std::vector<Entity> entities;
    std::optional<Terrain::Heights> heights;
    CellDamage loaded;
    GIANT_CHECK(!load_cell(text, c, 0, entities, heights, &loaded).has_value());
    sort_damage(d);
    GIANT_CHECK(loaded == d); // every number exact
    GIANT_CHECK(loaded.kits[0].pieces[0].piece == 1 && loaded.trees[0].x == 90.0);
    GIANT_CHECK(save_cell(c, entities, nullptr, &loaded) == text); // the same text again
    GIANT_CHECK(!contains(save_cell(c, entities, nullptr, nullptr), "damage"));
    const CellDamage none;
    GIANT_CHECK(!contains(save_cell(c, entities, nullptr, &none), "damage"));
    // A scene: set_damage keeps it sorted, finds kit records, marks them changed.
    Scene scene;
    Kit kit;
    kit.name = "house";
    kit.pieces.resize(4);
    scene.kits.push_back(kit);
    scene.add(house);
    (void)scene.take_changes();
    const u64 edits = scene.damage_edits();
    scene.set_damage(c, some_damage());
    GIANT_CHECK(scene.damage_edits() == edits + 1 && scene.damage_in(c) != nullptr && *scene.damage_in(c) == d);
    GIANT_CHECK(scene.kept_kit(7) != nullptr && scene.kept_kit(7)->pieces.size() == 2 && scene.kept_kit(8) == nullptr);
    const SceneChanges changed = scene.take_changes();
    GIANT_CHECK(!changed.all && changed.ids.size() == 1 && changed.ids[0] == 7);
    scene.set_damage(c, some_damage()); // the same: nothing
    GIANT_CHECK(scene.damage_edits() == edits + 1 && scene.take_changes().ids.empty());
    Scene loaded_scene;
    GIANT_CHECK(!load_scene(save_scene(scene), loaded_scene).has_value());
    GIANT_CHECK(loaded_scene.damage().size() == 1 && *loaded_scene.damage_in(c) == d && loaded_scene.kept_kit(7) != nullptr);
    const Scene copy = scene;
    GIANT_CHECK(copy.damage() == scene.damage() && copy.kept_kit(7) != nullptr);
    scene.set_damage(c, {});
    GIANT_CHECK(scene.damage().empty() && scene.kept_kit(7) == nullptr && scene.take_changes().ids.size() == 1);
    GIANT_CHECK(!contains(save_scene(scene), "damage"));
}

GIANT_TEST("world: bad kept damage says what is wrong") {
    const CellCoord c{1, 0};
    const auto error = [&](CellDamage d) {
        std::vector<Entity> entities;
        std::optional<Terrain::Heights> heights;
        CellDamage out;
        const auto bad = load_cell(save_cell(c, entities, nullptr, &d), c, 0, entities, heights, &out);
        return bad ? bad->message : std::string();
    };
    CellDamage d = some_damage();
    d.kits[0].position.x = 10.0; // the placement in another cell
    GIANT_CHECK(contains(error(d), "damage.kits[0].position is not in cell [1, 0]"));
    d = some_damage();
    d.trees[0].x = 200.0;
    GIANT_CHECK(contains(error(d), "is not in cell [1, 0]"));
    d = some_damage();
    d.kits[0].pieces[1].piece = 3; // twice
    GIANT_CHECK(contains(error(d), "piece is listed twice"));
    d = some_damage();
    d.kits.push_back(d.kits[0]);
    GIANT_CHECK(contains(error(d), "entity is listed twice"));
    d = some_damage();
    d.kits[0].pieces.clear();
    GIANT_CHECK(contains(error(d), "pieces must be an array of 1"));
    d = some_damage();
    d.trees[0].height = 0.0f;
    GIANT_CHECK(contains(error(d), "height must be from"));
    GIANT_CHECK(error(some_damage()).empty());
    // A scene's damage in the wrong cell is refused.
    Scene scene;
    scene.set_damage({5, 0}, some_damage());
    Scene out;
    const auto bad = load_scene(save_scene(scene), out);
    GIANT_CHECK(bad && contains(bad->message, "damage[0].kits[0].position is not in cell [5, 0]"));
}
