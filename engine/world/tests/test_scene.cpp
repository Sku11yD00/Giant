#include "giant/world/scene.h"

#include "giant_test.h"

#include <string>

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
                "entities[0].shape must be cube, sphere or plane");
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
