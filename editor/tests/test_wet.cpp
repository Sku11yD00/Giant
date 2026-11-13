// What went in the water looks wet a while after (wet_things.h).
#include "../src/wet_things.h"

#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/physics_state.h"
#include "../src/play_state.h"

#include "giant/world/water.h"
#include "giant_test.h"

#include <cmath>
#include <vector>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

sim::BodyDesc box(DVec3 at, Vec3 half, sim::Motion motion = sim::Motion::fixed, float mass = 0.0f) {
    sim::BodyDesc d;
    d.motion = motion;
    d.position = at;
    d.mass = mass;
    d.parts.push_back({sim::BoxShape{half}});
    return d;
}

world::Water lake(float level) {
    world::Water w;
    w.name = "lake";
    w.kind = world::WaterKind::lake;
    w.level = level;
    w.outline = {{-50.0, -50.0}, {50.0, -50.0}, {50.0, 50.0}, {-50.0, 50.0}};
    return w;
}

void run(sim::PhysicsWorld& physics, WetThings& wet, double seconds) {
    for (int i = 0; i < static_cast<int>(std::lround(seconds * 60.0)); ++i) {
        wet.after_step(physics, physics.step(1.0 / 60.0) * sim::PhysicsWorld::k_step_seconds);
    }
}

const WetThings::Wet* find(const WetThings& wet, sim::BodyId body) {
    for (const auto& [key, w] : wet.things()) {
        if (w.body == body) return &w;
    }
    return nullptr;
}

} // namespace

GIANT_TEST("wet: a crate floating is wet to its waterline, drawn so, and dries out of the water") {
    JobSystem jobs(2);
    sim::PhysicsWorld physics(jobs);
    physics.add(box({0.0, -4.5, 0.0}, {200.0f, 0.5f, 200.0f})); // a bed 4 m down, and dry land past the lake
    const std::vector<world::Water> water{lake(0.0f)};
    physics.set_water(water);
    // A 400 kg metre cube set in: it floats 0.4 m under.
    const sim::BodyId crate = physics.add(box({0.0, 0.4, 0.0}, {0.5f, 0.5f, 0.5f}, sim::Motion::dynamic, 400.0f));
    WetThings wet;
    run(physics, wet, 4.0);
    const WetThings::Wet* w = find(wet, crate);
    GIANT_CHECK(wet.size() == 1 && w != nullptr);
    if (w == nullptr) return;
    GIANT_CHECK(w->in_water && w->amount == 1.0f && w->axis == 2);
    GIANT_CHECK(w->line > 0.3f && w->line < 0.6f);
    std::vector<render::Decal> decals;
    wet.decals(physics, decals);
    GIANT_CHECK(decals.size() == 1);
    if (decals.size() == 1) {
        const render::Decal& d = decals[0];
        const DVec3 at = physics.state(crate)->position;
        GIANT_CHECK(d.shape == render::DecalShape::wet && d.opacity == 1.0f);
        // From just under its bottom up to the line, a little wider than it.
        GIANT_CHECK(std::fabs(d.size.x - 1.04f) < 0.01f && std::fabs(d.size.z - 1.04f) < 0.01f);
        GIANT_CHECK(std::fabs(d.size.y - (w->line + 0.02f + 0.04f)) < 1e-4f); // (the line's fade above it)
        const double bottom = d.position.y - 0.5 * static_cast<double>(d.size.y);
        GIANT_CHECK(std::fabs(bottom - (at.y - 0.52)) < 0.01);
        GIANT_CHECK(std::fabs(d.position.x - at.x) < 0.01 && std::fabs(d.position.z - at.z) < 0.01);
        GIANT_CHECK(d.softness > 0.0f && d.softness <= 0.5f);
    }
    // Lifted out onto dry land: out of the water, drying.
    physics.set_transform(crate, {120.0, -3.5, 0.0}, Quat{});
    run(physics, wet, 0.5);
    w = find(wet, crate);
    GIANT_CHECK(w != nullptr && !w->in_water && w->amount < 1.0f && w->amount > 0.99f);
    const float line = w != nullptr ? w->line : 0.0f;
    run(physics, wet, 3.0); // asleep there now: still drying
    w = find(wet, crate);
    GIANT_CHECK(w != nullptr && !w->in_water && w->line == line);
    GIANT_CHECK(w != nullptr && std::fabs(w->amount - (1.0f - 3.5f / WetThings::k_dry_seconds)) < 0.01f);
    // A step() call that ran no step changes nothing.
    const float amount = w != nullptr ? w->amount : 0.0f;
    wet.after_step(physics, 0.0);
    GIANT_CHECK(find(wet, crate) != nullptr && find(wet, crate)->amount == amount);
    // Dry after k_dry_seconds: forgotten, nothing drawn.
    physics.step(1.0 / 60.0);
    wet.after_step(physics, WetThings::k_dry_seconds);
    GIANT_CHECK(wet.size() == 0);
    decals.clear();
    wet.decals(physics, decals);
    GIANT_CHECK(decals.empty());
}

