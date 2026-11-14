// Animation commands: the clips of skinned files, what a skinned entity
// plays (saved and undoable: set_animation) and playing them in the view
// (play_animation, blend_animations, step_animation: view state, as the
// physics' play button).
#include "animation_state.h"
#include "editor_commands.h"
#include "mesh_library.h"

#include "giant/core/assert.h"

#include <cmath>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "animation command could not be registered");
}

CommandResult no_animation() { return CommandResult::failure("animation is not available"); }

Json id_schema() { return {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991.0}}; }
Json clip_schema(const char* description) {
    return {{"type", "string"}, {"maxLength", world::k_max_name_bytes}, {"description", description}};
}
Json speed_schema() {
    return {{"type", "number"},
            {"minimum", -world::k_max_animation_speed},
            {"maximum", world::k_max_animation_speed},
            {"description", "1 normal speed (the default), 2 twice as fast, negative plays it backwards."}};
}

// The skinned file an entity draws (imported), or an error.
const MeshLibrary::Mesh* skinned_of(const EditorState& state, world::EntityId id, std::string& error) {
    const world::Entity* e = state.scene.find(id);
    if (e == nullptr) {
        error = "no entity with id " + std::to_string(id) + "; list_entities shows them";
        return nullptr;
    }
    if (e->shape != world::Shape::mesh) {
        error = "entity " + std::to_string(id) + " is not a mesh";
        return nullptr;
    }
    const MeshLibrary::Mesh* mesh = state.meshes != nullptr ? state.meshes->find_normalized(e->mesh) : nullptr;
    if (mesh == nullptr) {
        error = e->mesh + " is not imported (import_mesh)";
        return nullptr;
    }
    if (mesh->skinned == nullptr) {
        error = e->mesh + " is not skinned: it has no animation";
        return nullptr;
    }
    return mesh;
}

Json clips_json(const MeshLibrary::Skinned& s) {
    Json clips = Json::array();
    for (const anim::Clip& c : s.clips) {
        clips.push_back({{"name", c.name}, {"seconds", c.duration}, {"channels", c.channels.size()}});
    }
    return clips;
}

Json clip_state_json(const MeshLibrary::Skinned& s, const anim::ClipState& c) {
    if (c.clip == anim::k_no_clip || c.clip >= s.clips.size()) return nullptr;
    return {{"clip", s.clips[c.clip].name}, {"time", c.time}, {"loop", c.loop}};
}

Json animation_json(const EditorState& state, world::EntityId id) {
    const world::Entity* e = state.scene.find(id);
    Json out{{"id", id}};
    if (e == nullptr) return out;
    out["saved"] = {{"clip", e->animation.clip}, {"speed", e->animation.speed}, {"loop", e->animation.loop}};
    const AnimationState::Animated* a = state.animation->find(id);
    out["animated"] = a != nullptr;
    if (a == nullptr) {
        // Why not: frozen past the limit, waiting to be copied, or nothing to play.
        std::string error;
        const MeshLibrary::Mesh* mesh = skinned_of(state, id, error);
        if (mesh == nullptr) {
            out["why"] = error;
        } else if (const std::string refused = state.animation->refused(id); !refused.empty()) {
            out["why"] = refused;
        } else if (e->animation.clip.empty()) {
            out["why"] = "nothing to play: it stands in its rest pose";
        } else if (mesh->skinned->find_clip(e->animation.clip) == anim::k_no_clip) {
            out["why"] = mesh->path + " has no clip named \"" + e->animation.clip + "\": it stands in its rest pose";
        } else if (!e->visible) {
            out["why"] = "hidden";
        } else {
            out["why"] = "waiting for a place (at most " + std::to_string(state.animation->max_animated) +
                         " animate at once): it stands in its rest pose";
        }
        return out;
    }
    const MeshLibrary::Skinned& s = *a->skinned;
    out["by_command"] = a->by_command;
    out["main"] = clip_state_json(s, a->player.main());
    out["other"] = clip_state_json(s, a->player.other());
    out["other_weight"] = a->player.other_weight();
    out["blending"] = a->player.blending();
    out["fading"] = a->player.fading();
    out["synced"] = a->player.synced();
    out["speed"] = a->player.speed();
    out["drawn"] = a->drawn; // its own posed copy drawn (loaded), not the rest pose
    out["poses"] = a->poses;
    out["pixels"] = a->pixels;               // its size on screen (pose sphere radius)
    out["pose_interval"] = a->interval;      // seconds between poses (0: every frame)
    out["bounds"] = {{"center", Json::array({a->bounds.center.x, a->bounds.center.y, a->bounds.center.z})},
                     {"radius", a->bounds.radius}};
    return out;
}

