// Ragdolls: a skinned character gone limp with ragdoll_entity while
// simulating falls as bodies joined at its joints, which pose it from then
// on; elbows and knees fold one way; a walker goes limp where it walks;
// letting go gives it back to its clips.
#include "../src/animation_state.h"
#include "../src/editor_commands.h"
#include "../src/hit_targets.h"
#include "../src/mesh_library.h"
#include "../src/nav_state.h"
#include "../src/physics_state.h"
#include "../src/pose_math.h"
#include "../src/ragdoll_state.h"
#include "../src/scene_render.h"
#include "../src/walker_state.h"

#include "gltf_test_files.h"
#include "giant_test.h"

#include "giant/anim/skeleton.h"
#include "giant/sim/hit_shapes.h"
#include "giant/world/scene.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::editor;
using ai::Json;
using geometry::testing::k_person;
using geometry::testing::k_person_joints;

namespace {

std::string person_file() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_ragdoll_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "person.glb";
    const std::vector<u8> bytes = geometry::testing::skinned_person();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path.generic_string();
}

struct Fixture {
    HitTargets hits; // before the physics, which reads its shapes
    PhysicsState physics;
    NavState nav{nullptr, nullptr, nullptr};
    WalkerState walkers{physics, nav};
    MeshLibrary meshes{nullptr};
    AnimationState animation{nullptr};
    RagdollState ragdolls{physics, animation, &walkers};
    ai::CommandRegistry commands;
    EditorState state;
    std::string file = person_file();
    Fixture() {
        state.physics = &physics;
        state.nav = &nav;
        state.walkers = &walkers;
        state.meshes = &meshes;
        state.animation = &animation;
        state.hits = &hits;
        state.ragdolls = &ragdolls;
        physics.set_hit_targets(&hits);
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_animation_commands(commands, state);
        register_nav_commands(commands, state);
        register_walker_commands(commands, state);
        register_ragdoll_commands(commands, state);
        connect_walkers(state);
        connect_ragdolls(state);
        connect_blows(state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        ai::ExecuteOptions options;
        options.record_history = true;
        return commands.execute(name, args, options);
    }
    world::EntityId create(const Json& args) {
        const ai::CommandResult r = run("create_entity", args);
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["id"].get<world::EntityId>() : 0;
    }
    void floor() { create({{"shape", "plane"}, {"position", {0.0, 0.0, 0.0}}, {"scale", {60.0, 1.0, 60.0}}}); }
    // A person standing with its feet at (x, y, z), playing "idle".
    world::EntityId person(double x, double y, double z, const char* body = "fixed") {
        const world::EntityId id = create({{"shape", "mesh"}, {"mesh", file}, {"position", {x, y, z}}, {"body", body}});
        GIANT_CHECK(run("set_animation", {{"id", id}, {"clip", "idle"}}).ok);
        return id;
    }
    // A frame as main runs one.
    void frame(double seconds = 1.0 / 60.0) {
        const world::SceneChanges changed = state.scene.take_changes();
        walkers.before_step(state.scene, seconds);
        ragdolls.before_step(state.scene, meshes, seconds);
        physics.update(state.scene, changed, seconds);
        walkers.after_step();
        ragdolls.after_step();
        animation.update(state.scene, meshes, changed, seconds);
        hits.update(state.scene, meshes, animation);
    }
    void frames(double seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 60.0 + 0.5); ++i) frame();
    }
    // Where a joint is in the world, as the character is drawn.
    DVec3 joint(world::EntityId id, usize j) const {
        const AnimationState::Animated* a = animation.find(id);
        const world::Entity* e = state.scene.find(id);
        GIANT_CHECK(a != nullptr && e != nullptr && j < a->model.size());
        if (a == nullptr || e == nullptr || j >= a->model.size()) return {};
        world::Transform at = e->transform;
        if (const auto p = walkers.poses().find(id); p != walkers.poses().end()) at = p->second;
        if (const auto p = ragdolls.poses().find(id); p != ragdolls.poses().end()) at = p->second;
        const Mat4& m = a->model[j];
        return pose::place(at, {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
    }
};

// The person's skeleton as imported (its joints in the importer's order, not the file's).
const anim::Skeleton& bones(const Fixture& f) { return f.meshes.find_normalized(f.file)->skinned->skeleton; }

usize joint_named(const Fixture& f, const char* name) {
    const std::vector<std::string>& names = bones(f).names;
    const auto it = std::find(names.begin(), names.end(), name);
    GIANT_CHECK(it != names.end());
    return it != names.end() ? static_cast<usize>(it - names.begin()) : 0;
}

// How far a joint is from the one above it, at rest.
float rest_gap(const Fixture& f, usize j) {
    const std::vector<Mat4>& rest = f.meshes.find_normalized(f.file)->skinned->rest_model;
    const usize p = static_cast<usize>(bones(f).parents[j]);
    return length(Vec3{rest[j].at(0, 3) - rest[p].at(0, 3), rest[j].at(1, 3) - rest[p].at(1, 3),
                       rest[j].at(2, 3) - rest[p].at(2, 3)});
}

// The file's own record of a bone (where its box ends).
const geometry::testing::PersonJoint& person_joint(const char* name) {
    for (const geometry::testing::PersonJoint& p : k_person) {
        if (std::string(p.name) == name) return p;
    }
    GIANT_CHECK(false);
    return k_person[0];
}

// A joint's turn in the entity's space, on a direction (its model matrix's, which neither stretches nor shears here).
Vec3 turned(const Mat4& m, const Vec3& v) {
    return {m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z, m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z,
            m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z};
}

} // namespace

GIANT_TEST("ragdolls: bones are told apart by the usual names") {
    using K = RagdollState::Kind;
    const std::pair<const char*, K> names[] = {
        {"mixamorig:Hips", K::other},       {"mixamorig:Spine2", K::spine},      {"mixamorig:Neck", K::neck},
        {"mixamorig:Head", K::head},        {"mixamorig:LeftShoulder", K::collar}, {"mixamorig:LeftArm", K::upper_arm},
        {"mixamorig:LeftForeArm", K::forearm}, {"mixamorig:LeftHand", K::hand},  {"mixamorig:RightUpLeg", K::thigh},
        {"mixamorig:RightLeg", K::shin},    {"mixamorig:RightFoot", K::foot},    {"mixamorig:RightToeBase", K::foot},
        {"pelvis", K::other},               {"spine_03", K::spine},              {"clavicle_l", K::collar},
        {"upperarm_l", K::upper_arm},       {"lowerarm_r", K::forearm},          {"hand_r", K::hand},
        {"thigh_l", K::thigh},              {"calf_l", K::shin},                 {"foot_l", K::foot},
        {"upper_arm.L", K::upper_arm},      {"forearm.L", K::forearm},           {"shin.R", K::shin},
        {"Armature", K::other},             {"Chest", K::spine},                 {"tail_01", K::other},
    };
    for (const auto& [name, kind] : names) {
        if (RagdollState::kind_of(name) != kind) GIANT_CHECK(std::string(name).empty());
    }
    // Elbows and knees fold; the rest lean in cones.
    GIANT_CHECK(RagdollState::limits_of(K::forearm).fold > 90.0f && RagdollState::limits_of(K::shin).fold > 90.0f);
    for (const K k : {K::other, K::spine, K::neck, K::head, K::collar, K::upper_arm, K::hand, K::thigh, K::foot}) {
        GIANT_CHECK(RagdollState::limits_of(k).fold == 0.0f && RagdollState::limits_of(k).swing > 0.0f);
    }
}

