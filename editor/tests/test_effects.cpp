// Effects in the editor: particle looks and effects made, changed, renamed
// and removed by command (undoable, saved with the scene), and entities
// given an effect to run.
#include "../src/editor_commands.h"
#include "../src/effects_state.h"
#include "../src/event_effects.h"
#include "../src/physics_state.h"
#include "../src/mesh_library.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"
#include "giant/world/effects.h"

#include <cmath>
#include <limits>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        register_scene_commands(commands, state);
        register_effects_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
};

Json steam_emitter() {
    return {{"look", "steam"}, {"rate", 12}, {"shape", "box"}, {"size", {1, 0.5, 1}}, {"ground", false}};
}

} // namespace

GIANT_TEST("effects commands: looks made from built-ins or defaults, changed, renamed with their effects, removed") {
    Fixture f;
    // A new look of a built-in's name starts from it; another from the defaults.
    ai::CommandResult r = f.run("set_particle_look", {{"name", "smoke"}, {"opacity_start", 0.3}});
    GIANT_CHECK(r.ok && r.value["created"] == true && r.value["buoyancy"] == world::find_particle_look({}, "smoke")->buoyancy);
    GIANT_CHECK(f.state.scene.particle_looks.size() == 1 && f.state.scene.particle_looks[0].opacity_start == 0.3f);
    r = f.run("set_particle_look", {{"name", "steam"}, {"life_min", 2}, {"life_max", 3}, {"form", "streak"}});
    GIANT_CHECK(r.ok && r.value["created"] == true && r.value["form"] == "streak" && r.value["size_start"] == 0.2f);
    // Changed: only what is given.
    r = f.run("set_particle_look", {{"name", "steam"}, {"color_start", {0.9, 0.9, 0.9}}});
    GIANT_CHECK(r.ok && r.value["created"] == false && f.state.scene.particle_looks[1].life_max == 3.0f &&
                f.state.scene.particle_looks[1].color_start.x == 0.9f);
    // Refused: life_min over life_max (left as it was), a rename onto another.
    GIANT_CHECK(!f.run("set_particle_look", {{"name", "steam"}, {"life_min", 5}}).ok);
    GIANT_CHECK(f.state.scene.particle_looks[1].life_min == 2.0f);
    GIANT_CHECK(!f.run("set_particle_look", {{"name", "steam"}, {"new_name", "smoke"}}).ok);

    GIANT_CHECK(f.run("create_effect", {{"name", "vent"}, {"emitters", {steam_emitter()}}, {"duration", 5}}).ok);
    // Renaming the look renames it in the effects, in one undo.
    GIANT_CHECK(f.run("set_particle_look", {{"name", "steam"}, {"new_name", "vapour"}}).ok);
    GIANT_CHECK(f.state.scene.particle_looks[1].name == "vapour" && f.state.scene.effects[0].emitters[0].look == "vapour");
    GIANT_CHECK(f.run("undo").ok);
    GIANT_CHECK(f.state.scene.particle_looks[1].name == "steam" && f.state.scene.effects[0].emitters[0].look == "steam");
    // A look an effect needs (no built-in of its name) is not removed; the scene's smoke is (built-in remains).
    GIANT_CHECK(!f.run("remove_particle_look", {{"name", "steam"}}).ok);
    GIANT_CHECK(f.run("remove_particle_look", {{"name", "smoke"}}).ok && f.state.scene.particle_looks.size() == 1);
    GIANT_CHECK(!f.run("remove_particle_look", {{"name", "smoke"}}).ok); // built-in: not the scene's
    GIANT_CHECK(f.run("undo").ok && f.state.scene.particle_looks.size() == 2);
    // A rename of a look not the scene's yet makes it, from the built-in, under the new name; effects are untouched.
    r = f.run("set_particle_look", {{"name", "fire"}, {"new_name", "blue_fire"}, {"emissive", {0.5, 1, 6}}});
    GIANT_CHECK(r.ok && r.value["created"] == true && r.value["name"] == "blue_fire" &&
                r.value["buoyancy"] == world::find_particle_look({}, "fire")->buoyancy);
    GIANT_CHECK(f.state.scene.particle_looks.size() == 3 && f.state.scene.effects[0].emitters[0].look == "steam");
    // Made looks are undone and redone.
    GIANT_CHECK(f.run("undo").ok && f.state.scene.particle_looks.size() == 2);
    GIANT_CHECK(f.run("redo").ok && f.state.scene.particle_looks.back().name == "blue_fire");
}

