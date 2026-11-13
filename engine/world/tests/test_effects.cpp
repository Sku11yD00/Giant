// Effects as world data: particle looks, effects of emitters, an entity's
// effect; checked, found by name (the scene's before the built-in ones),
// saved and loaded with the scene and the world.
#include "giant_test.h"

#include "giant/world/effects.h"
#include "giant/world/scene.h"

#include <cmath>
#include <limits>
#include <set>
#include <string>

using namespace giant;
using namespace giant::world;

namespace {

ParticleLook steam() {
    ParticleLook l;
    l.name = "steam";
    l.life_min = 2.0f;
    l.life_max = 3.5f;
    l.color_start = {0.9f, 0.9f, 0.92f};
    l.emissive = {0.0f, 0.1f, 0.0f};
    l.buoyancy = 1.25f;
    l.drag = 0.5f;
    l.form = ParticleForm::streak;
    l.lit = false;
    return l;
}

Effect vent() {
    Emitter a;
    a.look = "steam";
    a.rate = 12.5f;
    a.shape = EmitterShape::box;
    a.size = {1.0f, 0.25f, 0.5f};
    a.offset = {0.0f, 2.0f, 0.0f};
    a.direction = {0.0f, 1.0f, 0.5f};
    a.cone_degrees = 12.0f;
    a.speed_min = 0.5f;
    a.speed_max = 1.5f;
    a.size_scale = 2.0f;
    a.ground = false;
    Emitter b;
    b.look = "sparks"; // built-in
    b.rate = 0.0f;
    b.burst = 30;
    b.shape = EmitterShape::disc;
    b.size = {0.4f, 0.0f, 0.0f};
    return {"vent", {a, b}, 4.0f};
}

} // namespace

GIANT_TEST("effects: the built-in library is whole, valid, and every look its effects name is there") {
    const std::set<std::string> names = {"smoke", "fire", "sparks", "dust_kick", "spray", "mist", "embers", "debris_chips"};
    std::set<std::string> looks, effects;
    for (const ParticleLook& l : built_in_particle_looks()) {
        GIANT_CHECK(!check_particle_look(l));
        looks.insert(l.name);
    }
    for (const Effect& f : built_in_effects()) {
        GIANT_CHECK(!check_effect(f));
        effects.insert(f.name);
        for (const Emitter& e : f.emitters) GIANT_CHECK(find_particle_look({}, e.look) != nullptr);
    }
    GIANT_CHECK(looks == names && effects == names);
    GIANT_CHECK(built_in_particle_looks().size() == names.size() && built_in_effects().size() == names.size());
    GIANT_CHECK(built_in_particle_looks().size() + k_max_particle_looks <= 256); // the renderer's styles
}

GIANT_TEST("effects: the built-in library saved as a scene's own reads back the same, at the bounds too") {
    Scene scene;
    scene.particle_looks.assign(built_in_particle_looks().begin(), built_in_particle_looks().end());
    scene.effects.assign(built_in_effects().begin(), built_in_effects().end());
    ParticleLook edge = steam();
    edge.name = "edge";
    edge.buoyancy = k_max_look_buoyancy;
    edge.gravity = -k_max_look_pull;
    edge.emissive = {k_max_look_light, 0.0f, 0.0f};
    edge.life_max = k_max_look_life;
    GIANT_CHECK(!check_particle_look(edge));
    scene.particle_looks.push_back(edge);
    Scene loaded;
    const auto error = load_scene(save_scene(scene), loaded);
    GIANT_CHECK(!error.has_value());
    GIANT_CHECK(loaded.particle_looks == scene.particle_looks && loaded.effects == scene.effects);
}

GIANT_TEST("effects: the scene's own are found before the built-in ones of the same name") {
    ParticleLook mine = *find_particle_look({}, "smoke");
    mine.color_start = {0.1f, 0.1f, 0.1f};
    const std::vector<ParticleLook> looks = {mine, steam()};
    GIANT_CHECK(find_particle_look(looks, "smoke") == &looks[0]);
    GIANT_CHECK(find_particle_look(looks, "steam") == &looks[1]);
    GIANT_CHECK(find_particle_look(looks, "fire") == &built_in_particle_looks()[1]);
    GIANT_CHECK(find_particle_look(looks, "nothing") == nullptr);
    const std::vector<Effect> effects = {vent()};
    GIANT_CHECK(find_effect(effects, "vent") == &effects[0]);
    GIANT_CHECK(find_effect(effects, "mist") != nullptr && find_effect(effects, "mist")->name == "mist");
    GIANT_CHECK(find_effect(effects, "") == nullptr);
}