GIANT_TEST("ragdolls: a character goes limp as it stands, falls joined at its joints and comes to rest") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(2.0, 0.3, 3.0);
    const world::EntityId cube = f.create({{"shape", "cube"}, {"position", {20.0, 0.5, 20.0}}});
    f.frame();
    // Not simulating: refused. Not a character, no such entity: refused.
    GIANT_CHECK(!f.run("ragdoll_entity", {{"id", id}}).ok);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    GIANT_CHECK(!f.run("ragdoll_entity", {{"id", cube}}).ok && !f.run("ragdoll_entity", {{"id", 987654}}).ok);
    GIANT_CHECK(f.ragdolls.size() == 0 && f.animation.find(cube) == nullptr);
    GIANT_CHECK(!f.run("get_ragdoll", {{"id", id}}).ok && !f.run("stop_ragdoll", {{"id", id}}).ok);

    std::vector<DVec3> stood;
    for (usize j = 0; j < k_person_joints; ++j) stood.push_back(f.joint(id, j));
    GIANT_CHECK(std::fabs(stood[joint_named(f, "Head")].y - 1.8) < 1e-3);
    const usize bodies = f.physics.world().size();
    // (With a shove: let go standing straight and square on a level floor, it would balance on its shins.)
    const ai::CommandResult made = f.run("ragdoll_entity", {{"id", id}, {"velocity", {0.4, 0.0, 0.3}}});
    GIANT_CHECK(made.ok);
    if (!made.ok) return;
    // A body for each bone but the finger, which rides the forearm.
    GIANT_CHECK(made.value["bones"].get<usize>() == k_person_joints - 1 && made.value["joints"].get<usize>() == k_person_joints);
    GIANT_CHECK(f.physics.world().size() == bodies + k_person_joints - 1 && f.physics.world().ragdolls() == 1);
    GIANT_CHECK(made.value["mass"].get<float>() > 20.0f && made.value["mass"].get<float>() < 200.0f);
    const Json& list = made.value["bone_list"];
    GIANT_CHECK(list.size() == k_person_joints - 1 && list[0]["joint"] == "Hips" && list[0]["parent"].is_null());
    for (const Json& bone : list) {
        const std::string name = bone["joint"].get<std::string>();
        GIANT_CHECK(name != "LeftHandIndex1");
        const bool folds = name == "LeftForeArm" || name == "RightForeArm" || name == "LeftLeg" || name == "RightLeg";
        GIANT_CHECK(bone["hinge"].get<bool>() == folds);
        if (name == "LeftLeg") GIANT_CHECK(bone["kind"] == "shin" && bone["parent"] == "LeftUpLeg");
        if (name == "Head") GIANT_CHECK(bone["kind"] == "head" && bone["parent"] == "Spine");
        GIANT_CHECK(bone["mass"].get<float>() >= 0.02f * 0.99f * made.value["mass"].get<float>() / 11.0f);
    }
    GIANT_CHECK(!f.run("ragdoll_entity", {{"id", id}}).ok); // one already
    GIANT_CHECK(f.run("get_animation", {{"id", id}}).value["held"].get<bool>());
    // Nothing jumps as it goes limp: every joint where it stood.
    for (usize j = 0; j < k_person_joints; ++j) GIANT_CHECK(length(f.joint(id, j) - stood[j]) < 1e-4);
    GIANT_CHECK(f.ragdolls.take_redraw() == std::vector<world::EntityId>{id});

    // It falls: joints stay the lengths apart they were, the finger with its forearm.
    f.frames(0.25);
    GIANT_CHECK(f.joint(id, joint_named(f, "Hips")).y < stood[joint_named(f, "Hips")].y - 0.02); // dropping
    GIANT_CHECK(!f.ragdolls.take_moved().empty());
    double lowest = 1e9;
    for (int k = 0; k < 16; ++k) {
        f.frames(0.25);
        for (usize j = 0; j < k_person_joints; ++j) {
            const DVec3 at = f.joint(id, j);
            GIANT_CHECK(std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z));
            lowest = std::min(lowest, at.y);
            if (bones(f).parents[j] >= 0) {
                const usize p = static_cast<usize>(bones(f).parents[j]);
                GIANT_CHECK(std::fabs(length(at - f.joint(id, p)) - static_cast<double>(rest_gap(f, j))) < 0.04);
            }
        }
    }
    GIANT_CHECK(lowest > -0.05); // on the floor, not through it
    // Down: its head near the ground, lying about where it stood; the scene keeps where it was put.
    const DVec3 head = f.joint(id, joint_named(f, "Head"));
    GIANT_CHECK(head.y < 0.5 && std::hypot(head.x - 2.0, head.z - 3.0) < 2.5);
    GIANT_CHECK(f.state.scene.find(id)->transform.position == (DVec3{2.0, 0.3, 3.0}));
    const auto drawn = f.ragdolls.poses().find(id);
    GIANT_CHECK(drawn != f.ragdolls.poses().end() && drawn->second.position.y < 0.3 - 0.4);
    // At rest: asleep, and no more poses handed on.
    for (int k = 0; k < 40 && !f.run("get_ragdoll", {{"id", id}}).value["ragdoll"]["asleep"].get<bool>(); ++k) f.frames(0.25);
    const Json got = f.run("get_ragdoll", {{"id", id}}).value;
    GIANT_CHECK(got["ragdolls"] == 1 && got["asleep"] == 1 && got["bodies"] == k_person_joints - 1);
    GIANT_CHECK(got["ragdoll"]["asleep"].get<bool>() && !got["ragdoll"].contains("bone_list"));
    f.frames(0.1);
    const u64 poses = f.animation.poses();
    f.ragdolls.take_moved();
    f.frames(0.5);
    GIANT_CHECK(f.animation.poses() == poses && f.ragdolls.take_moved().empty());
    GIANT_CHECK(f.animation.posed_last() == 0); // (its clip's time moving underneath works out no pose)
}

GIANT_TEST("ragdolls: thrown about, elbows fold forward and knees back, never the other way") {
    Fixture f;
    f.floor();
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    // Three thrown different ways, tumbling over the floor.
    const Vec3 thrown[] = {{3.0f, 4.0f, -2.0f}, {-2.0f, 5.0f, 3.0f}, {0.5f, 2.0f, 5.0f}};
    std::vector<world::EntityId> ids;
    for (int i = 0; i < 3; ++i) ids.push_back(f.person(6.0 * i, 1.0, 0.0));
    f.frames(0.1);
    for (int i = 0; i < 3; ++i) {
        const Vec3& v = thrown[i];
        GIANT_CHECK(f.run("ragdoll_entity", {{"id", ids[static_cast<usize>(i)]}, {"velocity", {v.x, v.y, v.z}}}).ok);
    }
    // Each limb's fold: the turn of its lower bone against its upper, about the axis an elbow or a
    // knee turns on (square to the limb and to the way it folds, carried by the upper bone), in
    // degrees: over 0 the way it should fold (an elbow forward, a knee back), under 0 the other way.
    struct Limb {
        const char* top;
        const char* middle;
        float ahead;
    };
    const Limb limbs[] = {{"LeftArm", "LeftForeArm", 1.0f}, {"RightArm", "RightForeArm", 1.0f},
                          {"LeftUpLeg", "LeftLeg", -1.0f}, {"RightUpLeg", "RightLeg", -1.0f}};
    float least = 0.0f, most = 0.0f, aside = 0.0f;
    for (int step = 0; step < 300; ++step) {
        f.frame();
        for (const world::EntityId id : ids) {
            const AnimationState::Animated* a = f.animation.find(id);
            for (const Limb& limb : limbs) {
                const geometry::testing::PersonJoint& bone = person_joint(limb.middle);
                const Vec3 along = normalize_or_zero(Vec3{bone.to[0] - bone.at[0], bone.to[1] - bone.at[1], bone.to[2] - bone.at[2]});
                const Vec3 axis = normalize_or_zero(cross(along, Vec3{0.0f, 0.0f, limb.ahead}));
                // (At rest no joint is turned: the limb lies along `along` in both bones' spaces.)
                const Vec3 upper = turned(a->model[joint_named(f, limb.top)], along);
                const Vec3 lower = turned(a->model[joint_named(f, limb.middle)], along);
                const Vec3 hinge = turned(a->model[joint_named(f, limb.top)], axis);
                const float angle = std::atan2(dot(cross(upper, lower), hinge), dot(upper, lower)) * 57.29578f;
                least = std::min(least, angle);
                most = std::max(most, angle);
                // Out of the plane it folds in: the lower bone's lean along the hinge's axis.
                aside = std::max(aside, std::fabs(std::asin(std::clamp(dot(lower, hinge), -1.0f, 1.0f))) * 57.29578f);
            }
        }
    }
    GIANT_CHECK(most > 25.0f);  // limbs did fold
    GIANT_CHECK(most < 155.0f); // not past where an elbow or a knee stops (140 degrees, and a joint's give)
    // Never backwards, nor sideways. (A joint gives some degrees while the body's weight lands on a
    // limb at speed: measured 14 back and 15 aside at worst here, 3 and 9 let fall gently; folding
    // the wrong way would show as far as it folds the right way.)
    GIANT_CHECK(least > -25.0f);
    GIANT_CHECK(aside < 25.0f);
}