GIANT_TEST("effects commands: effects made, changed, renamed with their entities, removed, listed, saved") {
    Fixture f;
    ai::CommandResult r = f.run("create_effect", {{"name", "vent"}, {"emitters", {steam_emitter()}}});
    GIANT_CHECK(r.ok && r.value["missing_looks"] == Json::array({"steam"})); // made before its look
    GIANT_CHECK(f.run("set_particle_look", {{"name", "steam"}}).ok);
    // Refused: the same name, no emitters, a bad emitter, too many.
    GIANT_CHECK(!f.run("create_effect", {{"name", "vent"}, {"emitters", {steam_emitter()}}}).ok);
    GIANT_CHECK(!f.run("create_effect", {{"name", "b"}, {"emitters", Json::array()}}).ok);
    Json bad = steam_emitter();
    bad["speed_min"] = 3;
    bad["speed_max"] = 1;
    GIANT_CHECK(!f.run("create_effect", {{"name", "b"}, {"emitters", {bad}}}).ok);
    GIANT_CHECK(f.state.scene.effects.size() == 1);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.particle_looks.empty() && f.run("undo").ok && f.state.scene.effects.empty());
    GIANT_CHECK(f.run("redo").ok && f.run("redo").ok && f.state.scene.effects.size() == 1 &&
                f.state.scene.particle_looks.size() == 1);
    // A built-in's name stands in for it.
    GIANT_CHECK(f.run("create_effect", {{"name", "fire"}, {"emitters", {{{"look", "fire"}, {"rate", 80}}}}}).ok);

    // An entity runs one; refused when on with no name.
    r = f.run("create_entity", {{"name", "chimney"}, {"effect", {{"on", true}, {"effect", "vent"}, {"rate", 2}}}});
    GIANT_CHECK(r.ok && r.value["effect"]["effect"] == "vent" && r.value["effect"]["rate"] == 2.0);
    const world::EntityId id = r.value["id"].get<world::EntityId>();
    GIANT_CHECK(!f.run("create_entity", {{"name", "x"}, {"effect", {{"on", true}}}}).ok);
    GIANT_CHECK(f.state.scene.find(id)->effect.on && f.state.scene.find(id)->effect.effect == "vent");
    // Kit pieces take none (the kit entity runs one).
    GIANT_CHECK(!f.run("create_kit", {{"name", "hut"}, {"pieces", {{{"shape", "cube"}, {"effect", {{"on", true}, {"effect", "smoke"}}}}}}}).ok);
    GIANT_CHECK(f.run("create_kit", {{"name", "hut"}, {"pieces", {{{"shape", "cube"}}}}}).ok);

    // Changed: only the fields given; renamed, and the entity with it, in one undo.
    GIANT_CHECK(f.run("set_effect", {{"name", "vent"}, {"duration", 3}}).ok);
    GIANT_CHECK(f.state.scene.effects[0].duration == 3.0f && f.state.scene.effects[0].emitters.size() == 1);
    r = f.run("set_effect", {{"name", "vent"}, {"new_name", "chimney_smoke"}});
    GIANT_CHECK(r.ok && r.value["entities_renamed"] == 1);
    GIANT_CHECK(f.state.scene.find(id)->effect.effect == "chimney_smoke");
    GIANT_CHECK(f.run("undo").ok);
    GIANT_CHECK(f.state.scene.effects[0].name == "vent" && f.state.scene.find(id)->effect.effect == "vent");
    GIANT_CHECK(f.run("redo").ok && f.state.scene.find(id)->effect.effect == "chimney_smoke");
    {
        // An undo with an entity naming it gone (streamed out) changes nothing: the two stay in step.
        Fixture g;
        GIANT_CHECK(g.run("create_effect", {{"name", "vent"}, {"emitters", {{{"look", "smoke"}}}}}).ok);
        const ai::CommandResult made = g.run("create_entity", {{"effect", {{"on", true}, {"effect", "vent"}}}});
        GIANT_CHECK(made.ok && g.run("set_effect", {{"name", "vent"}, {"new_name", "flue"}}).ok);
        g.state.scene.remove(made.value["id"].get<world::EntityId>());
        GIANT_CHECK(g.run("undo").ok);
        GIANT_CHECK(g.state.scene.effects[0].name == "flue");
    }
    GIANT_CHECK(!f.run("set_effect", {{"name", "smoke"}, {"duration", 1}}).ok); // built-in, not the scene's

    // Listed: the scene's, the built-in names (the scene's fire standing in), who names each.
    r = f.run("get_effects");
    GIANT_CHECK(r.ok && r.value["effects"].size() == 2 && r.value["particle_looks"].size() == 1);
    GIANT_CHECK(r.value["effects"][0]["entities"] == Json::array({id}));
    GIANT_CHECK(r.value["built_in"]["effects"].size() == world::built_in_effects().size() && !r.value["built_in"]["effects"][0].contains("emitters"));
    bool fire_stood_in = false;
    for (const Json& j : r.value["built_in"]["effects"]) fire_stood_in |= j["name"] == "fire" && j.value("stood_in_for", false);
    GIANT_CHECK(fire_stood_in);
    r = f.run("get_effects", {{"built_in", true}});
    GIANT_CHECK(r.ok && r.value["built_in"]["effects"][0].contains("emitters") &&
                r.value["built_in"]["particle_looks"][0].contains("life_min"));

    // Saved with the scene.
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(f.state.scene), loaded).has_value());
    GIANT_CHECK(loaded.effects == f.state.scene.effects && loaded.particle_looks == f.state.scene.particle_looks);
    GIANT_CHECK(loaded.find(id)->effect == f.state.scene.find(id)->effect);

    // Removed (the entity keeps naming it); undone.
    r = f.run("remove_effect", {{"name", "chimney_smoke"}});
    GIANT_CHECK(r.ok && r.value["entities"] == Json::array({id}) && f.state.scene.effects.size() == 1);
    GIANT_CHECK(f.state.scene.find(id)->effect.effect == "chimney_smoke");
    GIANT_CHECK(!f.run("remove_effect", {{"name", "chimney_smoke"}}).ok);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.effects.size() == 2);
}

// --- Emitting (effects plan FX2b) ---------------------------------------------

namespace {

const EffectsState::Ground k_flat = [](double, double) { return std::optional<double>(0.0); };

world::EntityId add_runner(world::Scene& scene, const DVec3& at, const char* effect, float rate = 1.0f) {
    world::Entity e;
    e.transform.position = at;
    e.effect = {true, effect, rate};
    return scene.add(e);
}

// An update whose bursts are drawn (taken).
void tick(EffectsState& fx, const world::Scene& scene, const DVec3& camera, double seconds,
          const EffectsState::Ground& ground) {
    fx.update(scene, camera, seconds, ground);
    (void)fx.take();
}

u32 born(const EffectsState& fx) {
    u32 n = 0;
    for (const render::ParticleBurst& b : fx.bursts()) n += b.count;
    return n;
}

} // namespace

GIANT_TEST("effects emitting: an entity's effect runs its rates where it stands, fractions carried, paused gives none") {
    world::Scene scene;
    const world::EntityId id = add_runner(scene, {10.0, 2.0, -5.0}, "smoke"); // 6 a second off a 0.3 m disc
    EffectsState fx;
    fx.follow(scene, scene.untaken_changes());
    u32 total = 0;
    for (int i = 0; i < 8; ++i) { // 2 s at 0.25 s: 12
        tick(fx, scene, {}, 0.25, k_flat);
        total += born(fx);
        for (const render::ParticleBurst& b : fx.bursts()) {
            GIANT_CHECK(b.count == 1 && b.spread == 0.0f && b.ground < -1e29); // smoke does not stop on the ground
            GIANT_CHECK(std::abs(b.position.y - 2.0) < 1e-6);
            const double dx = b.position.x - 10.0, dz = b.position.z + 5.0;
            GIANT_CHECK(dx * dx + dz * dz <= 0.3 * 0.3 + 1e-6);
            GIANT_CHECK(b.style < fx.styles().size());
        }
    }
    GIANT_CHECK(total == 12 && fx.stats().running == 1);
    // Paused: nothing.
    tick(fx, scene, {}, 0.0, k_flat);
    GIANT_CHECK(fx.bursts().empty());
    // A rate scale; off; hidden; far.
    scene.modify(id)->effect.rate = 2.0f;
    fx.follow(scene, scene.take_changes());
    total = 0;
    for (int i = 0; i < 4; ++i) {
        tick(fx, scene, {}, 0.25, k_flat);
        total += born(fx);
    }
    GIANT_CHECK(total == 12);
    scene.modify(id)->effect.on = false;
    fx.follow(scene, scene.take_changes());
    tick(fx, scene, {}, 0.25, k_flat);
    GIANT_CHECK(fx.bursts().empty() && fx.stats().running == 0);
    scene.modify(id)->effect.on = true;
    scene.modify(id)->visible = false;
    fx.follow(scene, scene.take_changes());
    tick(fx, scene, {}, 0.25, k_flat);
    GIANT_CHECK(fx.bursts().empty());
    scene.modify(id)->visible = true;
    fx.follow(scene, scene.take_changes());
    fx.reach = 5.0;
    tick(fx, scene, {}, 1.0, k_flat);
    GIANT_CHECK(fx.bursts().empty() && fx.stats().far == 1);
    // Naming nothing: counted.
    fx.reach = EffectsState::k_default_reach;
    scene.modify(id)->effect.effect = "nothing";
    fx.follow(scene, scene.take_changes());
    tick(fx, scene, {}, 0.25, k_flat);
    GIANT_CHECK(fx.bursts().empty() && fx.stats().missing == 1);
}

