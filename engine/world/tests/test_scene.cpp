#include "giant/world/scene.h"
#include "giant/world/weather.h"

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
                "entities[0].shape must be cube, sphere, plane, mesh, kit, decal or cloth");
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

GIANT_TEST("world: a material's surface saves and loads by name, and only when set") {
    for (usize i = 0; i < k_surface_count; ++i) {
        const Surface s = static_cast<Surface>(i);
        GIANT_CHECK(surface_from_string(to_string(s)) == s);
    }
    GIANT_CHECK(!surface_from_string("steel").has_value());
    Scene scene;
    Entity crate = named("Crate");
    crate.material.surface = Surface::wood;
    scene.add(crate);
    scene.add(named("Plain"));
    Kit kit;
    kit.name = "wall";
    KitPiece pane;
    pane.material.surface = Surface::glass;
    kit.pieces.push_back(pane);
    scene.kits.push_back(kit);
    ScatterLayer layer;
    layer.name = "rocks";
    ScatterItem rock;
    rock.material.surface = Surface::concrete;
    layer.items.push_back(rock);
    scene.scatter.push_back(layer);
    const std::string text = save_scene(scene);
    GIANT_CHECK(contains(text, "\"wood\"") && contains(text, "\"glass\"") && contains(text, "\"concrete\""));
    GIANT_CHECK(!contains(text, "\"standard\"")); // the plain one writes none
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->material.surface == Surface::wood);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->material.surface == Surface::standard);
    GIANT_CHECK(loaded.find_kit("wall") && loaded.find_kit("wall")->pieces[0].material.surface == Surface::glass);
    GIANT_CHECK(loaded.scatter.size() == 1 && loaded.scatter[0].items[0].material.surface == Surface::concrete);
    GIANT_CHECK(save_scene(loaded) == text);
    Scene bad;
    const auto error = load_scene(R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "material": {"surface": "steel"}}]})", bad);
    GIANT_CHECK(error.has_value() && contains(error->message, "surface must be standard, flesh"));
}

GIANT_TEST("world: a material's wind saves and loads, and only when set") {
    Scene scene;
    Entity tree = named("Tree");
    tree.material.sway = 1.0f;
    tree.material.flutter = 2.5f;
    scene.add(tree);
    scene.add(named("Rock"));
    ScatterLayer layer;
    layer.name = "grass";
    ScatterItem blade;
    blade.material.sway = 3.0f;
    layer.items.push_back(blade);
    scene.scatter.push_back(layer);
    const std::string text = save_scene(scene);
    GIANT_CHECK(text.find("\"sway\"") != text.rfind("\"sway\"")); // the tree's and the grass's
    GIANT_CHECK(text.find("\"flutter\"") == text.rfind("\"flutter\"")); // the tree's alone
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->material.sway == 1.0f && loaded.find(1)->material.flutter == 2.5f);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->material.sway == 0.0f);
    GIANT_CHECK(loaded.scatter.size() == 1 && loaded.scatter[0].items[0].material.sway == 3.0f);
    GIANT_CHECK(save_scene(loaded) == text);
    Scene bad;
    const auto error = load_scene(R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "material": {"sway": 9}}]})", bad);
    GIANT_CHECK(error.has_value() && contains(error->message, "sway must be from 0 to 4"));
}