GIANT_TEST("wet: one turned over in the water is wet all over; one at rest on the bed stays wet; gone, forgotten") {
    JobSystem jobs(2);
    sim::PhysicsWorld physics(jobs);
    physics.add(box({0.0, -4.5, 0.0}, {200.0f, 0.5f, 200.0f}));
    const std::vector<world::Water> water{lake(0.0f)};
    physics.set_water(water);
    // A plank floating flat, and a heavy block sunk to the bed.
    const sim::BodyId plank = physics.add(box({0.0, 0.1, 0.0}, {1.0f, 0.1f, 0.3f}, sim::Motion::dynamic, 40.0f));
    const sim::BodyId block = physics.add(box({5.0, -3.0, 0.0}, {0.5f, 0.5f, 0.5f}, sim::Motion::dynamic, 3000.0f));
    WetThings wet;
    run(physics, wet, 3.0);
    GIANT_CHECK(find(wet, plank) != nullptr && find(wet, plank)->line < 1.0f);
    GIANT_CHECK(find(wet, block) != nullptr && find(wet, block)->line == 1.0f); // all under
    {
        // Wet all over: the box past its top too, no fade (its top face as wet as the rest).
        std::vector<render::Decal> decals;
        wet.decals(physics, decals);
        const DVec3 at = physics.state(block)->position;
        bool found = false;
        for (const render::Decal& d : decals) {
            if (std::fabs(d.position.x - at.x) > 0.1) continue;
            found = true;
            const double top = d.position.y + 0.5 * static_cast<double>(d.size.y);
            GIANT_CHECK(top >= at.y + 0.5 + 0.019 && d.softness == 0.0f);
        }
        GIANT_CHECK(found);
    }
    // Stood on its end in the water: its up side no longer up.
    physics.set_transform(plank, {0.0, 0.0, 0.0}, quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 1.5707963f));
    run(physics, wet, 0.1);
    GIANT_CHECK(find(wet, plank) != nullptr && find(wet, plank)->line == 1.0f);
    // The block asleep on the bed (not looked at by the physics): still in the water, not drying.
    run(physics, wet, 6.0);
    GIANT_CHECK(!physics.state(block)->active);
    GIANT_CHECK(find(wet, block) != nullptr && find(wet, block)->in_water && find(wet, block)->amount == 1.0f);
    physics.remove(block);
    run(physics, wet, 0.1);
    GIANT_CHECK(find(wet, block) == nullptr);
}

GIANT_TEST("wet: a character wading is wet to the water's depth over its feet, upright, the ground left out") {
    JobSystem jobs(2);
    sim::PhysicsWorld physics(jobs);
    physics.add(box({0.0, -1.5, 0.0}, {200.0f, 0.5f, 200.0f})); // a bed 1 m down
    const std::vector<world::Water> water{lake(0.0f)};
    physics.set_water(water);
    sim::CharacterDesc d;
    d.height = 1.8f;
    d.radius = 0.3f;
    d.position = {0.0, -1.0, 0.0};
    d.user = 7; // a walker's entity (drawn)
    const sim::CharacterId person = physics.add_character(d);
    // The player (no entity: not drawn) is wet but makes no decal.
    sim::CharacterDesc p = d;
    p.user = 0;
    p.position = {5.0, -1.0, 0.0};
    const sim::CharacterId player = physics.add_character(p);
    WetThings wet;
    run(physics, wet, 1.0);
    GIANT_CHECK(wet.size() == 2 && wet.in_water() == 2);
    const auto of = [&wet](sim::CharacterId c) -> const WetThings::Wet* {
        for (const auto& [key, x] : wet.things()) {
            if (x.character == c) return &x;
        }
        return nullptr;
    };
    GIANT_CHECK(of(player) != nullptr);
    const WetThings::Wet* w = of(person);
    GIANT_CHECK(w != nullptr && w->character == person && std::fabs(w->line - 1.0f / 1.8f) < 0.02f);
    std::vector<render::Decal> decals;
    wet.decals(physics, decals);
    GIANT_CHECK(decals.size() == 1);
    if (decals.size() == 1) {
        const DVec3 feet = physics.character_state(person)->position;
        const double bottom = decals[0].position.y - 0.5 * static_cast<double>(decals[0].size.y);
        GIANT_CHECK(bottom > feet.y + 0.02 && bottom < feet.y + 0.04);
        // Up to the surface, its fade above it.
        GIANT_CHECK(std::fabs(bottom + static_cast<double>(decals[0].size.y) - 0.04) < 0.03);
        GIANT_CHECK(std::fabs(decals[0].size.x - 0.8f) < 1e-4f && decals[0].rotation == Quat{});
    }
    // Walked out onto dry land: drying.
    physics.set_character_position(person, {120.0, -1.0, 0.0});
    run(physics, wet, 0.5);
    w = of(person);
    GIANT_CHECK(w != nullptr && !w->in_water && w->amount < 1.0f);
    physics.remove_character(person);
    physics.remove_character(player);
    run(physics, wet, 0.1);
    GIANT_CHECK(wet.size() == 0);
}