GIANT_TEST("effects emitting: spawned effects burst once, turned, landing on the ground, then end") {
    world::Scene scene;
    EffectsState fx;
    fx.follow(scene, scene.untaken_changes());
    GIANT_CHECK(!fx.spawn(scene, "nothing", {}).has_value());
    const Quat turn = normalize(quat_from_euler({0.0f, 0.0f, 90.0f})); // rolled: up becomes sideways
    GIANT_CHECK(fx.spawn(scene, "sparks", {1.0, 6.0, 2.0}, turn).has_value());
    const EffectsState::Ground five = [](double, double) { return std::optional<double>(5.0); };
    tick(fx, scene, {}, 1.0 / 60.0, five);
    GIANT_CHECK(fx.bursts().size() == 1 && born(fx) == 40);
    const render::ParticleBurst& b = fx.bursts()[0];
    GIANT_CHECK(std::abs(b.ground - 4.99) < 1e-9 && b.position == (DVec3{1.0, 6.0, 2.0}));
    GIANT_CHECK(std::abs(b.direction.y) < 1e-5f && std::abs(std::abs(b.direction.x) - 1.0f) < 1e-5f);
    GIANT_CHECK(fx.stats().spawned == 0); // sparks: bursts only, done
    tick(fx, scene, {}, 1.0 / 60.0, five);
    GIANT_CHECK(fx.bursts().empty());
    // Born below the ground's height: they land where they were born.
    GIANT_CHECK(fx.spawn(scene, "sparks", {0.0, 3.0, 0.0}).has_value());
    tick(fx, scene, {}, 1.0 / 60.0, five);
    GIANT_CHECK(fx.bursts().size() == 1 && std::abs(fx.bursts()[0].ground - 2.99) < 1e-9);
    // One with a duration runs its rates that long (mist: 8 a second for 10 s), a spawn standing in included.
    GIANT_CHECK(fx.spawn(scene, "mist", {}).has_value());
    u32 total = 0;
    for (int i = 0; i < 60; ++i) {
        tick(fx, scene, {}, 0.25, k_flat);
        total += born(fx);
    }
    GIANT_CHECK(total == 80 && fx.stats().spawned == 0);
    // The cap.
    for (u32 i = 0; i < EffectsState::k_max_spawned; ++i) GIANT_CHECK(fx.spawn(scene, "mist", {}).has_value());
    GIANT_CHECK(!fx.spawn(scene, "mist", {}).has_value());
    fx.clear();
    GIANT_CHECK(fx.spawn(scene, "mist", {}).has_value());
    // Another scene (one loaded): what was spawned is forgotten.
    const world::Scene other;
    tick(fx, other, {}, 0.25, k_flat);
    GIANT_CHECK(fx.stats().spawned == 0 && fx.bursts().empty());
}

GIANT_TEST("effects emitting: budgets go to the nearest, the rest counted; the same steps give the same bursts") {
    world::Scene scene;
    EffectsState fx;
    fx.births_per_frame = 50;
    fx.follow(scene, scene.untaken_changes());
    GIANT_CHECK(fx.spawn(scene, "sparks", {100.0, 0.0, 0.0}).has_value()); // 40 each, the far one first
    GIANT_CHECK(fx.spawn(scene, "sparks", {10.0, 0.0, 0.0}).has_value());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(born(fx) == 50 && fx.stats().cut == 30 && fx.stats().born == 50);
    GIANT_CHECK(fx.bursts()[0].position.x == 10.0 && fx.bursts()[0].count == 40 && fx.bursts()[1].count == 10);
    // Grounded ones have their own budget.
    fx.births_per_frame = 1000;
    fx.grounded_per_frame = 5;
    GIANT_CHECK(fx.spawn(scene, "sparks", {}).has_value());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(born(fx) == 5 && fx.stats().cut == 35);

    // Deterministic.
    const auto run = [] {
        world::Scene s;
        add_runner(s, {0.0, 0.0, 0.0}, "fire");
        add_runner(s, {3.0, 0.0, 0.0}, "mist");
        EffectsState f;
        f.follow(s, s.untaken_changes());
        std::vector<render::ParticleBurst> all;
        for (int i = 0; i < 10; ++i) {
            tick(f, s, {}, 0.1, k_flat);
            all.insert(all.end(), f.bursts().begin(), f.bursts().end());
        }
        return all;
    };
    const auto a = run(), b = run();
    GIANT_CHECK(a.size() == b.size() && !a.empty());
    for (usize i = 0; i < a.size() && i < b.size(); ++i) {
        GIANT_CHECK(a[i].seed == b[i].seed && a[i].count == b[i].count && a[i].position == b[i].position);
    }
}