GIANT_TEST("world: a material's image files save and load, and only when set") {
    Scene scene;
    Entity crate = named("Crate");
    crate.shape = Shape::mesh;
    crate.mesh = "assets/crate.glb";
    crate.material.maps.base_color = "assets/crate_albedo.png";
    crate.material.maps.normal = "assets/crate_normal.png";
    crate.material.maps.roughness_metal = "assets/crate_orm.png";
    crate.material.maps.occlusion = "assets/crate_orm.png";
    crate.material.maps.normal_green_down = true;
    scene.add(crate);
    scene.add(named("Rock"));
    Kit kit;
    kit.name = "shed";
    KitPiece wall;
    wall.shape = Shape::mesh;
    wall.mesh = "assets/wall.glb";
    wall.material.maps.base_color = "assets/planks.png";
    kit.pieces.push_back(wall);
    scene.kits.push_back(kit);
    ScatterLayer layer;
    layer.name = "ferns";
    ScatterItem fern;
    fern.shape = Shape::mesh;
    fern.mesh = "assets/fern.glb";
    fern.material.maps.normal = "assets/fern_n.png";
    layer.items.push_back(fern);
    scene.scatter.push_back(layer);
    const std::string text = save_scene(scene);
    GIANT_CHECK(text.find("\"maps\"") != std::string::npos);
    GIANT_CHECK(text.find("crate_albedo") != std::string::npos);
    Scene loaded;
    const auto loaded_error = load_scene(text, loaded);
    GIANT_CHECK(!loaded_error.has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->material.maps == crate.material.maps);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->material.maps == MaterialMaps{});
    GIANT_CHECK(loaded.find_kit("shed") && loaded.find_kit("shed")->pieces[0].material.maps == wall.material.maps);
    GIANT_CHECK(loaded.scatter.size() == 1 && loaded.scatter[0].items[0].material.maps == fern.material.maps);
    GIANT_CHECK(save_scene(loaded) == text);
    // The rock has none: no "maps" written for it (three: crate, wall, fern).
    usize count = 0;
    for (usize at = text.find("\"maps\""); at != std::string::npos; at = text.find("\"maps\"", at + 1)) ++count;
    GIANT_CHECK(count == 3);
    for (const char* bad_scene :
         {R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "material": {"maps": {"albedo": "a.png"}}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "material": {"maps": {"normal": 3}}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "material": {"maps": "a.png"}}]})"}) {
        Scene bad;
        const auto error = load_scene(bad_scene, bad);
        GIANT_CHECK(error.has_value() && contains(error->message, "maps"));
    }
}

GIANT_TEST("world: cloth saves and loads, only when set, and hangs from entities alone") {
    Scene scene;
    Entity flag = named("Flag");
    flag.shape = Shape::cloth;
    flag.cloth.width = 2.0f;
    flag.cloth.height = 1.25f;
    flag.cloth.pin = ClothPin::left;
    flag.cloth.weight = 0.1f;
    flag.cloth.stiffness = 0.2f;
    scene.add(flag);
    Entity plain = named("Banner");
    plain.shape = Shape::cloth; // the default sheet: still written (it is cloth)
    scene.add(plain);
    scene.add(named("Rock"));
    const std::string text = save_scene(scene);
    GIANT_CHECK(text.find("\"cloth\": {") != text.rfind("\"cloth\": {")); // the flag's and the banner's
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->shape == Shape::cloth && loaded.find(1)->cloth == flag.cloth);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->cloth == ClothSettings{});
    GIANT_CHECK(loaded.find(3) && loaded.find(3)->cloth == ClothSettings{});
    GIANT_CHECK(save_scene(loaded) == text);
    for (const char* bad_scene :
         {R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "shape": "cloth", "cloth": {"pin": "bottom"}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "shape": "cloth", "cloth": {"width": 50}}]})",
          R"({"format": "giant-scene", "version": 1, "kits": [{"name": "k", "pieces": [{"shape": "cloth"}]}]})",
          R"({"format": "giant-scene", "version": 1, "scatter": [{"name": "s", "items": [{"shape": "cloth"}]}]})"}) {
        Scene bad;
        GIANT_CHECK(load_scene(bad_scene, bad).has_value());
    }
}