GIANT_TEST("wet: at most k_most remembered, the driest out of the water making way") {
    JobSystem jobs(2);
    sim::PhysicsWorld physics(jobs);
    physics.add(box({0.0, -1.5, 0.0}, {200.0f, 0.5f, 200.0f}));
    const std::vector<world::Water> water{lake(0.0f)};
    physics.set_water(water);
    std::vector<sim::BodyId> bodies;
    for (u32 i = 0; i < WetThings::k_most + 40; ++i) {
        const double x = -45.0 + static_cast<double>(i % 30) * 3.0;
        const double z = -45.0 + static_cast<double>(i / 30) * 3.0;
        bodies.push_back(physics.add(box({x, 0.1, z}, {0.25f, 0.25f, 0.25f}, sim::Motion::dynamic, 5.0f)));
    }
    WetThings wet;
    run(physics, wet, 0.5);
    GIANT_CHECK(wet.size() == WetThings::k_most && wet.in_water() == WetThings::k_most);
    // Forty lifted out (drying) make way for the forty not remembered, still in the water.
    std::vector<sim::BodyId> lifted;
    for (const auto& [key, w] : wet.things()) {
        if (lifted.size() < 40) lifted.push_back(w.body);
    }
    for (usize i = 0; i < lifted.size(); ++i) {
        physics.set_transform(lifted[i], {120.0 + 2.0 * static_cast<double>(i), -0.75, 0.0}, Quat{});
    }
    run(physics, wet, 0.5);
    GIANT_CHECK(wet.in_water() == WetThings::k_most);
    for (const sim::BodyId b : lifted) GIANT_CHECK(find(wet, b) == nullptr);
    GIANT_CHECK(wet.size() == WetThings::k_most);
}

GIANT_TEST("wet: one floating on its side is wet along the side that is up; back in another way up, along that") {
    JobSystem jobs(2);
    sim::PhysicsWorld physics(jobs);
    physics.add(box({0.0, -4.5, 0.0}, {200.0f, 0.5f, 200.0f}));
    const std::vector<world::Water> water{lake(0.0f)};
    physics.set_water(water);
    // A slab 0.4 thick along its own x (1 by 0.8 across), turned so its x is up; 128 kg floats 0.16 m under.
    const Quat on_side = quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 1.5707963f);
    const sim::BodyId slab = physics.add(box({0.0, 0.05, 0.0}, {0.2f, 0.5f, 0.4f}, sim::Motion::dynamic, 128.0f));
    physics.set_transform(slab, {0.0, 0.05, 0.0}, on_side);
    WetThings wet;
    run(physics, wet, 4.0);
    const WetThings::Wet* w = find(wet, slab);
    GIANT_CHECK(w != nullptr && w->axis == 0 && w->line > 0.2f && w->line < 0.8f);
    std::vector<render::Decal> decals;
    wet.decals(physics, decals);
    GIANT_CHECK(decals.size() == 1);
    if (decals.size() == 1 && w != nullptr) {
        const render::Decal& d = decals[0];
        const sim::BodyState s = *physics.state(slab);
        // Its +Y is the world's up; across, the slab's other sides.
        const Vec3 up = rotate(d.rotation, {0.0f, 1.0f, 0.0f});
        GIANT_CHECK(up.y > 0.99f);
        GIANT_CHECK(std::fabs(d.size.x - 1.04f) < 0.01f && std::fabs(d.size.z - 0.84f) < 0.01f);
        GIANT_CHECK(std::fabs(d.size.y - (w->line * 0.4f + 0.06f)) < 1e-3f);
        const double bottom = d.position.y - 0.5 * static_cast<double>(d.size.y);
        GIANT_CHECK(std::fabs(bottom - (s.position.y - 0.22)) < 0.02);
    }
    // Lifted out, then set back in flat (its y up now): wet along y from there, not all over.
    physics.set_transform(slab, {120.0, -3.0, 0.0}, Quat{});
    run(physics, wet, 0.5);
    GIANT_CHECK(find(wet, slab) != nullptr && !find(wet, slab)->in_water);
    physics.set_transform(slab, {0.0, 0.3, 0.0}, Quat{});
    run(physics, wet, 0.2);
    w = find(wet, slab);
    GIANT_CHECK(w != nullptr && w->in_water && w->axis == 2 && w->line < 1.0f);
}