GIANT_TEST("effects emitting: every built-in and stood-in look and effect is drawable: the renderer drops nothing") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    rhi::TextureDesc t;
    t.width = 64;
    t.height = 64;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(t);
    world::Scene scene;
    // A look at the bounds stands in for smoke; an effect at the bounds.
    world::ParticleLook edge = *world::find_particle_look({}, "smoke");
    edge.buoyancy = world::k_max_look_buoyancy;
    edge.gravity = -world::k_max_look_pull;
    edge.wind = world::k_max_look_pull;
    edge.drag = world::k_max_look_drag;
    edge.emissive = {world::k_max_look_light, world::k_max_look_light, world::k_max_look_light};
    edge.size_start = edge.size_end = edge.wobble = edge.soft = world::k_max_look_size;
    edge.life_min = edge.life_max = world::k_max_look_life;
    scene.particle_looks = {edge};
    world::Emitter far_box;
    far_box.look = "smoke";
    far_box.burst = 64;
    far_box.shape = world::EmitterShape::box;
    far_box.size = {world::k_max_emitter_reach, world::k_max_emitter_reach, world::k_max_emitter_reach};
    far_box.offset = {world::k_max_emitter_reach, -world::k_max_emitter_reach, 0.0f};
    far_box.speed_min = far_box.speed_max = world::k_max_emitter_speed;
    far_box.cone_degrees = 180.0f;
    far_box.size_scale = 1000.0f;
    world::Emitter ball = far_box;
    ball.shape = world::EmitterShape::sphere;
    scene.effects = {{"edge", {far_box, ball}, 0.0f, {}}};
    EffectsState fx;
    fx.follow(scene, scene.untaken_changes());
    for (const world::Effect& f : world::built_in_effects()) GIANT_CHECK(fx.spawn(scene, f.name, {}).has_value());
    GIANT_CHECK(fx.spawn(scene, "edge", {}).has_value());
    tick(fx, scene, {}, 0.25, k_flat);
    GIANT_CHECK(fx.stats().missing == 0 && fx.stats().cut == 0 && born(fx) > 0);
    render::FrameInput frame;
    frame.settings.gpu_culling = false;
    frame.particle_styles = fx.styles();
    frame.particle_bursts = fx.bursts();
    rhi::CommandList* cmd = device->begin_frame(nullptr);
    renderer->render(*cmd, target, frame);
    device->end_frame();
    const render::RenderStats s = renderer->last_stats();
    GIANT_CHECK(s.particles_dropped == 0 && s.particles_born == born(fx));
}

GIANT_TEST("effects emitting: spawn_effect plays one and get_effects reports the emitting") {
    Fixture f;
    EffectsState fx;
    f.state.effects = &fx;
    ai::CommandResult r = f.run("spawn_effect", {{"name", "dust_kick"}, {"position", {1, 2, 3}}, {"rotation", {{"yaw", 45}}}});
    GIANT_CHECK(r.ok && r.value["spawned"].get<u32>() >= 1);
    GIANT_CHECK(!f.run("spawn_effect", {{"name", "nothing"}, {"position", {0, 0, 0}}}).ok);
    fx.follow(f.state.scene, f.state.scene.untaken_changes());
    tick(fx, f.state.scene, {}, 0.1, k_flat);
    r = f.run("get_effects");
    GIANT_CHECK(r.ok && r.value["emitting"]["born"] == 12 && r.value["emitting"]["running"] == 1);
    f.state.effects = nullptr;
    GIANT_CHECK(!f.run("spawn_effect", {{"name", "dust_kick"}, {"position", {0, 0, 0}}}).ok);
    GIANT_CHECK(f.run("get_effects").value["emitting"].is_null());
}

GIANT_TEST("effects emitting: births of a frame not drawn are given with the next; styles keep their slots") {
    world::Scene scene;
    EffectsState fx;
    fx.follow(scene, scene.untaken_changes());
    GIANT_CHECK(fx.spawn(scene, "sparks", {}).has_value());
    fx.update(scene, {}, 0.1, k_flat); // not taken: the frame failed to draw
    GIANT_CHECK(fx.spawn(scene, "dust_kick", {}).has_value());
    fx.update(scene, {}, 0.1, k_flat);
    GIANT_CHECK(born(fx) == 40 + 12);
    (void)fx.take();
    fx.update(scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.bursts().empty());

    // Slots: a new look, a stand-in, a removal leave the others' indices as they were.
    const u32 smoke = fx.style_index("smoke").value(), fire = fx.style_index("fire").value();
    world::ParticleLook ash = *world::find_particle_look({}, "smoke");
    ash.name = "ash";
    scene.particle_looks = {ash};
    fx.update(scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.style_index("smoke") == smoke && fx.style_index("fire") == fire && fx.style_index("ash").has_value());
    const u32 ash_slot = *fx.style_index("ash");
    world::ParticleLook red = *world::find_particle_look({}, "smoke");
    red.color_start = {1.0f, 0.0f, 0.0f};
    scene.particle_looks.push_back(red); // stands in for smoke: in smoke's slot
    fx.update(scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.style_index("smoke") == smoke && fx.styles()[smoke].color_start.x == 1.0f);
    scene.particle_looks.erase(scene.particle_looks.begin()); // ash gone: its slot kept, not offered
    fx.update(scene, {}, 0.1, k_flat);
    GIANT_CHECK(!fx.style_index("ash").has_value() && fx.style_index("smoke") == smoke && fx.styles().size() > ash_slot);
    // Many looks come and go: never past the renderer's table, the live ones kept.
    for (int round = 0; round < 3; ++round) {
        scene.particle_looks.clear();
        for (u32 k = 0; k < world::k_max_particle_looks; ++k) {
            world::ParticleLook l = ash;
            l.name = "look" + std::to_string(round) + "_" + std::to_string(k);
            scene.particle_looks.push_back(l);
        }
        fx.update(scene, {}, 0.1, k_flat);
        GIANT_CHECK(fx.styles().size() <= render::k_max_particle_styles);
        GIANT_CHECK(fx.style_index("look" + std::to_string(round) + "_0").has_value() && fx.style_index("fire") == fire);
    }
}