GIANT_TEST("world: swing settings save and load, only when set, within their ranges") {
    Scene scene;
    Entity sign = named("Sign");
    sign.swing.on = true;
    sign.swing.length = 0.3f;
    sign.swing.hinge = SwingHinge::x;
    sign.swing.weight = 6.0f;
    sign.swing.limit = 45.0f;
    scene.add(sign);
    Entity off = named("Off");
    off.swing.length = 2.0f; // kept while off
    scene.add(off);
    scene.add(named("Rock"));
    const std::string text = save_scene(scene);
    GIANT_CHECK(text.find("\"swing\": {") != text.rfind("\"swing\": {")); // the sign's and the off one's, not the rock's
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->swing == sign.swing);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->swing == off.swing && !loaded.find(2)->swing.on);
    GIANT_CHECK(loaded.find(3) && loaded.find(3)->swing == SwingSettings{});
    GIANT_CHECK(save_scene(loaded) == text);
    GIANT_CHECK(swing_hinge_from_string("x") == SwingHinge::x && !swing_hinge_from_string("y"));
    for (const char* bad_scene :
         {R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "swing": {"hinge": "y"}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "swing": {"length": 0}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "swing": {"limit": 180}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "swing": {"on": 1}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "swing": {"spin": true}}]})"}) {
        Scene bad;
        GIANT_CHECK(load_scene(bad_scene, bad).has_value());
    }
}

GIANT_TEST("world: lamps on entities and kit pieces save and load, only when set, within their ranges") {
    Scene scene;
    Entity lamp = named("Street lamp");
    lamp.lamp.on = true;
    lamp.lamp.kind = LampKind::spot;
    lamp.lamp.color = {0.9f, 0.7f, 0.4f};
    lamp.lamp.intensity = 40.0f;
    lamp.lamp.range = 18.0f;
    lamp.lamp.offset = {0.0f, 4.5f, 0.6f};
    lamp.lamp.inner_degrees = 20.0f;
    lamp.lamp.outer_degrees = 50.0f;
    lamp.lamp.source_radius = 0.1f;
    lamp.lamp.shadows = true;
    lamp.lamp.shadow_near = 0.35f;
    scene.add(lamp);
    Entity off = named("Off");
    off.lamp.range = 3.0f; // kept while off
    scene.add(off);
    scene.add(named("Rock"));
    Kit kit;
    kit.name = "Lamp post";
    KitPiece post;
    post.name = "post";
    KitPiece bulb;
    bulb.name = "bulb";
    bulb.shape = Shape::sphere;
    bulb.lamp.on = true;
    kit.pieces = {post, bulb};
    scene.kits.push_back(kit);
    const std::string text = save_scene(scene);
    usize lamps = 0;
    for (usize at = text.find("\"lamp\": {"); at != std::string::npos; at = text.find("\"lamp\": {", at + 1)) ++lamps;
    GIANT_CHECK(lamps == 3); // the lamp's, the off one's and the bulb's; not the rock's or the post's
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->lamp == lamp.lamp);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->lamp == off.lamp && !loaded.find(2)->lamp.on);
    GIANT_CHECK(loaded.find(3) && loaded.find(3)->lamp == Lamp{});
    GIANT_CHECK(loaded.kits.size() == 1 && loaded.kits[0] == kit);
    GIANT_CHECK(save_scene(loaded) == text);
    GIANT_CHECK(lamp_kind_from_string("spot") == LampKind::spot && !lamp_kind_from_string("area"));
    for (const char* bad_scene :
         {R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "lamp": {"kind": "area"}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "lamp": {"range": 0}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "lamp": {"color": [2, 1, 1]}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "lamp": {"outer_degrees": 90}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "lamp": {"intensity": -1}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "lamp": {"glow": true}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "lamp": {"shadow_near": 0}}]})",
          R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "lamp": {"shadows": 1}}]})",
          R"({"format": "giant-scene", "version": 1, "kits": [{"name": "k", "pieces": [{"lamp": {"on": 1}}]}]})"}) {
        Scene bad;
        GIANT_CHECK(load_scene(bad_scene, bad).has_value());
    }
}

