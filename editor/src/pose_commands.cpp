// Baked poses' commands (pose_bakes.h): a character held in one pose as a
// small plain mesh, what a crowd's distant people are drawn with. View state.
#include "editor_commands.h"
#include "pose_bakes.h"

#include "giant/anim/skeleton.h"
#include "giant/core/assert.h"

#include <cmath>
#include <string>
#include <utility>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

double rounded(float value, double to) { return std::round(static_cast<double>(value) * to) / to; }

Json pose_json(const PoseBakes::Pose& p, usize index) {
    Json out = {{"pose", index},
                {"mesh", p.mesh},
                {"clip", p.clip},
                {"time", rounded(p.time, 1000.0)},
                {"triangles", p.triangles},
                {"state", PoseBakes::to_string(p.state)}};
    if (p.state == PoseBakes::State::failed) out["problem"] = p.problem;
    if (p.state == PoseBakes::State::ready) {
        out["parts"] = p.meshes.size();
        out["baked_triangles"] = p.baked_triangles;
        out["source_triangles"] = p.source_triangles;
        out["error"] = rounded(p.error, 100000.0);
        out["seams_kept"] = p.seams_kept;
        out["center"] = {rounded(p.center.x, 10000.0), rounded(p.center.y, 10000.0), rounded(p.center.z, 10000.0)};
        out["radius"] = rounded(p.radius, 10000.0);
        out["bake_ms"] = rounded(p.seconds * 1000.0f, 100.0);
    }
    return out;
}

} // namespace

void register_pose_commands(ai::CommandRegistry& registry, EditorState& state) {
    bool added = registry.add(
        {"bake_pose",
         "Bakes a skinned mesh file (loaded: import_mesh) held in one pose into a small plain mesh, what a "
         "crowd's distant people are drawn with (they need no skeleton of their own): `mesh` the file, `clip` "
         "one of its clips (\"\" or left out: the rest pose), `time` seconds into the clip (default 0; past its "
         "end is its last pose), `triangles` what the whole figure is cut down to (64 to 20000, default 3000; "
         "a file drawn in parts shares them out by their triangles, at least 16 each, the parts still meeting "
         "where they met while each can be cut that far so: `seams_kept`). The bake runs on a worker (the "
         "file is cut down once for a size, kept for its other poses of that size; then each pose takes a "
         "few milliseconds) and its meshes go to the renderer a few a frame (2, and no more than 8000 triangles after the first): `state` is `baking`, then "
         "`ready` (or `failed` with `problem`); `wait` true waits for it (this thread then helps bake, and "
         "everything is handed over at once: for tools, not for a running game). Returns the pose (as "
         "get_baked_poses lists it), the same one when asked for again (a failed one is tried again). Morph targets stay at their rest weights (a clip's morph "
         "channels are not baked). At most 256 poses are kept (clear_baked_poses). View state: not saved, not "
         "undone.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"mesh"}},
              {"properties",
               {{"mesh", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
                {"clip", {{"type", "string"}, {"maxLength", 128}}},
                {"time", {{"type", "number"}, {"minimum", 0}, {"maximum", anim::k_max_clip_seconds}}},
                {"triangles",
                 {{"type", "integer"}, {"minimum", PoseBakes::k_min_triangles}, {"maximum", PoseBakes::k_max_triangles}}},
                {"wait", {{"type", "boolean"}}}}}},
         false, // view state
         [&state](const Json& args, CommandContext&) {
             if (state.poses == nullptr) return CommandResult::failure("poses cannot be baked here");
             std::string error;
             const u32 index = state.poses->want(args["mesh"].get<std::string>(), args.value("clip", std::string()),
                                                 args.value("time", 0.0f),
                                                 args.value("triangles", PoseBakes::k_default_triangles), error);
             if (index == PoseBakes::k_no_pose) return CommandResult::failure(error);
             if (args.value("wait", false)) state.poses->finish();
             return CommandResult::success(pose_json(state.poses->poses()[index], index));
         }});
    GIANT_ASSERT(added, "pose command could not be registered");
    added = registry.add(
        {"get_baked_poses",
         "The poses baked (bake_pose): `poses`, each with its index `pose`, `mesh`, `clip`, `time`, `triangles` "
         "(asked for), `state` (`baking`, `ready`, `failed` with `problem`) and when ready `parts` (its meshes: "
         "one, or one for each part the file is drawn in), `baked_triangles`, `source_triangles` (the file's), "
         "`error` (metres the cut moved the surface as the simplifier measured it, at bind, the worst part's), "
         "`seams_kept` (false: a part's joins with the others were let go to reach its triangles), `center` and `radius` (a sphere "
         "round the posed figure, in the file's space) and `bake_ms` (the worker's time: the first pose of a "
         "file includes cutting it down); with `baking` (how many are not done) and `baked` (bakes finished, "
         "failed ones too, since the editor started). A file imported again has its poses baked again. `wait` true waits for "
         "every bake first.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", {{"wait", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.poses == nullptr) return CommandResult::failure("poses cannot be baked here");
             if (args.value("wait", false)) state.poses->finish();
             Json poses = Json::array();
             for (usize i = 0; i < state.poses->poses().size(); ++i) poses.push_back(pose_json(state.poses->poses()[i], i));
             return CommandResult::success(
                 {{"poses", std::move(poses)}, {"baking", state.poses->baking()}, {"baked", state.poses->baked()}});
         }});
    GIANT_ASSERT(added, "pose command could not be registered");
    added = registry.add(
        {"clear_baked_poses",
         "Forgets every baked pose (bake_pose) and takes their meshes out of the renderer; indices begin again "
         "from 0. Returns `cleared`, how many there were.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         false,
         [&state](const Json&, CommandContext&) {
             if (state.poses == nullptr) return CommandResult::failure("poses cannot be baked here");
             const usize cleared = state.poses->poses().size();
             state.poses->clear();
             return CommandResult::success({{"cleared", cleared}});
         }});
    GIANT_ASSERT(added, "pose command could not be registered");
}

} // namespace giant::editor