GIANT_TEST("effects emitting: ground under each point of a box, none without terrain, a spawn's rates its duration") {
    world::Scene scene;
    world::Emitter e;
    e.look = "sparks";
    e.rate = 0.0f;
    e.burst = 64;
    e.shape = world::EmitterShape::box;
    e.size = {50.0f, 0.0f, 0.0f};
    world::Emitter drip;
    drip.look = "spray";
    drip.rate = 100.0f;
    scene.effects = {{"slope", {e}, 0.0f, {}}, {"drip", {drip}, 0.05f, {}}};
    EffectsState fx;
    fx.follow(scene, scene.untaken_changes());
    const EffectsState::Ground slope = [](double x, double) { return std::optional<double>(x * 0.1); };
    GIANT_CHECK(fx.spawn(scene, "slope", {0.0, 2.0, 0.0}).has_value());
    tick(fx, scene, {}, 0.1, slope);
    GIANT_CHECK(fx.bursts().size() == 64);
    for (const render::ParticleBurst& b : fx.bursts()) {
        GIANT_CHECK(std::abs(b.ground - (std::min(b.position.x * 0.1, 2.0) - 0.01)) < 1e-9);
    }
    // No terrain: they fall on, and the grounded budget is not spent.
    fx.grounded_per_frame = 1;
    GIANT_CHECK(fx.spawn(scene, "sparks", {}).has_value());
    tick(fx, scene, {}, 0.1, [](double, double) { return std::optional<double>(); });
    GIANT_CHECK(born(fx) == 40 && fx.stats().cut == 0 && fx.bursts()[0].ground < -1e29);
    // A spawn's rates run only its duration, however long the frame: 100 a second for 0.05 s.
    fx.grounded_per_frame = 1000;
    GIANT_CHECK(fx.spawn(scene, "drip", {}).has_value());
    tick(fx, scene, {}, 0.25, k_flat);
    GIANT_CHECK(born(fx) == 5);
    tick(fx, scene, {}, 0.25, k_flat);
    GIANT_CHECK(fx.bursts().empty() && fx.stats().spawned == 0);
}

GIANT_TEST("effects emitting: a sphere lands under its middle less its radius; headings turn; edits and clear followed") {
    world::Scene scene;
    world::Emitter ball;
    ball.look = "sparks";
    ball.rate = 0.0f;
    ball.burst = 10;
    ball.shape = world::EmitterShape::sphere;
    ball.size = {2.0f, 0.0f, 0.0f};
    ball.direction = {0.0f, 1e-3f, 0.0f}; // short, but a heading
    scene.effects = {{"ball", {ball}, 0.0f, {}}};
    EffectsState fx;
    fx.follow(scene, scene.untaken_changes());
    const Quat turn = normalize(quat_from_euler({0.0f, 0.0f, 90.0f}));
    GIANT_CHECK(fx.spawn(scene, "ball", {0.0, 10.0, 0.0}, turn).has_value());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.bursts().size() == 1 && fx.bursts()[0].spread == 2.0f);
    GIANT_CHECK(std::abs(fx.bursts()[0].ground - (0.0 - 0.01)) < 1e-9);       // ground under it
    GIANT_CHECK(std::abs(length(fx.bursts()[0].direction) - 1.0f) < 1e-5f && std::abs(fx.bursts()[0].direction.y) < 1e-5f);
    GIANT_CHECK(fx.spawn(scene, "ball", {0.0, 1.0, 0.0}).has_value());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(std::abs(fx.bursts()[0].ground - (1.0 - 2.0 - 0.01)) < 1e-9); // its middle less its radius

    // An entity's effect edited (set_effect): the new one runs.
    const world::EntityId id = add_runner(scene, {}, "ball");
    fx.follow(scene, scene.take_changes());
    scene.effects[0].emitters[0].rate = 40.0f;
    scene.effects[0].emitters[0].burst = 0;
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(born(fx) == 4);
    scene.effects.insert(scene.effects.begin(), world::Effect{"other", {ball}, 0.0f, {}}); // indices move
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(born(fx) == 4);
    // Cleared: entities' effects still run (looked over again).
    fx.clear();
    fx.follow(scene, scene.take_changes());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(born(fx) == 4 && fx.stats().running == 1);
    (void)id;
}

GIANT_TEST("effects from events: a spawn's bursts scale; hits throw what their surface is made of, turned to it") {
    world::Scene scene;
    EffectsState fx;
    fx.follow(scene, scene.untaken_changes());
    GIANT_CHECK(fx.spawn(scene, "sparks", {}, {}, 1.0f, 0.5f).has_value());
    tick(fx, scene, {}, 1.0 / 60.0, k_flat);
    GIANT_CHECK(born(fx) == 20);
    GIANT_CHECK(fx.spawn(scene, "sparks", {}, {}, 1.0f, 2.0f).has_value());
    tick(fx, scene, {}, 1.0 / 60.0, k_flat);
    GIANT_CHECK(born(fx) == 80);
    GIANT_CHECK(fx.spawn(scene, "sparks", {}, {}, 1.0f, 0.0f).has_value()); // none of its bursts
    tick(fx, scene, {}, 1.0 / 60.0, k_flat);
    GIANT_CHECK(born(fx) == 0);
    GIANT_CHECK(!fx.spawn(scene, "sparks", {}, {}, 1.0f, -1.0f).has_value());
    GIANT_CHECK(!fx.spawn(scene, "sparks", {}, {}, 1.0f, std::nanf("")).has_value());
    GIANT_CHECK(!fx.spawn(scene, "sparks", {}, {}, 1.0f, world::k_max_effect_rate_scale * 2.0f).has_value());
    // Spawned by name: an edit while it runs is followed, removing it ends it.
    world::Effect own = *world::find_effect({}, "mist");
    own.name = "a very long effect name that is past what a short string holds in place";
    own.duration = 100.0f;
    scene.effects.push_back(own);
    GIANT_CHECK(fx.spawn(scene, own.name, {}).has_value());
    tick(fx, scene, {}, 1.0, k_flat);
    GIANT_CHECK(born(fx) == 8 && fx.stats().spawned == 1);
    scene.effects[0].emitters[0].rate = 16.0f;
    tick(fx, scene, {}, 1.0, k_flat);
    GIANT_CHECK(born(fx) == 16);
    scene.effects.clear();
    tick(fx, scene, {}, 1.0, k_flat);
    GIANT_CHECK(born(fx) == 0 && fx.stats().missing == 1 && fx.stats().spawned == 0);

    // Every surface's effects are built-ins; a rifle round's as built, a weak one less, never past twice.
    const world::Surface all[] = {world::Surface::standard, world::Surface::metal, world::Surface::concrete,
                                  world::Surface::brick,    world::Surface::glass, world::Surface::wood,
                                  world::Surface::foliage,  world::Surface::dirt,  world::Surface::flesh};
    for (const world::Surface s : all) {
        const HitEffect h = hit_effect(s, 3000.0f);
        if (h.effect != nullptr) GIANT_CHECK(world::find_effect({}, h.effect) != nullptr);
        if (h.second != nullptr) GIANT_CHECK(world::find_effect({}, h.second) != nullptr);
    }
    GIANT_CHECK(std::string(hit_effect(world::Surface::metal, 3000.0f).effect) == "sparks");
    GIANT_CHECK(std::string(hit_effect(world::Surface::wood, 3000.0f).effect) == "splinters");
    GIANT_CHECK(hit_effect(world::Surface::flesh, 3000.0f).effect == nullptr);
    GIANT_CHECK(hit_effect(world::Surface::metal, 3000.0f).bursts == 1.0f);
    GIANT_CHECK(hit_effect(world::Surface::metal, 10.0f).bursts == 0.3f);
    GIANT_CHECK(hit_effect(world::Surface::metal, 1.0e9f).bursts == 2.0f);
    GIANT_CHECK(hit_effect(world::Surface::metal, std::nanf("")).bursts >= 0.3f);

    // The turn takes up to the normal, any way round.
    const Vec3 normals[] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0, 0, -1}, normalize_or_zero(Vec3{1, 2, -3}), normalize_or_zero(Vec3{0.1f, -1, 0})};
    for (const Vec3& n : normals) {
        const Vec3 up = rotate(turn_up_to(n), Vec3{0, 1, 0});
        GIANT_CHECK(length(up - n) < 1e-4f);
    }
    GIANT_CHECK(turn_up_to({}) == Quat{});
}