GIANT_TEST("world: a mesh entity's animation saves and loads, and only when set") {
    Scene scene;
    Entity walker = named("Walker");
    walker.shape = Shape::mesh;
    walker.mesh = "assets/giantess.glb";
    walker.animation = {"walk", -2.5f, false};
    scene.add(walker);
    Entity still = named("Still");
    still.shape = Shape::mesh;
    still.mesh = "assets/giantess.glb";
    scene.add(still);
    const std::string text = save_scene(scene);
    GIANT_CHECK(contains(text, "\"walk\"") && text.find("animation") == text.rfind("animation")); // the still one writes none
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->animation == walker.animation);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->animation == EntityAnimation{});
    GIANT_CHECK(save_scene(loaded) == text);
    const auto refused = [](const std::string& animation, const char* expected) {
        const std::string json = R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "animation": )" +
                                 animation + "}]}";
        Scene bad;
        const auto error = load_scene(json, bad);
        GIANT_CHECK(error.has_value() && contains(error->message, expected));
    };
    refused(R"({"clip": "a", "speed": 101})", "speed must be from -100 to 100");
    refused(R"({"clip": "a", "pace": 1})", "pace is not a known key");
    refused(R"({"clip": 3})", "clip must be a string");
    refused(R"("walk")", "must be an object");
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
    world.kits.push_back({"house", {KitPiece{"wall", Shape::cube, {}, {}, {}, {}, {}}}});
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
    // Two cells naming one placement (a stale record kept before it moved):
    // asked where it is placed, the record made there.
    CellDamage stale = some_damage();
    stale.trees.clear();
    stale.kits[0].position = {10.0, 0.0, 10.0};
    scene.set_damage({0, 0}, stale);
    Transform placed;
    placed.position = d.kits[0].position;
    placed.rotation = d.kits[0].rotation;
    placed.scale = d.kits[0].scale;
    GIANT_CHECK(scene.kept_kit(7, &placed) != nullptr && scene.kept_kit(7, &placed)->position == d.kits[0].position);
    placed.position = {10.0, 0.0, 10.0};
    GIANT_CHECK(scene.kept_kit(7, &placed) != nullptr && scene.kept_kit(7, &placed)->position.x == 10.0);
    placed.position.x = 11.0;
    GIANT_CHECK(scene.kept_kit(7, &placed) == nullptr);
    scene.set_damage({0, 0}, {});
    placed.position = d.kits[0].position;
    GIANT_CHECK(scene.kept_kit(7, &placed) != nullptr); // the other still found
    // An empty one held says "none here"; it is never saved.
    scene.set_damage({3, 3}, {}, true);
    GIANT_CHECK(scene.damage_in({3, 3}) != nullptr && scene.damage_in({3, 3})->empty());
    scene.set_damage({3, 3}, {});
    GIANT_CHECK(scene.damage_in({3, 3}) == nullptr);
    (void)scene.take_changes();
    scene.set_damage(c, {});
    GIANT_CHECK(scene.damage().empty() && scene.kept_kit(7) == nullptr && scene.take_changes().ids.size() == 1);
    GIANT_CHECK(!contains(save_scene(scene), "damage"));
}