GIANT_TEST("effects: bad looks, emitters, effects and entity effects are refused with what is wrong") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    GIANT_CHECK(!check_particle_look(steam()));
    const auto bad_look = [](auto change) {
        ParticleLook l = steam();
        change(l);
        return check_particle_look(l).has_value();
    };
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.name.clear(); }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.name = std::string(k_max_effect_name_bytes + 1, 'a'); }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.life_min = 0.0f; }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.life_max = 1.0f; })); // under life_min
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.life_max = 61.0f; }));
    GIANT_CHECK(bad_look([&](ParticleLook& l) { l.size_end = nan; }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.color_end.y = 1.5f; }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.opacity_start = -0.1f; }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.emissive.x = 2.0e4f; }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.gravity = 101.0f; }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.buoyancy = 1000.0f; }));
    GIANT_CHECK(!bad_look([](ParticleLook& l) { l.buoyancy = -k_max_look_buoyancy; }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.drag = -1.0f; }));
    GIANT_CHECK(bad_look([](ParticleLook& l) { l.form = static_cast<ParticleForm>(7); }));
    GIANT_CHECK(!bad_look([](ParticleLook& l) { l.gravity = -2.0f; })); // falling up is allowed

    GIANT_CHECK(!check_effect(vent()));
    const auto bad_effect = [](auto change) {
        Effect f = vent();
        change(f);
        return check_effect(f);
    };
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters.clear(); }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters.resize(k_max_emitters + 1, f.emitters[0]); }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.duration = -1.0f; }).has_value());
    const auto msg = bad_effect([](Effect& f) { f.emitters[1].speed_max = 0.5f; }); // under speed_min
    GIANT_CHECK(msg.has_value() && msg->rfind("emitters[1].", 0) == 0);
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].direction = {}; }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].burst = k_max_emitter_burst + 1; }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].rate = 2.0e4f; }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].cone_degrees = 181.0f; }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].size.x = -1.0f; }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].offset.z = 501.0f; }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].size.y = 501.0f; }).has_value()); // a box's corner within 1 km
    GIANT_CHECK(!bad_effect([](Effect& f) { f.emitters[0].size = {k_max_emitter_reach, k_max_emitter_reach, k_max_emitter_reach}; }));
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].size_scale = 0.0f; }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].look.clear(); }).has_value());
    GIANT_CHECK(bad_effect([](Effect& f) { f.emitters[0].shape = static_cast<EmitterShape>(9); }).has_value());

    GIANT_CHECK(!check_entity_effect({}));
    GIANT_CHECK(check_entity_effect({true, "", 1.0f}).has_value());
    GIANT_CHECK(!check_entity_effect({true, "smoke", 2.0f}));
    GIANT_CHECK(check_entity_effect({false, "smoke", -1.0f}).has_value());
    GIANT_CHECK(check_entity_effect({false, std::string(k_max_effect_name_bytes + 1, 'x'), 1.0f}).has_value());
}

GIANT_TEST("effects: saved and loaded with the scene and the world, refused when bad or named twice") {
    Scene scene;
    scene.particle_looks = {steam()};
    scene.effects = {vent()};
    Entity chimney;
    chimney.name = "chimney";
    chimney.effect = {true, "smoke", 1.5f};
    const EntityId id = scene.add(chimney);
    Entity plain;
    plain.name = "plain"; // no effect: none written
    scene.add(plain);
    const std::string text = save_scene(scene);
    GIANT_CHECK(text.find("\"particle_looks\"") != std::string::npos && text.find("\"effects\"") != std::string::npos);
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.particle_looks == scene.particle_looks && loaded.effects == scene.effects);
    GIANT_CHECK(loaded.find(id) != nullptr && loaded.find(id)->effect == scene.find(id)->effect);
    GIANT_CHECK(save_scene(loaded) == text);
    // A scene without any writes none (older scenes read and write the same).
    GIANT_CHECK(save_scene(Scene{}).find("effect") == std::string::npos);
    // Copied and moved with the scene.
    Scene copy = scene;
    GIANT_CHECK(copy.effects == scene.effects && copy.particle_looks == scene.particle_looks);
    Scene moved = std::move(copy);
    GIANT_CHECK(moved.effects == scene.effects && moved.particle_looks == scene.particle_looks);

    WorldIndex index;
    index.particle_looks = scene.particle_looks;
    index.effects = scene.effects;
    WorldIndex back;
    GIANT_CHECK(!load_world_index(save_world_index(index), back).has_value());
    GIANT_CHECK(back.particle_looks == scene.particle_looks && back.effects == scene.effects);

    // Named twice, a bad number, an unknown key, an effect with no emitters, a bad entity effect.
    const auto refused = [&](auto change) {
        Scene s = scene;
        change(s);
        Scene out;
        out.effects = {vent()};
        const auto error = load_scene(save_scene(s), out);
        // (`out` untouched on failure)
        return error.has_value() && out.effects.size() == 1 && out.particle_looks.empty();
    };
    GIANT_CHECK(refused([](Scene& s) { s.particle_looks.push_back(s.particle_looks[0]); }));
    GIANT_CHECK(refused([](Scene& s) { s.effects.push_back(s.effects[0]); }));
    GIANT_CHECK(refused([](Scene& s) { s.particle_looks[0].life_max = 0.5f; })); // under life_min
    GIANT_CHECK(refused([](Scene& s) { s.effects[0].emitters[0].speed_min = 5.0f; }));
    GIANT_CHECK(refused([&](Scene& s) { s.modify(id)->effect = {true, "", 1.0f}; }));
    Scene out;
    // An unknown key anywhere in them: refused.
    for (const char* key : {"\"cone_degrees\"", "\"wobble\"", "\"duration\"", "\"rate\": 1.5"}) {
        std::string typo = text;
        const usize at = typo.find(key);
        GIANT_CHECK(at != std::string::npos);
        typo.insert(at + 1, "x");
        GIANT_CHECK(load_scene(typo, out).has_value());
    }
    std::string empty = text;
    const usize at = empty.find("\"emitters\": [");
    GIANT_CHECK(at != std::string::npos);
    const usize end = empty.find("\n      ]", at);
    GIANT_CHECK(end != std::string::npos);
    empty.replace(at, end + 8 - at, "\"emitters\": []");
    const auto error = load_scene(empty, out);
    GIANT_CHECK(error.has_value() && error->message.find("emitters") != std::string::npos);
}