GIANT_TEST("effects from events: shots' hits, splashes and dust thrown up spawn effects; the cap counted") {
    PhysicsState physics;
    EffectsState fx;
    EditorState state;
    state.physics = &physics;
    state.effects = &fx;
    int heard = 0;
    physics.on_hits = [&heard](std::span<const PhysicsState::ShotHit>) { ++heard; }; // the sounds', chained
    connect_event_effects(state);
    connect_event_effects(state); // once only
    fx.follow(state.scene, state.scene.untaken_changes());

    PhysicsState::ShotHit metal;
    metal.hit.point = {0.0, 1.0, 0.0};
    metal.hit.normal = {1.0f, 0.0f, 0.0f};
    metal.hit.surface = world::Surface::metal;
    metal.hit.energy = 3000.0f;
    PhysicsState::ShotHit person = metal;
    person.character = true; // flesh: nothing
    PhysicsState::ShotHit wall = metal;
    wall.hit.surface = world::Surface::concrete;
    const PhysicsState::ShotHit hits[] = {metal, person, wall};
    physics.on_hits(hits);
    GIANT_CHECK(heard == 1 && fx.events.hits == 2 && fx.stats().spawned == 0);
    tick(fx, state.scene, {}, 1.0 / 60.0, k_flat);
    // sparks (40) turned sideways, chips (24) and dust (12 at half) off the wall.
    GIANT_CHECK(born(fx) == 40 + 24 + 6);
    bool sideways = false;
    for (const render::ParticleBurst& b : fx.bursts()) sideways = sideways || std::abs(b.direction.x - 1.0f) < 1e-4f;
    GIANT_CHECK(sideways);

    sim::WaterEntry jump;
    jump.point = {5.0, 0.0, 0.0};
    jump.size = 1.0f;
    jump.speed = 4.0f;
    sim::WaterEntry boulder = jump;
    boulder.size = 4.0f;
    boulder.speed = 10.0f;
    const sim::WaterEntry in[] = {jump};
    physics.on_splashes(in);
    tick(fx, state.scene, {}, 1.0 / 60.0, k_flat);
    GIANT_CHECK(fx.events.splashes == 1 && born(fx) == 60);
    const sim::WaterEntry big[] = {boulder};
    physics.on_splashes(big);
    tick(fx, state.scene, {}, 1.0 / 60.0, k_flat);
    GIANT_CHECK(born(fx) == 240); // four times, at most

    // A crater throws up earth: dust and chips, once (the events taken).
    world::Terrain& t = state.scene.terrain;
    t.set_samples(8);
    for (int x = -1; x <= 1; ++x)
        for (int z = -1; z <= 1; ++z) t.set({x, z}, world::Terrain::Heights(64, 0.0f));
    world::Dent crater;
    crater.radius = 6.0;
    crater.depth = 2.0f;
    GIANT_CHECK(physics.dent(state.scene, crater, {10.0, 0.0, 10.0}) > 0);
    GIANT_CHECK(physics.dust().events().size() == 1 && physics.dust().events()[0].source == sim::DustSource::ground);
    // Thrown up from the crater's floor (the puff's middle is above it).
    GIANT_CHECK(physics.dust().events().size() == 1 && physics.dust().events()[0].at.y < 0.0 &&
                physics.dust().events()[0].at.y > -2.5 && physics.dust().puffs()[0].position.y > 1.0);
    event_effects_frame(state, 1.0 / 60.0);
    GIANT_CHECK(physics.dust().events().empty() && fx.events.dust == 1);
    tick(fx, state.scene, {}, 1.0 / 60.0, k_flat);
    GIANT_CHECK(born(fx) > 12);
    event_effects_frame(state, 1.0 / 60.0);
    tick(fx, state.scene, {}, 1.0 / 60.0, k_flat);
    GIANT_CHECK(born(fx) == 0 && fx.events.dust == 1);

    // The cap: what is left out is counted.
    for (u32 i = 0; i < EffectsState::k_max_spawned; ++i) (void)fx.spawn(state.scene, "mist", {});
    physics.on_hits(hits);
    GIANT_CHECK(fx.events.refused == 3 && fx.events.hits == 4);
    // Without effects: nothing breaks, the sounds still told.
    state.effects = nullptr;
    physics.on_hits(hits);
    physics.on_splashes(in);
    event_effects_frame(state, 1.0 / 60.0);
    GIANT_CHECK(heard == 3);
}