GIANT_TEST("ragdolls: a walker goes limp where it walks, moving as it was, and walks no more") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(-10.0, 0.0, 0.0, "none");
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.5);
    const ai::CommandResult sent = f.run("walk_to", {{"id", id}, {"to", {10.0, 0.0, 0.0}}, {"speed", 3.0}});
    GIANT_CHECK(sent.ok);
    if (!sent.ok) return;
    for (int k = 0; k < 600 && f.run("get_walker", {{"id", id}}).value["walker"]["phase"] != "walking"; ++k) f.frame();
    f.frames(1.5);
    const auto walked = f.walkers.poses().find(id);
    GIANT_CHECK(walked != f.walkers.poses().end());
    if (walked == f.walkers.poses().end()) return;
    const double x = walked->second.position.x;
    GIANT_CHECK(x > -8.0 && x < 0.0); // well on its way
    const DVec3 head = f.joint(id, joint_named(f, "Head"));
    const ai::CommandResult made = f.run("ragdoll_entity", {{"id", id}});
    GIANT_CHECK(made.ok);
    if (!made.ok) return;
    GIANT_CHECK(length(f.joint(id, joint_named(f, "Head")) - head) < 1e-3); // where it walked, not where the scene has it
    GIANT_CHECK(f.walkers.find(id) == nullptr && !f.run("get_walker", {{"id", id}}).ok);
    // Nor is it sent walking while it lies; one made to (a game's walkers put back) is stopped at the next step.
    GIANT_CHECK(!f.run("walk_to", {{"id", id}, {"to", {10.0, 0.0, 0.0}}}).ok && f.walkers.find(id) == nullptr);
    GIANT_CHECK(f.walkers.walk_to(f.state.scene, id, {10.0, 0.0, 0.0}, 1.4f).empty() && f.walkers.find(id) != nullptr);
    f.frame();
    GIANT_CHECK(f.walkers.find(id) == nullptr && f.ragdolls.find(id) != nullptr);
    GIANT_CHECK(made.value["velocity"][0].get<float>() > 2.0f); // carried on at its walk
    f.frames(0.5);
    GIANT_CHECK(f.ragdolls.poses().at(id).position.x > x + 0.5);
    // A velocity given is the one it gets.
    const world::EntityId other = f.person(0.0, 0.0, 12.0);
    f.frame();
    const ai::CommandResult thrown = f.run("ragdoll_entity", {{"id", other}, {"velocity", {0.0, 6.0, 0.0}}});
    GIANT_CHECK(thrown.ok && std::fabs(thrown.value["velocity"][1].get<float>() - 6.0f) < 1e-3f);
    f.frames(0.3);
    GIANT_CHECK(f.joint(other, joint_named(f, "Hips")).y > 2.0);
}