GIANT_TEST("world: the game's damage shows over the kept, is never saved, and its text round trips") {
    const CellCoord c{1, 0};
    Scene scene;
    Entity house;
    house.id = 7;
    house.shape = Shape::kit;
    house.kit = "house";
    house.transform.position = {70.5, 2.0, 10.25};
    Kit kit;
    kit.name = "house";
    kit.pieces.resize(4);
    scene.kits.push_back(kit);
    scene.add(house);
    scene.set_damage(c, some_damage());
    (void)scene.take_changes();
    // The game's: one piece down more than kept, the trees standing again (as the game has them).
    CellDamage game = some_damage();
    game.kits[0].pieces.push_back({2, {71.0, 0.2, 10.0}, Quat{}, {1.0f, 1.0f, 1.0f}});
    game.trees.clear();
    const u64 edits = scene.damage_edits();
    scene.set_game_damage(c, game);
    sort_damage(game);
    GIANT_CHECK(scene.damage_edits() == edits + 1 && scene.take_changes().ids == std::vector<EntityId>{7});
    GIANT_CHECK(scene.shown_damage_in(c) != nullptr && *scene.shown_damage_in(c) == game);
    GIANT_CHECK(scene.kept_kit(7) != nullptr && scene.kept_kit(7)->pieces.size() == 3);
    CellDamage kept = some_damage();
    sort_damage(kept);
    GIANT_CHECK(*scene.damage_in(c) == kept); // the kept stays as it was
    GIANT_CHECK(scene.shown_damage_in({4, 4}) == nullptr);
    // Never in the scene's (or a cell's) file; copied and moved with the scene.
    Scene loaded;
    GIANT_CHECK(!load_scene(save_scene(scene), loaded).has_value());
    GIANT_CHECK(loaded.game_damage().empty() && loaded.kept_kit(7)->pieces.size() == 2);
    Scene copy = scene;
    GIANT_CHECK(copy.game_damage() == scene.game_damage() && copy.kept_kit(7)->pieces.size() == 3);
    const Scene moved = std::move(copy);
    GIANT_CHECK(moved.kept_kit(7)->pieces.size() == 3);
    // The game's emptied: the kept shows again. A game record in another cell is found first.
    (void)scene.take_changes();
    scene.set_game_damage(c, {});
    GIANT_CHECK(scene.game_damage().empty() && scene.kept_kit(7)->pieces.size() == 2);
    GIANT_CHECK(scene.take_changes().ids == std::vector<EntityId>{7});
    // Held with only trees: the kept house record is hidden, and its placement marked changed.
    CellDamage only_trees = some_damage();
    only_trees.kits.clear();
    scene.set_game_damage(c, only_trees);
    GIANT_CHECK(scene.kept_kit(7) == nullptr && scene.take_changes().ids == std::vector<EntityId>{7});
    scene.set_game_damage(c, {});
    GIANT_CHECK(scene.kept_kit(7) != nullptr && scene.take_changes().ids == std::vector<EntityId>{7});
    CellDamage elsewhere = game;
    elsewhere.trees.clear();
    elsewhere.kits[0].position = {10.0, 0.0, 10.0};
    scene.set_game_damage({0, 0}, elsewhere);
    GIANT_CHECK(scene.kept_kit(7)->position.x == 10.0);
    Transform placed;
    placed.position = kept.kits[0].position;
    placed.rotation = kept.kits[0].rotation;
    placed.scale = kept.kits[0].scale;
    GIANT_CHECK(scene.kept_kit(7, &placed) != nullptr && scene.kept_kit(7, &placed)->pieces.size() == 2);
    (void)scene.take_changes();
    scene.clear_game_damage();
    GIANT_CHECK(scene.game_damage().empty() && scene.take_changes().ids.size() == 1);
    scene.clear_game_damage(); // nothing
    GIANT_CHECK(scene.take_changes().ids.empty());

    // A cell's damage as text on its own, and the weather.
    CellDamage back;
    GIANT_CHECK(!load_cell_damage(save_cell_damage(some_damage()), c, back).has_value() && back == kept);
    GIANT_CHECK(load_cell_damage(save_cell_damage(some_damage()), {2, 0}, back).has_value() && back == kept);
    GIANT_CHECK(load_cell_damage("{\"kits\": [], \"oops\": 1}", c, back).has_value());
    // With a game's ground: its heights exactly, refused where not asked for or not whole.
    Terrain::Heights ground(64);
    for (usize i = 0; i < ground.size(); ++i) ground[i] = 0.1f * static_cast<float>(i) - 3.3f;
    const std::string with = save_cell_damage(some_damage(), &ground);
    std::optional<Terrain::Heights> heights;
    GIANT_CHECK(!load_cell_damage(with, c, back, &heights).has_value() && back == kept && heights == ground);
    GIANT_CHECK(load_cell_damage(with, c, back).has_value());
    GIANT_CHECK(!load_cell_damage(save_cell_damage(some_damage()), c, back, &heights).has_value() && !heights);
    GIANT_CHECK(load_cell_damage("{\"heights\": [\"1 2\"]}", c, back, &heights).has_value());
    GIANT_CHECK(load_cell_damage("{\"heights\": 3}", c, back, &heights).has_value());
    const auto rows = [](usize n, const std::string& row) {
        std::string t = "{\"heights\": [";
        for (usize j = 0; j < n; ++j) t += (j > 0 ? ",\"" : "\"") + row + "\"";
        return t + "]}";
    };
    std::string eight = "0 0 0 0 0 0 0 0";
    GIANT_CHECK(!load_cell_damage(rows(8, eight), c, back, &heights).has_value() && heights && heights->size() == 64);
    GIANT_CHECK(load_cell_damage(rows(8, "0 0 0 0 0 0 0"), c, back, &heights).has_value());     // a short row
    GIANT_CHECK(load_cell_damage(rows(8, eight + " 0"), c, back, &heights).has_value());        // a long one
    GIANT_CHECK(load_cell_damage(rows(12, eight), c, back, &heights).has_value());              // not a power of two
    GIANT_CHECK(load_cell_damage(rows(8, "0 0 0 0 0 0 0 nan"), c, back, &heights).has_value());
    GIANT_CHECK(load_cell_damage(rows(8, "0 0 0 0 0 0 0 20000"), c, back, &heights).has_value());
    GIANT_CHECK(load_cell_damage("nope", c, back).has_value());
    GIANT_CHECK(!load_cell_damage("{}", c, back).has_value() && back.empty());
    const Weather rain = *weather_preset("rain");
    Weather w;
    GIANT_CHECK(!load_weather(save_weather(rain), w).has_value() && w == rain);
    GIANT_CHECK(load_weather("{\"cloud_cover\": 7}", w).has_value() && w == rain);
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

GIANT_TEST("world: a cell's tile paint saves and loads exactly with its heights; paint without a tile is refused") {
    Terrain::Heights tile(64, 1.5f);
    Terrain::Paint paint(64 * k_paint_channels, 0);
    for (usize k = 0; k < paint.size(); k += 5) paint[k] = static_cast<u8>(255 - k % 256);
    const std::string cell = save_cell({2, -1}, std::vector<Entity>{}, &tile, nullptr, &paint);
    GIANT_CHECK(contains(cell, "\"paint\""));
    std::vector<Entity> entities;
    std::optional<Terrain::Heights> heights;
    Terrain::Paint back{1, 2, 3};
    GIANT_CHECK(!load_cell(cell, {2, -1}, 8, entities, heights, nullptr, &back).has_value());
    GIANT_CHECK(heights && *heights == tile && back == paint);
    GIANT_CHECK(save_cell({2, -1}, std::vector<Entity>{}, &*heights, nullptr, &back) == cell);
    // Empty paint is not written, and reads back empty.
    const Terrain::Paint none;
    const std::string plain = save_cell({2, -1}, std::vector<Entity>{}, &tile, nullptr, &none);
    GIANT_CHECK(!contains(plain, "paint"));
    GIANT_CHECK(!load_cell(plain, {2, -1}, 8, entities, heights, nullptr, &back).has_value() && back.empty());
    // Paint with no heights: refused, outputs unchanged.
    std::string orphan = cell;
    const usize from = orphan.find("\"heights\"");
    const usize to = orphan.find(']', from);
    GIANT_CHECK(from != std::string::npos && to != std::string::npos);
    // The key, its rows and the comma after them ("paint" follows).
    const usize comma = orphan.find(',', to);
    GIANT_CHECK(comma != std::string::npos);
    orphan.erase(from, comma + 1 - from);
    back = {7};
    const auto refused = load_cell(orphan, {2, -1}, 8, entities, heights, nullptr, &back);
    GIANT_CHECK(refused && contains(refused->message, "this cell has none"));
    GIANT_CHECK(back.size() == 1);
}

GIANT_TEST("world: two-sided and translucent materials save and load, only when set, and are checked") {
    Scene scene;
    Entity leaf = named("Leaf");
    leaf.material.two_sided = true;
    leaf.material.translucency = 0.45f;
    scene.add(leaf);
    scene.add(named("Rock"));
    ScatterLayer layer;
    layer.name = "grass";
    ScatterItem blade;
    blade.material.two_sided = true;
    layer.items.push_back(blade);
    scene.scatter.push_back(layer);
    const std::string text = save_scene(scene);
    GIANT_CHECK(text.find("\"two_sided\"") != text.rfind("\"two_sided\""));       // the leaf's and the grass's
    GIANT_CHECK(text.find("\"translucency\"") == text.rfind("\"translucency\"")); // the leaf's alone
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->material.two_sided && loaded.find(1)->material.translucency == 0.45f);
    GIANT_CHECK(loaded.find(2) && !loaded.find(2)->material.two_sided && loaded.find(2)->material.translucency == 0.0f);
    GIANT_CHECK(loaded.scatter[0].items[0].material.two_sided);
    GIANT_CHECK(save_scene(loaded) == text);
    Scene bad;
    const auto error = load_scene(
        R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "material": {"translucency": 2}}]})", bad);
    GIANT_CHECK(error.has_value() && contains(error->message, "translucency"));
}