GIANT_TEST("effects from events: running on dry bare ground kicks dust each stride; walking, wet, snow or off it none") {
    sim::CharacterState p;
    p.position = {1.0, 2.0, 3.0};
    p.on_ground = true;
    p.velocity = {4.0f, -1.0f, 0.0f};
    double walked = 0.0;
    int steps = 0;
    for (int i = 0; i < 60; ++i) { // 4 m/s for a second: 4 m, two strides of 1.6
        const std::optional<DVec3> at = running_step(walked, p, world::Surface::dirt, {}, 1.0 / 60.0);
        if (at) {
            ++steps;
            GIANT_CHECK(std::abs(at->y - 2.05) < 1e-9 && at->x == 1.0);
        }
    }
    GIANT_CHECK(steps == 2 && std::abs(walked - 0.8) < 1e-6);
    // Any frame rate: the same strides.
    walked = 0.0;
    steps = 0;
    for (int i = 0; i < 4; ++i) steps += running_step(walked, p, world::Surface::dirt, {}, 0.25).has_value() ? 1 : 0;
    GIANT_CHECK(steps == 2);
    const auto none = [&](const sim::CharacterState& c, const world::Ground& g,
                          world::Surface under = world::Surface::dirt) {
        double w = 1.5;
        for (int i = 0; i < 60; ++i)
            if (running_step(w, c, under, g, 1.0 / 60.0)) return false;
        return true;
    };
    sim::CharacterState walking = p;
    walking.velocity = {2.0f, 0.0f, 0.0f};
    GIANT_CHECK(none(walking, {}));
    GIANT_CHECK(none(p, {0.5f, 0.0f}));
    GIANT_CHECK(none(p, {0.0f, 0.5f}));
    sim::CharacterState air = p;
    air.on_ground = false;
    double w = 1.5;
    GIANT_CHECK(!running_step(w, air, world::Surface::dirt, {}, 1.0 / 60.0) && w == 0.0);
    sim::CharacterState wading = p;
    wading.water_depth = 0.3f;
    GIANT_CHECK(none(wading, {}));
    GIANT_CHECK(none(p, {}, world::Surface::concrete)); // a floor, a deck: no dust
    GIANT_CHECK(none(p, {}, world::Surface::wood));
    sim::CharacterState carried = p;
    carried.carrier = 7; // on a platform that moves it: not running
    GIANT_CHECK(none(carried, {}));
    // Bad numbers never leave it kicking every frame.
    double bad = 1.0;
    GIANT_CHECK(!running_step(bad, p, world::Surface::dirt, {}, std::numeric_limits<double>::infinity()));
    bad = std::numeric_limits<double>::infinity();
    GIANT_CHECK(!running_step(bad, p, world::Surface::dirt, {}, 1.0 / 60.0) && bad == 0.0);
    sim::CharacterState flying = p;
    flying.velocity.x = std::numeric_limits<float>::infinity();
    bad = 0.0;
    GIANT_CHECK(!running_step(bad, flying, world::Surface::dirt, {}, 1.0 / 60.0) && bad == 0.0);
    GIANT_CHECK(!running_step(w, p, world::Surface::dirt, {}, 0.0) && !running_step(w, p, world::Surface::dirt, {}, std::nan("")));
}

// --- Effect lights (effects plan FX4a) -----------------------------------------

GIANT_TEST("effect lights: a fire glows where it stands, flickering with the clock; paused it stays; off, none") {
    world::Scene scene;
    world::Entity e;
    e.transform.position = {4.0, 1.0, -2.0};
    e.transform.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 1.5707963f); // turned: the offset turns with it
    e.effect = {true, "fire", 1.0f};
    const world::EntityId id = scene.add(e);
    world::Effect lit{"lit", {}, 0.0f, {}};
    lit.emitters.push_back(world::find_effect({}, "smoke")->emitters[0]);
    lit.light.on = true;
    lit.light.offset = {1.0f, 0.0f, 0.0f};
    lit.light.flicker = 0.0f;
    lit.light.shadows = true;
    scene.effects.push_back(lit);
    EffectsState fx;
    fx.follow(scene, scene.untaken_changes());
    tick(fx, scene, {}, 0.1, k_flat);
    const world::EffectLight& glow = world::find_effect({}, "fire")->light;
    GIANT_CHECK(fx.lights().size() == 1 && fx.stats().lights == 1);
    render::LocalLight l = fx.lights()[0];
    GIANT_CHECK(std::abs(l.position.x - 4.0) < 1e-5 && std::abs(l.position.y - (1.0 + glow.offset.y)) < 1e-5);
    GIANT_CHECK(l.color == glow.color && l.range == glow.range && l.shadow_id == 0 && l.kind == render::LightKind::point);
    GIANT_CHECK(l.intensity <= glow.intensity && l.intensity >= glow.intensity * (1.0f - glow.flicker) - 1e-5f);
    GIANT_CHECK(l.intensity == glow.intensity * world::effect_flicker(glow.flicker, fx.clock(), id));
    // It wavers as the clock runs; paused, it stands (and stays).
    bool changed = false;
    for (int i = 0; i < 20; ++i) {
        tick(fx, scene, {}, 0.05, k_flat);
        changed = changed || fx.lights()[0].intensity != l.intensity;
    }
    GIANT_CHECK(changed);
    l = fx.lights()[0];
    const double clock = fx.clock();
    tick(fx, scene, {}, 0.0, k_flat);
    GIANT_CHECK(fx.lights().size() == 1 && fx.lights()[0].intensity == l.intensity && fx.clock() == clock);
    // Off, hidden, at rate 0 or past reach: none.
    scene.modify(id)->effect.on = false;
    fx.follow(scene, scene.take_changes());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.lights().empty());
    scene.modify(id)->effect.on = true;
    scene.modify(id)->effect.rate = 0.0f;
    fx.follow(scene, scene.take_changes());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.lights().empty());
    scene.modify(id)->effect.rate = 1.0f;
    fx.follow(scene, scene.take_changes());
    fx.reach = 2.0;
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.lights().empty());
    fx.reach = EffectsState::k_default_reach;
    // The scene's own: turned with the entity, steady, casting, its shadow kept by the entity.
    scene.modify(id)->effect.effect = "lit";
    fx.follow(scene, scene.take_changes());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.lights().size() == 1 && fx.lights()[0].intensity == lit.light.intensity);
    l = fx.lights()[0];
    GIANT_CHECK(std::abs(l.position.x - 4.0) < 1e-5 && std::abs(l.position.z - (-3.0)) < 1e-5); // +X turned to -Z
    GIANT_CHECK(l.shadow_id != 0);
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.lights()[0].shadow_id == l.shadow_id);
}