GIANT_TEST("wet: get_physics reports what is wet; stopping or another scene forgets it") {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    PhysicsState physics;
    PlayState play{physics};
    EditorState state;
    state.meshes = &meshes;
    state.physics = &physics;
    state.play = &play;
    register_scene_commands(commands, state);
    register_physics_commands(commands, state);
    register_water_commands(commands, state);
    const auto run_command = [&commands](const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    };
    GIANT_CHECK(run_command("create_water", {{"name", "pond"}, {"kind", "lake"}, {"level", 0.0},
                                              {"outline", Json::array({{-20, -20}, {20, -20}, {20, 20}, {-20, 20}})}})
                    .ok);
    GIANT_CHECK(run_command("create_entity", {{"shape", "plane"}, {"position", {0, -3, 0}}, {"scale", {100, 1, 100}}}).ok);
    const ai::CommandResult made =
        run_command("create_entity", {{"shape", "cube"}, {"position", {0, 0.3, 0}}, {"body", "dynamic"}, {"mass", 400}});
    GIANT_CHECK(made.ok);
    const world::EntityId cube = made.ok ? made.value["id"].get<world::EntityId>() : 0;
    GIANT_CHECK(run_command("simulate", {{"on", true}}).ok);
    ai::CommandResult r = run_command("step_physics", {{"seconds", 2}});
    GIANT_CHECK(r.ok && r.value["wet"]["count"] == 1 && r.value["wet"]["in_water"] == 1);
    if (r.ok && r.value["wet"]["things"].size() == 1) {
        const Json& t = r.value["wet"]["things"][0];
        GIANT_CHECK(t["entity"] == cube && t["body"] != 0 && t["character"] == false && t["player"] == false);
        GIANT_CHECK(t["amount"] == 1.0 && t["line"].get<double>() > 0.2 && t["in_water"] == true);
    }
    // Stopping: the bodies back where they were, dry.
    GIANT_CHECK(run_command("simulate", {{"on", false}}).ok);
    GIANT_CHECK(physics.wet().size() == 0);
    GIANT_CHECK(run_command("simulate", {{"on", true}}).ok);
    GIANT_CHECK(run_command("step_physics", {{"seconds", 1}}).ok && physics.wet().size() == 1);
    // Another scene (its bodies' ids made again): forgotten.
    state.scene = world::Scene{};
    physics.flush(state.scene);
    GIANT_CHECK(physics.wet().size() == 0);
}

GIANT_TEST("wet: back in after drying a while, wet from the new waterline; out a moment, the line kept") {
    JobSystem jobs(2);
    sim::PhysicsWorld physics(jobs);
    physics.add(box({0.0, -4.5, 0.0}, {200.0f, 0.5f, 200.0f})); // a bed 4 m down, dry land past the lake
    physics.set_water(std::vector<world::Water>{lake(0.0f)});
    const sim::BodyId block = physics.add(box({0.0, -3.5, 0.0}, {0.5f, 0.5f, 0.5f}, sim::Motion::dynamic, 3000.0f));
    WetThings wet;
    run(physics, wet, 0.5);
    GIANT_CHECK(find(wet, block) != nullptr && find(wet, block)->line == 1.0f);
    // Out a moment and back: still wet all over.
    physics.set_transform(block, {120.0, -3.5, 0.0}, Quat{});
    run(physics, wet, 0.2);
    GIANT_CHECK(find(wet, block) != nullptr && !find(wet, block)->in_water);
    physics.set_transform(block, {0.0, -3.5, 0.0}, Quat{});
    run(physics, wet, 0.2);
    GIANT_CHECK(find(wet, block) != nullptr && find(wet, block)->in_water && find(wet, block)->line == 1.0f);
    // Out 20 s, then into water 0.2 m deep: wet to there only.
    physics.set_transform(block, {120.0, -3.5, 0.0}, Quat{});
    run(physics, wet, 0.2);
    physics.step(1.0 / 60.0);
    wet.after_step(physics, 20.0);
    GIANT_CHECK(find(wet, block) != nullptr && find(wet, block)->amount < 0.9f);
    physics.set_water(std::vector<world::Water>{lake(-3.8f)});
    physics.set_transform(block, {0.0, -3.5, 0.0}, Quat{});
    run(physics, wet, 0.5);
    const WetThings::Wet* w = find(wet, block);
    GIANT_CHECK(w != nullptr && w->in_water && w->line > 0.1f && w->line < 0.4f);
}
