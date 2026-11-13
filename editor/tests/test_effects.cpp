// Effects in the editor: particle looks and effects made, changed, renamed
// and removed by command (undoable, saved with the scene), and entities
// given an effect to run.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"

#include "giant_test.h"

#include "giant/world/effects.h"

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
    GIANT_CHECK(r.value["built_in"]["effects"].size() == 8 && !r.value["built_in"]["effects"][0].contains("emitters"));
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