GIANT_TEST("effect lights: spawned ones glow while they run; the nearest 256 given, the rest counted") {
    world::Scene scene;
    EffectsState fx;
    // A fire spawned runs its duration (10 s), then its light goes with it.
    GIANT_CHECK(fx.spawn(scene, "fire", {0.0, 0.0, 0.0}).has_value());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.lights().size() == 1);
    for (int i = 0; i < 12; ++i) tick(fx, scene, {}, 1.0, k_flat);
    GIANT_CHECK(fx.lights().empty() && fx.stats().spawned == 0);
    // 300 fires on entities in a row: the nearest 256 lit, nearest first.
    for (int i = 0; i < 300; ++i) add_runner(scene, {static_cast<double>(i), 0.0, 0.0}, "fire");
    fx.follow(scene, scene.untaken_changes());
    tick(fx, scene, {-0.5, 0.0, 0.0}, 0.1, k_flat);
    GIANT_CHECK(fx.lights().size() == EffectsState::k_max_effect_lights && fx.stats().lights_cut == 300 - EffectsState::k_max_effect_lights);
    bool nearest = true;
    for (usize i = 0; i < fx.lights().size(); ++i) nearest = nearest && std::abs(fx.lights()[i].position.x - static_cast<double>(i)) < 1e-6;
    GIANT_CHECK(nearest);
    // Cleared: none until the next update follows them again.
    fx.clear();
    GIANT_CHECK(fx.lights().empty());
    // Bursts only (duration 0, or rate 0): a flash of k_min_glow on the game clock, whatever the frame rate.
    world::Scene flashes;
    world::Effect flash{"flash", {}, 0.0f, {}};
    flash.emitters.push_back(world::find_effect({}, "sparks")->emitters[0]);
    flash.light.on = true;
    flash.light.flicker = 0.0f;
    flashes.effects.push_back(flash);
    EffectsState fy;
    GIANT_CHECK(fy.spawn(flashes, "flash", {}).has_value());
    int lit_frames = 0;
    for (int i = 0; i < 100; ++i) { // 1 s at 100 frames a second
        tick(fy, flashes, {}, 0.01, k_flat);
        lit_frames += fy.lights().empty() ? 0 : 1;
    }
    GIANT_CHECK(lit_frames >= 24 && lit_frames <= 26 && fy.stats().spawned == 0);
    GIANT_CHECK(fy.spawn(flashes, "fire", {}, {}, 0.0f).has_value()); // rate 0: its bursts (none) and a flash
    tick(fy, flashes, {}, 0.01, k_flat);
    GIANT_CHECK(fy.lights().size() == 1 && fy.lights()[0].intensity > 0.0f);
    for (int i = 0; i < 30; ++i) tick(fy, flashes, {}, 0.01, k_flat);
    GIANT_CHECK(fy.lights().empty());
    // Lit while it runs: followed (an unlit spawn of 10 s glows once its effect's light is on).
    world::Effect later{"later", {}, 10.0f, {}};
    later.emitters.push_back(world::find_effect({}, "smoke")->emitters[0]);
    flashes.effects.push_back(later);
    GIANT_CHECK(fy.spawn(flashes, "later", {}).has_value());
    tick(fy, flashes, {}, 0.1, k_flat);
    GIANT_CHECK(fy.lights().empty());
    flashes.effects.back().light.on = true;
    tick(fy, flashes, {}, 0.1, k_flat);
    GIANT_CHECK(fy.lights().size() == 1);
    // Another scene: the old one's lights go at once (before any update).
    world::Scene other;
    fy.follow(other, other.untaken_changes());
    GIANT_CHECK(fy.lights().empty() && fy.stats().lights == 0);
}

GIANT_TEST("effect lights: a fire run at a fraction of its rate glows as much less; casting, ids of their own") {
    world::Scene scene;
    world::Effect lit{"lit", {}, 0.0f, {}};
    lit.emitters.push_back(world::find_effect({}, "smoke")->emitters[0]);
    lit.light.on = true;
    lit.light.flicker = 0.0f;
    lit.light.shadows = true;
    scene.effects.push_back(lit);
    const world::EntityId half = add_runner(scene, {1.0, 0.0, 0.0}, "lit", 0.5f);
    add_runner(scene, {2.0, 0.0, 0.0}, "lit", 3.0f);
    EffectsState fx;
    fx.follow(scene, scene.untaken_changes());
    tick(fx, scene, {}, 0.1, k_flat);
    GIANT_CHECK(fx.lights().size() == 2);
    GIANT_CHECK(fx.lights()[0].intensity == 0.5f * lit.light.intensity); // (nearest first)
    GIANT_CHECK(fx.lights()[1].intensity == lit.light.intensity);        // (over 1: no brighter)
    GIANT_CHECK((fx.lights()[0].shadow_id & 0x80000000u) != 0 && (fx.lights()[1].shadow_id & 0x80000000u) != 0);
    GIANT_CHECK(fx.lights()[0].shadow_id != fx.lights()[1].shadow_id);
    (void)half;
}

GIANT_TEST("effect lights: set_effect sets the light's fields given, refuses bad ones; get_effects counts them") {
    Fixture f;
    ai::CommandResult r = f.run("create_effect", {{"name", "torch"}, {"emitters", Json::array({{{"look", "fire"}}})},
                                                   {"light", {{"on", true}, {"intensity", 6}, {"flicker", 0.5}}}});
    GIANT_CHECK(r.ok);
    const world::Effect& torch = f.state.scene.effects.back();
    GIANT_CHECK(torch.light.on && torch.light.intensity == 6.0f && torch.light.flicker == 0.5f && torch.light.range == 8.0f);
    r = f.run("set_effect", {{"name", "torch"}, {"light", {{"range", 3}, {"shadows", true}}}});
    GIANT_CHECK(r.ok && r.value["light"]["range"] == 3.0f && r.value["light"]["intensity"] == 6.0f &&
                r.value["light"]["shadows"] == true);
    GIANT_CHECK(!f.run("set_effect", {{"name", "torch"}, {"light", {{"flicker", 2}}}}).ok);
    GIANT_CHECK(f.state.scene.effects.back().light.flicker == 0.5f);
    r = f.run("get_effects");
    GIANT_CHECK(r.ok && r.value["effects"][0]["light"]["on"] == true);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.effects.back().light.range == 8.0f &&
                !f.state.scene.effects.back().light.shadows);
    // A scene's own "fire" stands in for the built-in one: its glow kept unless set.
    r = f.run("create_effect", {{"name", "fire"}, {"emitters", Json::array({{{"look", "fire"}}})}});
    GIANT_CHECK(r.ok && r.value["light"]["on"] == true && f.state.scene.effects.back().light == world::find_effect({}, "fire")->light);
    r = f.run("create_effect", {{"name", "smoke"}, {"emitters", Json::array({{{"look", "smoke"}}})}});
    GIANT_CHECK(r.ok && r.value["light"]["on"] == false);
}