Json summary_json(const AnimationState& a) {
    return {{"animated", a.animated()},
            {"waiting", a.waiting()},
            {"frozen", a.frozen()},
            {"paused", a.paused()},
            {"poses", a.poses()},
            {"max_animated", a.max_animated},
            {"posed_last_frame", a.posed_last()},
            {"overdue", a.overdue()},
            {"poses_per_frame", a.poses_per_frame}};
}

} // namespace

void register_animation_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"list_animations",
         "Lists a skinned glTF/GLB file's animation clips (name, seconds long, joint channels) and its "
         "skeleton's joints: by `path` (imported first if need be) or by the mesh entity `id` draws. "
         "A file that is not skinned has none.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_mesh_path_bytes}}},
                {"id", id_schema()}}}},
         false, // may import
         [&state](const Json& args, CommandContext&) {
             if (state.meshes == nullptr) return CommandResult::failure("meshes are not available");
             const MeshLibrary::Mesh* mesh = nullptr;
             if (args.contains("path") == args.contains("id")) {
                 return CommandResult::failure("give one of path or id");
             }
             if (args.contains("path")) {
                 const MeshLibrary::Loaded loaded = state.meshes->load(args["path"].get<std::string>());
                 if (loaded.mesh == nullptr) return CommandResult::failure(loaded.error);
                 mesh = loaded.mesh;
             } else {
                 const world::Entity* e = state.scene.find(args["id"].get<world::EntityId>());
                 if (e == nullptr) return CommandResult::failure("no such entity; list_entities shows them");
                 if (e->mesh.empty()) return CommandResult::failure("the entity names no mesh file");
                 const MeshLibrary::Loaded loaded = state.meshes->load(e->mesh);
                 if (loaded.mesh == nullptr) return CommandResult::failure(loaded.error);
                 mesh = loaded.mesh;
             }
             Json out{{"path", mesh->path}, {"skinned", mesh->skinned != nullptr}};
             if (mesh->skinned == nullptr) {
                 out["clips"] = Json::array();
                 out["joints"] = 0;
                 return CommandResult::success(out);
             }
             out["clips"] = clips_json(*mesh->skinned);
             out["joints"] = mesh->skinned->skeleton.size();
             out["joint_names"] = mesh->skinned->skeleton.names;
             return CommandResult::success(out);
         }});

    add(registry,
        {"set_animation",
         "Sets the animation a skinned mesh entity plays, saved with the scene: a clip of its file "
         "(list_animations), looping or once, at a speed. An empty clip stands it in its rest pose (on an "
         "entity that cannot animate it clears the field). It starts playing at once, even when that is "
         "already what is saved (fading from what showed, if it was animated), replacing whatever "
         "play_animation or blend_animations played. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"id", "clip"}},
              {"properties",
               {{"id", id_schema()},
                {"clip", clip_schema("The clip's name; empty for none.")},
                {"speed", speed_schema()},
                {"loop", {{"type", "boolean"}, {"description", "Default true; false plays it once and holds the end."}}}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             const world::EntityId id = args["id"].get<world::EntityId>();
             const world::Entity* found = state.scene.find(id);
             if (found == nullptr) {
                 return CommandResult::failure("no entity with id " + std::to_string(id) + "; list_entities shows them");
             }
             world::EntityAnimation after = found->animation;
             after.clip = args["clip"].get<std::string>();
             if (args.contains("speed")) after.speed = args["speed"].get<float>();
             if (args.contains("loop")) after.loop = args["loop"].get<bool>();
             std::string error;
             const MeshLibrary::Mesh* mesh = skinned_of(state, id, error);
             if (after.clip.empty() && mesh == nullptr) {
                 after = {}; // clearing what an entity that cannot animate has: back to none at all
             } else if (!after.clip.empty()) {
                 if (mesh == nullptr) return CommandResult::failure(error);
                 if (mesh->skinned->find_clip(after.clip) == anim::k_no_clip) {
                     return CommandResult::failure(mesh->path + " has no clip named \"" + after.clip +
                                                   "\"; list_animations shows them");
                 }
             }
             const world::EntityAnimation before = found->animation;
             const auto set = [&state, id](const world::EntityAnimation& value) {
                 if (world::Entity* target = state.scene.modify(id)) target->animation = value;
             };
             set(after);
             context.record(scene_change(state, [set, before] { set(before); }, [set, after] { set(after); }));
             if (state.animation != nullptr && state.meshes != nullptr) {
                 state.animation->follow_now(state.scene, *state.meshes, state.scene.untaken_changes());
                 state.animation->restart_saved(state.scene, id); // even when the value was already saved
                 state.animation->follow_now(state.scene, *state.meshes, world::SceneChanges{{id}, false});
                 return CommandResult::success(animation_json(state, id));
             }
             return CommandResult::success(Json{{"id", id}});
         }});

    add(registry,
        {"play_animation",
         "Plays a clip on a skinned mesh entity now, in the view: from `start` seconds, fading from what "
         "showed over `blend_seconds` (0 cuts at once), looping or once, at a speed. View state, not "
         "saved or undoable (set_animation saves one): it plays until set_animation is used on the "
         "entity, the entity is hidden or draws another file (or its file is imported again), or the "
         "scene is replaced. At most 64 entities animate at once. Returns get_animation's report.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"id", "clip"}},
              {"properties",
               {{"id", id_schema()},
                {"clip", clip_schema("The clip's name (list_animations).")},
                {"blend_seconds", {{"type", "number"}, {"minimum", 0}, {"maximum", 60}, {"description", "Default 0.2."}}},
                {"loop", {{"type", "boolean"}, {"description", "Default true."}}},
                {"speed", speed_schema()},
                {"start", {{"type", "number"}, {"minimum", 0}, {"maximum", anim::k_max_clip_seconds},
                           {"description", "Seconds into the clip. Default its start (its end when played once "
                                           "backwards)."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.animation == nullptr || state.meshes == nullptr) return no_animation();
             const world::EntityId id = args["id"].get<world::EntityId>();
             state.animation->follow_now(state.scene, *state.meshes, state.scene.untaken_changes());
             const std::string error = state.animation->play(
                 state.scene, *state.meshes, id, args["clip"].get<std::string>(), args.value("blend_seconds", 0.2f),
                 args.value("loop", true), args.value("speed", 1.0f), args.value("start", -1.0f));
             if (!error.empty()) return CommandResult::failure(error);
             return CommandResult::success(animation_json(state, id));
         }});

    add(registry,
        {"blend_animations",
         "Holds two clips of a skinned mesh entity blended, in the view: `weight` 0 is all of `a`, 1 all "
         "of `b` (a walk and a run by speed). Both loop; with `sync` (the default) b's time follows a's at "
         "the same fraction of its length, so feet stay in step. Call again to change the weight. View "
         "state, as play_animation. Returns get_animation's report.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"id", "a", "b", "weight"}},
              {"properties",
               {{"id", id_schema()},
                {"a", clip_schema("A clip's name.")},
                {"b", clip_schema("Another (or the same) clip's name.")},
                {"weight", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                {"sync", {{"type", "boolean"}, {"description", "Default true."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.animation == nullptr || state.meshes == nullptr) return no_animation();
             const world::EntityId id = args["id"].get<world::EntityId>();
             state.animation->follow_now(state.scene, *state.meshes, state.scene.untaken_changes());
             const std::string error = state.animation->blend(state.scene, *state.meshes, id, args["a"].get<std::string>(),
                                                              args["b"].get<std::string>(), args["weight"].get<float>(),
                                                              args.value("sync", true));
             if (!error.empty()) return CommandResult::failure(error);
             return CommandResult::success(animation_json(state, id));
         }});

    add(registry,
        {"step_animation",
         "Pauses or resumes every animation (`paused`), and moves every animated entity's time on by "
         "`seconds` now, paused or not (for checking a pose, or a screenshot at an exact moment). View "
         "state. Returns how many animate, wait for a place or stand frozen past the limit.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"paused", {{"type", "boolean"}}},
                {"seconds", {{"type", "number"}, {"minimum", 0}, {"maximum", 3600}, {"description", "Default 0."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.animation == nullptr || state.meshes == nullptr) return no_animation();
             if (args.contains("paused")) state.animation->set_paused(args["paused"].get<bool>());
             state.animation->follow_now(state.scene, *state.meshes, state.scene.untaken_changes());
             state.animation->step(args.value("seconds", 0.0));
             return CommandResult::success(summary_json(*state.animation));
         }});

    add(registry,
        {"get_animation",
         "Reports animation: with `id`, that entity's saved animation and what plays now (the main clip "
         "and its time, the clip fading out or held blended and its weight, whether its own posed copy "
         "is drawn yet, the pose's bounding sphere; or why it is not animated); without, how many "
         "entities animate, wait for a place or stand frozen past the limit, and whether paused.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", {{"id", id_schema()}}}},
         false, // follows the scene's latest edits first (a scene just loaded)
         [&state](const Json& args, CommandContext&) {
             if (state.animation == nullptr || state.meshes == nullptr) return no_animation();
             state.animation->follow_now(state.scene, *state.meshes, state.scene.untaken_changes());
             if (!args.contains("id")) return CommandResult::success(summary_json(*state.animation));
             const world::EntityId id = args["id"].get<world::EntityId>();
             if (state.scene.find(id) == nullptr) {
                 return CommandResult::failure("no entity with id " + std::to_string(id) + "; list_entities shows them");
             }
             return CommandResult::success(animation_json(state, id));
         }});
}

} // namespace giant::editor