GIANT_TEST("ragdolls: let go by command, with its entity, and all when the simulation stops or the scene goes") {
    Fixture f;
    f.floor();
    const world::EntityId a = f.person(0.0, 0.5, 0.0), b = f.person(5.0, 0.5, 0.0), c = f.person(10.0, 0.5, 0.0);
    // (One with nothing of its own to play: animated only while it is a ragdoll.)
    const world::EntityId d = f.create({{"shape", "mesh"}, {"mesh", f.file}, {"position", {15.0, 0.5, 0.0}}});
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    const usize bodies = f.physics.world().size();
    GIANT_CHECK(f.animation.find(d) == nullptr);
    for (const world::EntityId id : {a, b, c, d}) GIANT_CHECK(f.run("ragdoll_entity", {{"id", id}}).ok);
    GIANT_CHECK(f.animation.find(d) != nullptr && f.ragdolls.size() == 4);
    f.frames(1.0);
    f.ragdolls.take_redraw();
    // By command: its bodies gone, drawn where the scene has it, its clip posing it again.
    GIANT_CHECK(f.run("stop_ragdoll", {{"id", a}}).ok && !f.run("stop_ragdoll", {{"id", a}}).ok);
    GIANT_CHECK(f.ragdolls.find(a) == nullptr && !f.ragdolls.poses().contains(a));
    GIANT_CHECK(f.ragdolls.take_redraw() == std::vector<world::EntityId>{a});
    GIANT_CHECK(f.physics.world().ragdolls() == 3 && f.physics.world().size() == bodies + 3 * (k_person_joints - 1));
    f.frame();
    GIANT_CHECK(!f.run("get_animation", {{"id", a}}).value["held"].get<bool>());
    GIANT_CHECK(std::fabs(f.joint(a, joint_named(f, "Head")).y - 2.0) < 1e-3); // standing as placed
    // One with nothing to play stands at rest again, animated no more.
    GIANT_CHECK(f.run("stop_ragdoll", {{"id", d}}).ok);
    GIANT_CHECK(f.animation.find(d) == nullptr);
    // Hidden: let go at the next step. Removed: likewise.
    GIANT_CHECK(f.run("set_entity", {{"id", b}, {"visible", false}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.find(b) == nullptr && f.physics.world().ragdolls() == 1);
    GIANT_CHECK(f.run("delete_entities", {{"ids", Json::array({c})}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.size() == 0 && f.physics.world().ragdolls() == 0 && f.physics.world().size() == bodies);
    GIANT_CHECK(f.ragdolls.poses().empty());
    // Stopping the simulation lets every one go. (A clip played on it meanwhile plays underneath.)
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", a}}).ok);
    GIANT_CHECK(f.run("play_animation", {{"id", a}, {"clip", "raise"}}).ok && f.animation.holds_pose(a));
    f.frames(0.5);
    GIANT_CHECK(f.ragdolls.find(a) != nullptr && f.joint(a, joint_named(f, "Hips")).y < 1.4);
    // (Stopped and started again within a frame: another run, let go all the same.)
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok && f.run("simulate", {{"on", true}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.size() == 0 && f.physics.world().ragdolls() == 0);
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", a}}).ok);
    f.frames(0.5);
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.size() == 0 && f.physics.world().ragdolls() == 0 && !f.animation.holds_pose(a));
    GIANT_CHECK(std::fabs(f.joint(a, joint_named(f, "Head")).y - 2.0) < 1e-3);
    // A new scene: the world made afresh with the ragdolls in it, theirs forgotten.
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frame();
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", a}}).ok);
    f.frames(0.2);
    const u64 clears = f.physics.world().clears();
    GIANT_CHECK(f.run("new_scene").ok);
    // (As main: the physics notices the scene first, and clears its world whole with the bones in it.)
    f.physics.flush(f.state.scene);
    GIANT_CHECK(f.physics.world().clears() == clears + 1 && f.physics.world().ragdolls() == 0 && f.physics.world().size() == 0);
    GIANT_CHECK(f.ragdolls.size() == 1); // not yet looked
    f.ragdolls.after_step();             // (a step in between touches nothing gone)
    // (Asked before the next frame: none is one any more.)
    const ai::CommandResult none = f.run("get_ragdoll");
    GIANT_CHECK(none.ok && none.value["ragdolls"] == 0);
    GIANT_CHECK(!f.run("get_ragdoll", {{"id", a}}).ok && !f.run("stop_ragdoll", {{"id", a}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.size() == 0 && f.ragdolls.poses().empty() && f.physics.world().size() == 0);
    // And they work on in the new scene.
    f.floor();
    const world::EntityId again = f.person(0.0, 0.5, 0.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", again}}).ok);
    f.frames(0.5);
    GIANT_CHECK(f.joint(again, joint_named(f, "Hips")).y < 1.4);
}

GIANT_TEST("ragdolls: bullets meet a ragdoll's bones, not its hit capsules, and push them") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(0.0, 0.0, 0.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    GIANT_CHECK(f.hits.shapes().find(id) != nullptr);
    // Standing: a shot at its chest meets its capsule.
    const Json shot = {{"origin", {0.0, 1.3, -5.0}}, {"direction", {0.0, 0.0, 1.0}}, {"resolve", true}};
    ai::CommandResult fired = f.run("fire", shot);
    GIANT_CHECK(fired.ok && !fired.value["hits"].empty() && fired.value["hits"][0]["entity"] == id);
    GIANT_CHECK(fired.value["hits"][0]["bone"] == "Spine");
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", id}}).ok);
    GIANT_CHECK(f.hits.shapes().find(id) == nullptr); // its bones' bodies are met now, at once
    // (A ray names the entity by its bones too.)
    const ai::CommandResult ray = f.run("raycast", {{"origin", {0.0, 1.3, -5.0}}, {"direction", {0.0, 0.0, 1.0}}});
    GIANT_CHECK(ray.ok && ray.value["hit"].get<bool>() && ray.value["entity"] == id);
    const float before = f.run("get_ragdoll", {{"id", id}, {"bones", true}}).value["ragdoll"]["velocity"][2].get<float>();
    // (A heavy slug that stops in it: all its momentum goes to the body.)
    Json slug = shot;
    slug["mass"] = 0.05;
    slug["penetration"] = 0.05;
    fired = f.run("fire", slug);
    GIANT_CHECK(fired.ok && !fired.value["hits"].empty());
    if (!fired.ok || fired.value["hits"].empty()) return;
    const Json& hit = fired.value["hits"][0];
    GIANT_CHECK(hit["entity"] == id && hit["character"].get<bool>() && hit["surface"] == "flesh");
    // Pushed along the shot.
    f.frame();
    const RagdollState::Ragdoll& r = *f.ragdolls.find(id);
    const sim::BodyId spine = f.physics.world().ragdoll_bodies(r.id)[r.bone_of[joint_named(f, "Spine")]];
    GIANT_CHECK(f.physics.world().state(spine)->linear_velocity.z > before + 0.1f);
    // Let go once fallen: its capsules are met again, standing where the scene has it (not as it lay).
    f.frames(1.5);
    GIANT_CHECK(f.joint(id, joint_named(f, "Head")).y < 1.5);
    GIANT_CHECK(f.run("stop_ragdoll", {{"id", id}}).ok);
    const sim::HitCharacter* stands = f.hits.shapes().find(id);
    GIANT_CHECK(stands != nullptr);
    double top = 0.0;
    if (stands != nullptr) {
        for (const sim::HitCapsule& c : stands->capsules) top = std::max({top, c.a.y, c.b.y});
    }
    GIANT_CHECK(top > 1.5);
}

GIANT_TEST("ragdolls: a scaled, turned character keeps its shape as it goes limp; one of no size is refused") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.create({{"shape", "mesh"}, {"mesh", f.file}, {"position", {3.0, 0.2, -4.0}},
                                         {"rotation", {{"yaw", 70.0}}}, {"scale", {2.0, 2.0, 2.0}}});
    GIANT_CHECK(f.run("set_animation", {{"id", id}, {"clip", "raise"}}).ok); // (posed away from rest: an arm up)
    const world::EntityId flat = f.create({{"shape", "mesh"}, {"mesh", f.file}, {"position", {30.0, 0.2, 0.0}},
                                           {"scale", {1.0, 0.0005, 1.0}}});
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    std::vector<DVec3> stood;
    for (usize j = 0; j < k_person_joints; ++j) stood.push_back(f.joint(id, j));
    GIANT_CHECK(stood[joint_named(f, "LeftForeArm")].y > stood[joint_named(f, "LeftArm")].y + 0.4); // raised, twice the size
    const ai::CommandResult made = f.run("ragdoll_entity", {{"id", id}, {"bones", true}});
    GIANT_CHECK(!made.ok); // (no such argument)
    const ai::CommandResult limp = f.run("ragdoll_entity", {{"id", id}});
    GIANT_CHECK(limp.ok && limp.value["mass"].get<float>() > 8.0f * 20.0f); // eight times the weight
    for (usize j = 0; j < k_person_joints; ++j) GIANT_CHECK(length(f.joint(id, j) - stood[j]) < 1e-3);
    f.frames(1.0);
    for (usize j = 0; j < k_person_joints; ++j) {
        if (bones(f).parents[j] < 0) continue;
        const usize p = static_cast<usize>(bones(f).parents[j]);
        GIANT_CHECK(std::fabs(length(f.joint(id, j) - f.joint(id, p)) - 2.0 * static_cast<double>(rest_gap(f, j))) < 0.08);
    }
    GIANT_CHECK(!f.run("ragdoll_entity", {{"id", flat}}).ok && !f.animation.holds_pose(flat) && f.ragdolls.size() == 1);
}

GIANT_TEST("ragdolls: step_physics moves them too, and the scene draws them where they lie, as moving") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(0.0, 1.0, 0.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frame();
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", id}}).ok);
    GIANT_CHECK(f.run("step_physics", {{"seconds", 1.0}}).ok);
    const auto at = f.ragdolls.poses().find(id);
    GIANT_CHECK(at != f.ragdolls.poses().end() && at->second.position.y < 0.5);
    // Its characters' light: its capsules where it is drawn.
    std::vector<render::Occluder> out;
    f.animation.follow_now(f.state.scene, f.meshes, {});
    character_occluders(f.state.scene, f.meshes, f.animation, nullptr, nullptr, nullptr, nullptr, DVec3{0.0, 1.0, 0.0}, out,
                        &f.ragdolls.poses());
    GIANT_CHECK(out.size() == k_person_joints);
    double top = -1e9;
    for (const render::Occluder& o : out) top = std::max({top, o.a.y, o.b.y});
    GIANT_CHECK(top < 1.9); // (standing as placed it would reach 2.7)
}

GIANT_TEST("ragdolls: a shot hard enough drops a standing character, pushed on the bone struck; a light one leaves it standing") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(0.0, 0.0, 0.0);
    const world::EntityId other = f.person(6.0, 0.0, 0.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    // The settings: told, set a field at a time, refused out of range.
    ai::CommandResult got = f.run("get_ragdoll");
    GIANT_CHECK(got.ok && got.value["settings"]["shot_drop"] == 3.0 && got.value["settings"]["blast_drop"] == 150.0 &&
                got.value["settings"]["starts_per_step"] == 4 && got.value["waiting"] == 0 && got.value["dropped"] == 0);
    GIANT_CHECK(!f.run("set_ragdoll_settings", {{"starts_per_step", 0}}).ok && !f.run("set_ragdoll_settings", {{"shot_drop", -1.0}}).ok);
    ai::CommandResult set = f.run("set_ragdoll_settings", {{"shot_drop", 1000.0}});
    GIANT_CHECK(set.ok && set.value["shot_drop"] == 1000.0 && set.value["blast_drop"] == 150.0);
    // A slug that stops in its chest (all its momentum given: 0.05 kg at 880 m/s, 44 N s): not enough for that.
    const Json slug = {{"origin", {0.0, 1.3, -5.0}}, {"direction", {0.0, 0.0, 1.0}}, {"resolve", true},
                       {"mass", 0.05},       {"penetration", 0.05}};
    ai::CommandResult fired = f.run("fire", slug);
    GIANT_CHECK(fired.ok && !fired.value["hits"].empty() && fired.value["hits"][0]["entity"] == id);
    GIANT_CHECK(f.ragdolls.waiting() == 0);
    f.frames(0.1);
    GIANT_CHECK(f.ragdolls.size() == 0);
    // None at all with 0.
    GIANT_CHECK(f.run("set_ragdoll_settings", {{"shot_drop", 0.0}}).ok);
    GIANT_CHECK(f.run("fire", slug).ok && f.ragdolls.waiting() == 0);
    // Enough: it waits for the next step, then lies limp, its capsules gone, its chest moving along the shot.
    GIANT_CHECK(f.run("set_ragdoll_settings", {{"shot_drop", 3.0}}).ok);
    fired = f.run("fire", slug);
    GIANT_CHECK(fired.ok && !fired.value["hits"].empty());
    if (!fired.ok || fired.value["hits"].empty()) return;
    const Json first = fired.value["hits"][0];
    GIANT_CHECK(first["entity"] == id && first["bone"] == "Spine" && first["zone"].is_string());
    GIANT_CHECK(f.ragdolls.waiting() == 1 && f.ragdolls.find(id) == nullptr && f.hits.shapes().find(id) != nullptr);
    GIANT_CHECK(f.run("get_ragdoll").value["waiting"] == 1);
    f.frame();
    const RagdollState::Ragdoll* r = f.ragdolls.find(id);
    GIANT_CHECK(r != nullptr && f.ragdolls.waiting() == 0 && f.ragdolls.dropped() == 1);
    GIANT_CHECK(f.hits.shapes().find(id) == nullptr && f.ragdolls.find(other) == nullptr);
    if (r == nullptr) return;
    const sim::BodyId spine = f.physics.world().ragdoll_bodies(r->id)[r->bone_of[joint_named(f, "Spine")]];
    GIANT_CHECK(f.physics.world().state(spine)->linear_velocity.z > 0.5f);
    // A shot at it as it lies names the bone it meets and its zone, as it did its capsule's.
    fired = f.run("fire", slug);
    GIANT_CHECK(fired.ok && !fired.value["hits"].empty());
    if (fired.ok && !fired.value["hits"].empty()) {
        const Json& hit = fired.value["hits"][0];
        GIANT_CHECK(hit["entity"] == id && hit["character"].get<bool>());
        GIANT_CHECK(hit["bone"] == "Spine" && hit["zone"] == first["zone"]);
    }
    GIANT_CHECK(f.ragdolls.waiting() == 0); // (a bone's body: pushed as any body, nothing to drop)
    f.frames(1.0);
    GIANT_CHECK(f.joint(id, joint_named(f, "Head")).y < 1.5 && f.joint(id, joint_named(f, "Hips")).z > 0.05);
    // One struck that is gone before its turn, and one struck as the simulation stops: no ragdoll, nothing waiting.
    Json at_other = slug;
    at_other["origin"] = {6.0, 1.3, -5.0};
    GIANT_CHECK(f.run("fire", at_other).ok && f.ragdolls.waiting() == 1);
    GIANT_CHECK(f.run("delete_entities", {{"ids", Json::array({other})}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.waiting() == 0 && f.ragdolls.size() == 1);
    const world::EntityId third = f.person(-6.0, 0.0, 0.0);
    f.frames(0.2);
    at_other["origin"] = {-6.0, 1.3, -5.0};
    GIANT_CHECK(f.run("fire", at_other).ok && f.ragdolls.waiting() == 1);
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok && f.run("simulate", {{"on", true}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.waiting() == 0 && f.ragdolls.size() == 0 && f.ragdolls.find(third) == nullptr);
}

GIANT_TEST("ragdolls: a blast drops those standing in its reach, a few a step, and throws them and those lying") {
    Fixture f;
    f.floor();
    // Six in a ring 2 m about the blast, one far off.
    std::vector<world::EntityId> ring;
    std::vector<DVec3> where;
    for (int k = 0; k < 6; ++k) {
        const double a = 6.283185307179586 * k / 6.0;
        where.push_back({2.0 * std::cos(a), 0.0, 2.0 * std::sin(a)});
        ring.push_back(f.person(where.back().x, 0.0, where.back().z));
    }
    const world::EntityId far = f.person(30.0, 0.0, 0.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    GIANT_CHECK(f.run("set_ragdoll_settings", {{"starts_per_step", 2}}).ok);
    // Too weak where it reaches them: all stand.
    GIANT_CHECK(f.run("impact", {{"point", {0.0, 1.0, 0.0}}, {"radius", 6.0}, {"impulse", 100.0}}).ok);
    GIANT_CHECK(f.ragdolls.waiting() == 0);
    // Strong enough: all six wait, two made a step.
    GIANT_CHECK(f.run("impact", {{"point", {0.0, 1.0, 0.0}}, {"radius", 6.0}, {"impulse", 600.0}}).ok);
    GIANT_CHECK(f.ragdolls.waiting() == 6 && f.ragdolls.size() == 0);
    f.frame();
    GIANT_CHECK(f.ragdolls.waiting() == 4 && f.ragdolls.size() == 2);
    f.frame();
    GIANT_CHECK(f.ragdolls.waiting() == 2 && f.ragdolls.size() == 4);
    f.frame();
    GIANT_CHECK(f.ragdolls.waiting() == 0 && f.ragdolls.size() == 6 && f.ragdolls.dropped() == 6);
    GIANT_CHECK(f.ragdolls.find(far) == nullptr && f.hits.shapes().find(far) != nullptr);
    // Each thrown away from the blast (its hips moving outwards).
    f.frames(0.3);
    const usize hips = joint_named(f, "Hips");
    for (usize k = 0; k < ring.size(); ++k) {
        const DVec3 at = f.joint(ring[k], hips);
        const double out = ((at.x - where[k].x) * where[k].x + (at.z - where[k].z) * where[k].z) / 2.0;
        GIANT_CHECK(out > 0.1);
    }
    // Lying at rest, another blast throws them again, no faster than a blast throws (however strong).
    f.frames(4.0);
    const RagdollState::Ragdoll* r = f.ragdolls.find(ring[0]);
    GIANT_CHECK(r != nullptr);
    if (r == nullptr) return;
    const sim::BodyId root = f.physics.world().ragdoll_bodies(r->id)[0];
    const DVec3 lay = f.physics.world().state(root)->position;
    const Vec3 lying = f.physics.world().state(root)->linear_velocity;
    GIANT_CHECK(f.run("impact", {{"point", {0.0, 0.0, 0.0}}, {"radius", 20.0}, {"impulse", 1e8}}).ok);
    GIANT_CHECK(f.ragdolls.waiting() == 0); // (those lying are thrown at once; the far one is out of reach)
    f.frame();
    const Vec3 thrown = f.physics.world().state(root)->linear_velocity - lying;
    const double outwards = (static_cast<double>(thrown.x) * lay.x + static_cast<double>(thrown.z) * lay.z) /
                            std::sqrt(lay.x * lay.x + lay.z * lay.z);
    GIANT_CHECK(outwards > 1.0 && length(thrown) < 14.0f);
    // (And its pose follows: one asleep is woken.)
    f.frames(0.3);
    GIANT_CHECK(length(f.physics.world().state(root)->position - lay) > 0.2);
    GIANT_CHECK(length(f.ragdolls.poses().at(ring[0]).position - (f.physics.world().state(root)->position - r->root_start + r->start.position)) < 1e-6);
    // Blasts drop none with 0.
    GIANT_CHECK(f.run("set_ragdoll_settings", {{"blast_drop", 0.0}}).ok);
    GIANT_CHECK(f.run("impact", {{"point", {29.0, 1.0, 0.0}}, {"radius", 6.0}, {"impulse", 1e6}}).ok);
    GIANT_CHECK(f.ragdolls.waiting() == 0);
    // One standing dropped by a mighty blast is thrown no faster either.
    GIANT_CHECK(f.run("set_ragdoll_settings", {{"blast_drop", 150.0}}).ok);
    GIANT_CHECK(f.run("impact", {{"point", {29.0, 1.0, 0.0}}, {"radius", 6.0}, {"impulse", 1e6}}).ok);
    GIANT_CHECK(f.ragdolls.waiting() == 1);
    f.frame();
    const RagdollState::Ragdoll* blown = f.ragdolls.find(far);
    GIANT_CHECK(blown != nullptr);
    if (blown == nullptr) return;
    const Vec3 v = f.physics.world().state(f.physics.world().ragdoll_bodies(blown->id)[0])->linear_velocity;
    GIANT_CHECK(v.x > 3.0f && length(v) < 14.0f);
}

GIANT_TEST("ragdolls: a shot and a blast on one both push it; walkers are shot down and blown down; asked at once in a new run, made") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(0.0, 0.0, 0.0);
    const world::EntityId walker = f.person(-10.0, 0.0, 8.0, "none");
    const world::EntityId stroller = f.person(-10.0, 0.0, -8.0, "none");
    f.frame();
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    // Asked before any step of this run (as a command just after `simulate`): made at the first.
    const usize spine = joint_named(f, "Spine");
    GIANT_CHECK(f.ragdolls.knock(f.state.scene.identity(), id, static_cast<u32>(spine), {0.0f, 0.0f, 20.0f}, {0.0, 1.3, 0.0}));
    GIANT_CHECK(!f.ragdolls.knock(f.state.scene.identity(), id, static_cast<u32>(spine), {0.0f, 0.0f, 2.0f}, {0.0, 1.3, 0.0}));
    f.frame();
    GIANT_CHECK(f.ragdolls.find(id) != nullptr && f.ragdolls.dropped() == 1);
    GIANT_CHECK(f.run("stop_ragdoll", {{"id", id}}).ok);
    // One asked in another scene is forgotten, with what was asked before it.
    GIANT_CHECK(f.ragdolls.knock(f.state.scene.identity(), id, static_cast<u32>(spine), {0.0f, 0.0f, 20.0f}, {0.0, 1.3, 0.0}));
    GIANT_CHECK(f.ragdolls.knock(f.state.scene.identity() + 1, id, static_cast<u32>(spine), {0.0f, 0.0f, 20.0f}, {0.0, 1.3, 0.0}));
    GIANT_CHECK(f.ragdolls.waiting() == 1);
    f.frame();
    GIANT_CHECK(f.ragdolls.waiting() == 0 && f.ragdolls.size() == 0);
    f.frames(0.2);
    // A shot, then a blast from its right, before the next step: dropped once, pushed by both.
    GIANT_CHECK(f.ragdolls.knock(f.state.scene.identity(), id, static_cast<u32>(spine), {0.0f, 0.0f, 60.0f}, {0.0, 1.3, 0.0}));
    GIANT_CHECK(f.run("impact", {{"point", {-2.0, 1.0, 0.0}}, {"radius", 8.0}, {"impulse", 800.0}}).ok);
    GIANT_CHECK(f.ragdolls.waiting() == 2);
    f.frame();
    const RagdollState::Ragdoll* r = f.ragdolls.find(id);
    GIANT_CHECK(r != nullptr && f.ragdolls.size() == 1 && f.ragdolls.dropped() == 2 && f.ragdolls.waiting() == 0);
    if (r == nullptr) return;
    const std::span<const sim::BodyId> bodies = f.physics.world().ragdoll_bodies(r->id);
    GIANT_CHECK(f.physics.world().state(bodies[0])->linear_velocity.x > 2.0f);                 // the blast's, on all of it
    GIANT_CHECK(f.physics.world().state(bodies[r->bone_of[spine]])->linear_velocity.z > 0.3f); // the shot's, on its chest
    // A walker shot as it walks (its body is met: it has no capsules): down, pushed on the bone nearest.
    for (const world::EntityId w : {walker, stroller}) {
        const double z = w == walker ? 8.0 : -8.0;
        GIANT_CHECK(f.run("walk_to", {{"id", w}, {"to", {10.0, 0.0, z}}, {"speed", 2.0}}).ok);
    }
    for (int k = 0; k < 600 && f.run("get_walker", {{"id", walker}}).value["walker"]["phase"] != "walking"; ++k) f.frame();
    f.frames(1.0);
    const auto walked = f.walkers.poses().find(walker);
    GIANT_CHECK(walked != f.walkers.poses().end());
    if (walked == f.walkers.poses().end()) return;
    const DVec3 at = walked->second.position;
    const ai::CommandResult fired = f.run("fire", {{"origin", {at.x + 0.2, 1.0, 3.0}}, {"direction", {0.0, 0.0, 1.0}}, {"resolve", true},
                                                    {"mass", 0.05},                     {"penetration", 0.05}});
    GIANT_CHECK(fired.ok && !fired.value["hits"].empty());
    if (!fired.ok || fired.value["hits"].empty()) return;
    GIANT_CHECK(fired.value["hits"][0]["entity"] == walker && fired.value["hits"][0]["character"].get<bool>());
    GIANT_CHECK(f.ragdolls.waiting() == 1);
    // (Made by a fixed step run by hand as well as by a frame.)
    GIANT_CHECK(f.run("step_physics", {{"seconds", 1.0 / 60.0}}).ok);
    const RagdollState::Ragdoll* fell = f.ragdolls.find(walker);
    GIANT_CHECK(fell != nullptr && f.walkers.find(walker) == nullptr && f.ragdolls.waiting() == 0);
    if (fell == nullptr) return;
    f.frames(0.1);
    float most = 0.0f;
    for (const sim::BodyId b : f.physics.world().ragdoll_bodies(fell->id)) most = std::max(most, f.physics.world().state(b)->linear_velocity.z);
    GIANT_CHECK(most > 0.5f);
    // The other blown down by a blast beside it.
    const auto strolled = f.walkers.poses().find(stroller);
    GIANT_CHECK(strolled != f.walkers.poses().end());
    if (strolled == f.walkers.poses().end()) return;
    const DVec3 by = strolled->second.position;
    GIANT_CHECK(f.run("impact", {{"point", {by.x, 1.0, by.z + 1.5}}, {"radius", 6.0}, {"impulse", 800.0}}).ok);
    GIANT_CHECK(f.ragdolls.waiting() == 1);
    f.frame();
    const RagdollState::Ragdoll* blown = f.ragdolls.find(stroller);
    GIANT_CHECK(blown != nullptr && f.walkers.find(stroller) == nullptr);
    if (blown == nullptr) return;
    GIANT_CHECK(f.physics.world().state(f.physics.world().ragdoll_bodies(blown->id)[0])->linear_velocity.z < -2.0f);
}

GIANT_TEST("ragdolls: each bone starts moving as its joint was (an arm on its way up goes on up), over the character's own velocity") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(0.0, 0.0, 0.0);
    const world::EntityId still = f.person(4.0, 0.0, 0.0);
    const world::EntityId cut = f.person(8.0, 0.0, 0.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.3);
    // Its left arm fading up to "raise" (60 degrees in half a second): caught halfway.
    GIANT_CHECK(f.run("play_animation", {{"id", id}, {"clip", "raise"}, {"blend_seconds", 0.5}}).ok);
    f.frames(0.25);
    const usize forearm = joint_named(f, "LeftForeArm");
    const DVec3 was = f.joint(id, forearm);
    f.frame();
    const DVec3 now = f.joint(id, forearm);
    const Vec3 moving = Vec3{static_cast<float>(now.x - was.x), static_cast<float>(now.y - was.y), static_cast<float>(now.z - was.z)} * 60.0f;
    GIANT_CHECK(length(moving) > 0.3f); // (the clip does move it)
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", id}, {"velocity", {0.0, 0.0, 2.0}}}).ok);
    const RagdollState::Ragdoll* r = f.ragdolls.find(id);
    GIANT_CHECK(r != nullptr);
    if (r == nullptr) return;
    const std::span<const sim::BodyId> bodies = f.physics.world().ragdoll_bodies(r->id);
    const sim::BodyState arm = *f.physics.world().state(bodies[r->bone_of[joint_named(f, "LeftArm")]]);
    const sim::BodyState lower = *f.physics.world().state(bodies[r->bone_of[forearm]]);
    const sim::BodyState hips = *f.physics.world().state(bodies[0]);
    // The hips and the other arm at the velocity given; the raised arm's bones over it, the way the joint went, turning.
    GIANT_CHECK(length(hips.linear_velocity - Vec3{0.0f, 0.0f, 2.0f}) < 1e-3f && length(hips.angular_velocity) < 1e-3f);
    const sim::BodyState other = *f.physics.world().state(bodies[r->bone_of[joint_named(f, "RightForeArm")]]);
    GIANT_CHECK(length(other.linear_velocity - Vec3{0.0f, 0.0f, 2.0f}) < 1e-3f);
    const Vec3 own = lower.linear_velocity - Vec3{0.0f, 0.0f, 2.0f};
    GIANT_CHECK(length(own) > 0.3f && dot(own, moving) > 0.7f * length(own) * length(moving));
    GIANT_CHECK(length(own) > length(arm.linear_velocity - Vec3{0.0f, 0.0f, 2.0f})); // farther from the shoulder: faster
    GIANT_CHECK(length(arm.angular_velocity) > 1.0f && length(arm.angular_velocity) < 4.0f); // (about 2 rad/s)
    GIANT_CHECK(length(lower.angular_velocity - arm.angular_velocity) < 0.2f);                // (the elbow does not bend)
    // One standing still starts at rest; so does one whose clip was only just started (a cut: no motion to
    // carry), a frame ago or this very moment, though it was moving until then.
    const world::EntityId cut_now = f.person(12.0, 0.0, 0.0);
    f.frame();
    GIANT_CHECK(f.run("play_animation", {{"id", cut_now}, {"clip", "raise"}, {"blend_seconds", 0.5}}).ok);
    f.frames(0.25);
    GIANT_CHECK(f.run("play_animation", {{"id", cut}, {"clip", "raise"}, {"blend_seconds", 0.0}}).ok);
    f.frame();
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", still}}).ok);
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", cut}}).ok);
    GIANT_CHECK(f.run("play_animation", {{"id", cut_now}, {"clip", "idle"}, {"blend_seconds", 0.0}}).ok);
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", cut_now}}).ok);
    for (const world::EntityId e : {still, cut, cut_now}) {
        const RagdollState::Ragdoll* one = f.ragdolls.find(e);
        GIANT_CHECK(one != nullptr);
        if (one == nullptr) continue;
        for (const sim::BodyId b : f.physics.world().ragdoll_bodies(one->id)) {
            const sim::BodyState s = *f.physics.world().state(b);
            GIANT_CHECK(length(s.linear_velocity) < 1e-3f && length(s.angular_velocity) < 1e-3f);
        }
    }
    // One turned a quarter about: its arm's motion turned with it.
    const world::EntityId turned_one = f.create({{"shape", "mesh"}, {"mesh", f.file}, {"position", {16.0, 0.0, 0.0}},
                                                 {"rotation", {{"yaw", 90.0}}},  {"body", "fixed"}});
    GIANT_CHECK(f.run("set_animation", {{"id", turned_one}, {"clip", "idle"}}).ok);
    f.frames(0.1);
    GIANT_CHECK(f.run("play_animation", {{"id", turned_one}, {"clip", "raise"}, {"blend_seconds", 0.5}}).ok);
    f.frames(0.25);
    const DVec3 from = f.joint(turned_one, forearm);
    f.frame();
    const DVec3 to = f.joint(turned_one, forearm);
    const Vec3 went = Vec3{static_cast<float>(to.x - from.x), static_cast<float>(to.y - from.y), static_cast<float>(to.z - from.z)} * 60.0f;
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", turned_one}}).ok);
    const RagdollState::Ragdoll* q = f.ragdolls.find(turned_one);
    GIANT_CHECK(q != nullptr);
    if (q == nullptr) return;
    const sim::BodyState swung = *f.physics.world().state(f.physics.world().ragdoll_bodies(q->id)[q->bone_of[forearm]]);
    GIANT_CHECK(length(went) > 0.3f && dot(swung.linear_velocity, went) > 0.95f * length(swung.linear_velocity) * length(went));
    GIANT_CHECK(length(swung.linear_velocity) > 0.3f && length(swung.angular_velocity) > 1.0f);
    // (Its spin is about the way it faces now, as its arm rises in the plane across that.)
    const Vec3 facing = rotate(f.state.scene.find(turned_one)->transform.rotation, Vec3{0.0f, 0.0f, 1.0f});
    GIANT_CHECK(std::fabs(facing.x) > 0.9f); // (turned indeed)
    GIANT_CHECK(std::fabs(dot(swung.angular_velocity, facing)) > 0.9f * length(swung.angular_velocity));
    // Faster than a limb moves is no motion but a pop: the same fade on a giant sixty times the size swings
    // its forearm at over 20 m/s, and the whole of it starts at rest.
    const world::EntityId popped = f.create({{"shape", "mesh"}, {"mesh", f.file}, {"position", {200.0, 0.0, 0.0}},
                                             {"scale", {60.0, 60.0, 60.0}},  {"body", "fixed"}});
    GIANT_CHECK(f.run("set_animation", {{"id", popped}, {"clip", "idle"}}).ok);
    const world::EntityId frozen = f.person(24.0, 0.0, 0.0);
    f.frames(0.1);
    for (const world::EntityId e : {popped, frozen}) {
        GIANT_CHECK(f.run("play_animation", {{"id", e}, {"clip", "raise"}, {"blend_seconds", 0.5}}).ok);
    }
    f.frames(0.25);
    for (const world::EntityId e : {popped, frozen}) {
        const AnimationState::Animated* a = f.animation.find(e);
        GIANT_CHECK(a != nullptr && !a->cut && a->moved_seconds > 0.0); // (two poses that follow: motion to carry, were it one)
    }
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", popped}}).ok);
    // The animation paused: one frozen mid-swing starts at rest too.
    f.animation.set_paused(true);
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", frozen}}).ok);
    f.animation.set_paused(false);
    for (const world::EntityId e : {popped, frozen}) {
        const RagdollState::Ragdoll* one = f.ragdolls.find(e);
        GIANT_CHECK(one != nullptr);
        if (one == nullptr) continue;
        for (const sim::BodyId b : f.physics.world().ragdoll_bodies(one->id)) {
            const sim::BodyState state = *f.physics.world().state(b);
            GIANT_CHECK(length(state.linear_velocity) < 1e-3f && length(state.angular_velocity) < 1e-3f);
        }
    }
}

GIANT_TEST("ragdolls: one that can walk gets up where it lies, by command or after lying still, and walks on") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(-10.0, 0.0, 0.0, "none");
    const world::EntityId post = f.person(0.0, 0.0, 12.0); // (a body of its own: it cannot stand elsewhere)
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.5);
    GIANT_CHECK(f.run("walk_to", {{"id", id}, {"to", {10.0, 0.0, 0.0}}, {"speed", 2.0}}).ok);
    for (int k = 0; k < 600 && f.run("get_walker", {{"id", id}}).value["walker"]["phase"] != "walking"; ++k) f.frame();
    f.frames(1.0);
    GIANT_CHECK(!f.run("get_up", {{"id", id}}).ok); // not a ragdoll
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", id}}).ok && f.run("ragdoll_entity", {{"id", post}, {"velocity", {1.0, 0.0, 0.0}}}).ok);
    const auto asleep = [&f](world::EntityId e) {
        const ai::CommandResult got = f.run("get_ragdoll", {{"id", e}});
        return got.ok && got.value["ragdoll"]["asleep"].get<bool>();
    };
    for (int k = 0; k < 60 && !(asleep(id) && asleep(post)); ++k) f.frames(0.25);
    GIANT_CHECK(asleep(id) && asleep(post));
    GIANT_CHECK(f.run("get_ragdoll", {{"id", id}}).value["ragdoll"]["lying"].get<double>() > 0.0);
    // Stirred as it lies (a blast near), it has lain no time.
    const DVec3 stirred = f.joint(id, joint_named(f, "Hips"));
    GIANT_CHECK(f.run("impact", {{"point", {stirred.x, 0.0, stirred.z + 1.0}}, {"radius", 4.0}, {"impulse", 200.0}}).ok);
    f.frames(0.1);
    GIANT_CHECK(!asleep(id) && f.run("get_ragdoll", {{"id", id}}).value["ragdoll"]["lying"] == 0.0);
    for (int k = 0; k < 60 && !asleep(id); ++k) f.frames(0.25);
    GIANT_CHECK(asleep(id));
    const usize hips = joint_named(f, "Hips"), head = joint_named(f, "Head");
    const DVec3 lay_hips = f.joint(id, hips), lay_head = f.joint(id, head);
    GIANT_CHECK(lay_hips.y < 0.6 && lay_head.y < 0.8 && lay_hips.x > -9.0); // down, on from where it was put
    // The way it will face, from how it lies (it faces +z and its left is +x standing: its front is its left
    // crossed with its spine): that way if it is more upright than not, else toward its feet on its back, its head on its front.
    const DVec3 across = f.joint(id, joint_named(f, "LeftUpLeg")) - f.joint(id, joint_named(f, "RightUpLeg"));
    const DVec3 along = lay_head - lay_hips;
    const auto unit = [](const Vec3& v) { return v * (1.0f / std::max(length(v), 1e-6f)); };
    const Vec3 left = unit(Vec3{static_cast<float>(across.x), static_cast<float>(across.y), static_cast<float>(across.z)});
    const Vec3 spine = unit(Vec3{static_cast<float>(along.x), static_cast<float>(along.y), static_cast<float>(along.z)});
    const Vec3 front = unit(cross(left, spine));
    const Vec3 facing = front.x * front.x + front.z * front.z >= 0.25f ? front : (front.y > 0.0f ? spine * -1.0f : spine);
    const float expected_yaw = std::atan2(facing.x, facing.z);
    const auto angle_between = [](float a, float b) { return std::remainder(a - b, 6.2831853f); };
    GIANT_CHECK(std::fabs(spine.y) < 0.5f); // (it does lie)
    // One with a body of its own lies on.
    GIANT_CHECK(!f.run("get_up", {{"id", post}}).ok && f.ragdolls.find(post) != nullptr);
    // Up: a walker standing where it lay, its pose as it lay at first.
    const ai::CommandResult up = f.run("get_up", {{"id", id}});
    GIANT_CHECK(up.ok);
    if (!up.ok) return;
    GIANT_CHECK(f.ragdolls.find(id) == nullptr && !f.animation.holds_pose(id) && f.ragdolls.risen() == 1);
    const WalkerState::Walker* w = f.walkers.find(id);
    GIANT_CHECK(w != nullptr && w->phase == WalkerState::Phase::arrived);
    if (w == nullptr) return;
    GIANT_CHECK(std::fabs(angle_between(w->yaw, expected_yaw)) < 0.6f); // (its hips' own turn, near what its limbs tell)
    GIANT_CHECK(std::fabs(w->feet.x - lay_hips.x) < 0.05 && std::fabs(w->feet.z - lay_hips.z) < 0.05 && std::fabs(w->feet.y) < 0.05);
    GIANT_CHECK(f.physics.world().ragdolls() == 1); // (the post's)
    f.frame();
    GIANT_CHECK(length(f.joint(id, hips) - lay_hips) < 0.08 && length(f.joint(id, head) - lay_head) < 0.08);
    // Struck down again that moment (before the walkers have drawn it once): limp where it stands, not where the scene has it.
    {
        const world::EntityId again = f.person(-10.0, 0.0, 20.0, "none");
        f.frames(0.2);
        GIANT_CHECK(f.run("ragdoll_entity", {{"id", again}, {"velocity", {3.0, 0.0, 0.0}}}).ok);
        for (int k = 0; k < 60 && !asleep(again); ++k) f.frames(0.25);
        const DVec3 fell = f.joint(again, hips);
        GIANT_CHECK(asleep(again) && fell.x > -9.5);
        GIANT_CHECK(f.run("get_up", {{"id", again}}).ok && f.run("ragdoll_entity", {{"id", again}}).ok);
        GIANT_CHECK(length(f.joint(again, hips) - fell) < 0.1);
        f.frame();
        GIANT_CHECK(length(f.joint(again, hips) - fell) < 0.2 && f.walkers.find(again) == nullptr);
    }
    // Then standing as its clip has it, over where its hips were.
    f.frames(0.4);
    const double rising = f.joint(id, head).y;
    GIANT_CHECK(rising > lay_head.y + 0.1 && rising < 1.6);
    f.frames(1.0);
    const DVec3 up_hips = f.joint(id, hips), up_head = f.joint(id, head);
    GIANT_CHECK(std::fabs(up_hips.y - 1.0) < 0.05 && std::fabs(up_head.y - 1.5) < 0.05);
    GIANT_CHECK(std::fabs(up_hips.x - lay_hips.x) < 0.1 && std::fabs(up_hips.z - lay_hips.z) < 0.1);
    GIANT_CHECK(std::fabs(up_head.x - up_hips.x) < 0.02 && std::fabs(up_head.z - up_hips.z) < 0.02); // upright
    // It walks on from there.
    GIANT_CHECK(f.run("walk_to", {{"id", id}, {"to", {10.0, 0.0, 0.0}}, {"speed", 2.0}}).ok);
    for (int k = 0; k < 600 && f.run("get_walker", {{"id", id}}).value["walker"]["phase"] != "walking"; ++k) f.frame();
    f.frames(1.0);
    GIANT_CHECK(f.walkers.poses().at(id).position.x > lay_hips.x + 1.0);
    // Struck down again, it rises by itself after lying still as long as the settings say; the post never.
    GIANT_CHECK(!f.run("set_ragdoll_settings", {{"get_up_fade", 6.0}}).ok);
    const ai::CommandResult set = f.run("set_ragdoll_settings", {{"get_up_seconds", 1.0}, {"get_up_fade", 0.3}});
    GIANT_CHECK(set.ok && set.value["get_up_seconds"] == 1.0);
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", id}}).ok);
    f.frames(0.5);
    GIANT_CHECK(f.ragdolls.find(id) != nullptr); // (still falling)
    for (int k = 0; k < 80 && f.ragdolls.find(id) != nullptr; ++k) f.frames(0.25);
    GIANT_CHECK(f.ragdolls.find(id) == nullptr && f.walkers.find(id) != nullptr && f.ragdolls.risen() >= 3);
    GIANT_CHECK(f.ragdolls.find(post) != nullptr && f.run("get_ragdoll").value["risen"] == f.ragdolls.risen());
    f.frames(1.0);
    GIANT_CHECK(std::fabs(f.joint(id, head).y - 1.5) < 0.05);
}