GIANT_TEST("world: cut-out materials save and load their alpha cutoff, only when set, and it is checked") {
    Scene scene;
    Entity card = named("Card");
    card.material.alpha_cutoff = 0.5f;
    scene.add(card);
    scene.add(named("Rock"));
    ScatterLayer layer;
    layer.name = "grass";
    ScatterItem blade;
    blade.material.alpha_cutoff = 0.3f;
    layer.items.push_back(blade);
    scene.scatter.push_back(layer);
    const std::string text = save_scene(scene);
    GIANT_CHECK(text.find("\"alpha_cutoff\"") != text.rfind("\"alpha_cutoff\"")); // the card's and the grass's
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->material.alpha_cutoff == 0.5f);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->material.alpha_cutoff == 0.0f);
    GIANT_CHECK(loaded.scatter[0].items[0].material.alpha_cutoff == 0.3f);
    GIANT_CHECK(save_scene(loaded) == text);
    Scene bad;
    const auto error = load_scene(
        R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "material": {"alpha_cutoff": -1}}]})", bad);
    GIANT_CHECK(error.has_value() && contains(error->message, "alpha_cutoff"));
}

GIANT_TEST("world: canopy normals save and load, only when set, and are checked") {
    Scene scene;
    Entity crown = named("Crown");
    crown.material.canopy = 0.7f;
    scene.add(crown);
    scene.add(named("Rock"));
    ScatterLayer layer;
    layer.name = "bushes";
    ScatterItem bush;
    bush.material.canopy = 0.4f;
    layer.items.push_back(bush);
    scene.scatter.push_back(layer);
    const std::string text = save_scene(scene);
    GIANT_CHECK(text.find("\"canopy\"") != text.rfind("\"canopy\"")); // the crown's and the bush's, not the rock's
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.find(1) && loaded.find(1)->material.canopy == 0.7f);
    GIANT_CHECK(loaded.find(2) && loaded.find(2)->material.canopy == 0.0f);
    GIANT_CHECK(loaded.scatter[0].items[0].material.canopy == 0.4f);
    GIANT_CHECK(save_scene(loaded) == text);
    Scene bad;
    const auto error = load_scene(
        R"({"format": "giant-scene", "version": 1, "entities": [{"id": 1, "material": {"canopy": 2}}]})", bad);
    GIANT_CHECK(error.has_value() && contains(error->message, "canopy"));
}
