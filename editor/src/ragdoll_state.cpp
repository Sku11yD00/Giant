#include "ragdoll_state.h"

#include "pose_math.h"

#include "giant/anim/skeleton.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

using pose::apply;
using pose::stretch;

constexpr float k_radians = 0.017453292f;
// A bone gets a body when its capsule is at least this share of the
// character's size (a hand does, a finger does not).
constexpr float k_bone_share = 0.04f;
constexpr float k_density = 1000.0f; // kg a cubic metre: flesh is about water
// The lightest bone weighs at least this share of the heaviest (a hand
// against a chest: joints between very unlike masses come apart).
constexpr float k_lightest_share = 0.02f;
constexpr float k_min_mass = 0.05f;

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool has(std::string_view name, std::string_view word) { return name.find(word) != std::string_view::npos; }

// A model-space matrix in the entity's own space: scaled as the entity is (not turned, not placed).
Mat4 scaled(const Mat4& m, const Vec3& scale) {
    Mat4 out = m;
    for (int c = 0; c < 4; ++c) {
        out.at(0, c) *= scale.x;
        out.at(1, c) *= scale.y;
        out.at(2, c) *= scale.z;
    }
    return out;
}

Vec3 origin_of(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

// The turn nearest a matrix's (its stretch and any mirror left out).
Quat turn_of(const Mat4& m) {
    const Quat q = anim::decompose(m).rotation;
    const bool sound = std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) &&
                       q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w > 0.5f;
    return sound ? q : Quat{};
}

// The turn that takes +y to `dir` (unit length) the short way.
Quat from_up(const Vec3& dir) {
    if (dir.y < -0.9999f) return quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 3.14159265f);
    const Vec3 axis = cross(Vec3{0.0f, 1.0f, 0.0f}, dir);
    if (!(length(axis) > 1e-6f)) return {};
    return quat_from_axis_angle(axis, std::acos(std::clamp(dir.y, -1.0f, 1.0f)));
}

// How near two segments come (Ericson, Real-Time Collision Detection 5.1.9).
float segment_distance(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s = 0.0f, t = 0.0f;
    constexpr float k_tiny = 1e-12f;
    if (a <= k_tiny && e <= k_tiny) return length(r);
    if (a <= k_tiny) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = dot(d1, r);
        if (e <= k_tiny) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = dot(d1, d2);
            const float denom = a * e - b * b;
            if (denom > k_tiny) s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return length((p1 + d1 * s) - (p2 + d2 * t));
}

// How fast a bone can start, over its character's own speed (an animation's limb: m/s, radians a second). One
// faster is no motion but a pop (a loop's end not meeting its start): the whole character starts as one then.
constexpr float k_max_bone_speed = 20.0f;
constexpr float k_max_bone_spin = 30.0f;

struct Capsule {
    Vec3 a{};
    Vec3 b{};
    float radius = 0.0f;
};

Capsule placed(const anim::BoneCapsule& c, const Mat4& joint) {
    return {apply(joint, c.a), apply(joint, c.b), c.radius * stretch(joint)};
}

bool sound(const Capsule& c) { return finite(c.a) && finite(c.b) && std::isfinite(c.radius) && c.radius > 0.0f; }

float extent(const Capsule& c) { return length(c.b - c.a) + 2.0f * c.radius; }

// What start() makes: the ragdoll's own record and the physics' description.
struct Made {
    RagdollState::Ragdoll ragdoll;
    sim::RagdollDesc desc;
};

// The ragdoll of a character posed as `model` (its joints' model matrices)
// and drawn at `at`, every bone moving at `velocity` and, over that, as its
// joint moved from the pose `before` to this one in `seconds` (0: no such
// pose). Empty when no bone is worth a body.
std::optional<Made> make(const MeshLibrary::Skinned& s, std::span<const Mat4> model, const world::Transform& at,
                         const Vec3& velocity, std::span<const Mat4> before, float seconds) {
    const usize joints = s.skeleton.size();
    if (model.size() != joints || s.rest_model.size() != joints) return std::nullopt;
    // Each joint's capsule (the first, should a file have two), as posed and at rest, in the entity's space.
    constexpr u32 k_none = ~u32{0};
    std::vector<u32> capsule_of(joints, k_none);
    for (u32 c = 0; c < s.capsules.size(); ++c) {
        const u32 j = s.capsules[c].joint;
        if (j < joints && capsule_of[j] == k_none) capsule_of[j] = c;
    }
    std::vector<Mat4> posed(joints), rest(joints);
    for (usize j = 0; j < joints; ++j) {
        posed[j] = scaled(model[j], at.scale);
        rest[j] = scaled(s.rest_model[j], at.scale);
    }
    // The character's size: its capsules' reach at rest.
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (usize j = 0; j < joints; ++j) {
        if (capsule_of[j] == k_none) continue;
        const Capsule c = placed(s.capsules[capsule_of[j]], rest[j]);
        if (!sound(c)) continue;
        for (const Vec3& p : {c.a, c.b}) {
            lo = {std::min(lo.x, p.x - c.radius), std::min(lo.y, p.y - c.radius), std::min(lo.z, p.z - c.radius)};
            hi = {std::max(hi.x, p.x + c.radius), std::max(hi.y, p.y + c.radius), std::max(hi.z, p.z + c.radius)};
        }
    }
    if (!(hi.x >= lo.x)) return std::nullopt; // no capsule at all
    const float size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});

    Made out;
    RagdollState::Ragdoll& r = out.ragdoll;
    sim::RagdollDesc& d = out.desc;
    r.skinned = nullptr; // (the caller's)
    r.start = at;
    r.bone_of.assign(joints, k_none);
    std::vector<Quat> turn;      // per bone: its turn in the entity's space, as posed
    std::vector<Quat> rest_turn; // and at rest
    std::vector<Vec3> origin;    // per bone: its joint's place in the entity's space, as posed
    std::vector<Capsule> at_rest; // per bone
    bool wild = false;            // a bone moved as no limb does between the two poses
    for (u32 j = 0; j < joints; ++j) {
        const i32 parent_joint = s.skeleton.parents[j];
        const u32 above = parent_joint >= 0 ? r.bone_of[static_cast<usize>(parent_joint)] : k_none;
        r.bone_of[j] = above; // (until it has a bone of its own)
        if (capsule_of[j] == k_none || r.bones.size() >= sim::k_max_ragdoll_bones) continue;
        const anim::BoneCapsule& fitted = s.capsules[capsule_of[j]];
        const Capsule c = placed(fitted, posed[j]);
        const Capsule c_rest = placed(fitted, rest[j]);
        if (!sound(c) || !sound(c_rest) || !finite(origin_of(posed[j])) || extent(c_rest) < k_bone_share * size) continue;

        RagdollState::Bone bone;
        bone.joint = j;
        // It hangs from the bone above it, or from the first (a second root: a tail, a prop).
        bone.parent = r.bones.empty() ? -1 : (above != k_none ? static_cast<i32>(above) : 0);
        bone.kind = j < s.skeleton.names.size() ? RagdollState::kind_of(s.skeleton.names[j]) : RagdollState::Kind::other;
        const Quat q = turn_of(posed[j]);
        const Quat q_rest = turn_of(rest[j]);
        const Vec3 o = origin_of(posed[j]);

        sim::RagdollBone b;
        b.parent = bone.parent;
        const Vec3 world = rotate(at.rotation, o);
        b.position = at.position + DVec3{static_cast<double>(world.x), static_cast<double>(world.y),
                                         static_cast<double>(world.z)};
        b.rotation = normalize(at.rotation * q);
        b.linear_velocity = velocity;
        // Its capsule in its own space (the end nearer its joint first).
        Vec3 near = rotate(conjugate(q), c.a - o);
        Vec3 far = rotate(conjugate(q), c.b - o);
        if (dot(far, far) < dot(near, near)) std::swap(near, far);
        const Vec3 along = far - near;
        const float span = length(along);
        const Vec3 middle = (near + far) * 0.5f;
        // Moving as its joint was: its middle's speed and its spin, from the pose before (not for
        // a mirrored character, whose spin that would turn about).
        if (seconds > 0.0f && before.size() == joints && at.scale.x > 0.0f && at.scale.y > 0.0f && at.scale.z > 0.0f) {
            const Mat4 was = scaled(before[j], at.scale);
            const Vec3 o_was = origin_of(was);
            Quat dq = normalize(q * conjugate(turn_of(was)));
            if (dq.w < 0.0f) dq = {-dq.x, -dq.y, -dq.z, -dq.w};
            const Vec3 axis{dq.x, dq.y, dq.z};
            const float sine = length(axis);
            const Vec3 spin = sine > 1e-6f ? axis * (2.0f * std::atan2(sine, dq.w) / (sine * seconds)) : Vec3{};
            const Vec3 speed = (o - o_was) * (1.0f / seconds) + cross(spin, rotate(q, middle));
            // (A joint squashed to nothing in the pose before has no turn to tell a spin from.)
            if (finite(o_was) && finite(speed) && finite(spin) && stretch(was) > 1e-6f && length(speed) <= k_max_bone_speed &&
                length(spin) <= k_max_bone_spin) {
                b.linear_velocity = velocity + rotate(at.rotation, speed);
                b.angular_velocity = rotate(at.rotation, spin);
            } else {
                wild = true;
            }
        }
        if (span > 0.02f * c.radius && span > 1e-4f) {
            const Vec3 dir = along * (1.0f / span);
            b.part = {sim::CapsuleShape{0.5f * span, c.radius}, middle, from_up(dir)};
            b.twist_axis = dir;
        } else {
            b.part = {sim::SphereShape{c.radius}, middle, {}};
            const float out_from = length(middle);
            b.twist_axis = out_from > 1e-4f ? middle * (1.0f / out_from) : Vec3{0.0f, 1.0f, 0.0f};
        }
        const float volume = 3.14159265f * c.radius * c.radius * (span + 4.0f / 3.0f * c.radius);
        bone.mass = std::clamp(k_density * volume, k_min_mass, 1.0e5f);

        // Its joint: a cone about the rest pose, or a hinge folding the one way a limb does.
        const RagdollState::Limits limits = RagdollState::limits_of(bone.kind);
        b.swing_degrees = limits.swing;
        b.twist_degrees = limits.twist;
        if (bone.parent >= 0) {
            Quat centre = normalize(conjugate(rest_turn[static_cast<usize>(bone.parent)]) * q_rest);
            if (limits.fold > 0.0f) {
                // Elbows fold the forearm forward, knees the shin back (the character faces +z):
                // about the axis square to the bone and to that way, as the bone lies at rest.
                const Vec3 lies = rotate(q_rest, b.twist_axis);
                const float ahead = (at.scale.z < 0.0f ? -1.0f : 1.0f) * (bone.kind == RagdollState::Kind::shin ? -1.0f : 1.0f);
                const Vec3 axis = cross(lies, Vec3{0.0f, 0.0f, ahead});
                if (length(axis) > 0.2f) {
                    b.hinge_axis = rotate(conjugate(q_rest), normalize_or_zero(axis));
                    b.swing_degrees = 0.5f * limits.fold;
                    b.side_degrees = 0.0f;
                    centre = normalize(centre * quat_from_axis_angle(b.hinge_axis, 0.5f * limits.fold * k_radians));
                    bone.hinge = true;
                } else {
                    b.swing_degrees = 0.5f * limits.fold; // (pointing fore or aft at rest: which way it folds is not told)
                }
            }
            b.centre = centre;
        }
        r.bone_of[j] = static_cast<u32>(r.bones.size());
        r.bones.push_back(bone);
        d.bones.push_back(b);
        turn.push_back(q);
        rest_turn.push_back(q_rest);
        origin.push_back(o);
        at_rest.push_back(c_rest);
    }
    if (r.bones.empty()) return std::nullopt;
    // Joints above the first bone (an armature's root) ride it.
    for (u32& b : r.bone_of) {
        if (b == k_none) b = 0;
    }
    // No bone far lighter than the heaviest.
    float heaviest = 0.0f;
    for (const RagdollState::Bone& b : r.bones) heaviest = std::max(heaviest, b.mass);
    for (usize i = 0; i < r.bones.size(); ++i) {
        r.bones[i].mass = std::max(r.bones[i].mass, k_lightest_share * heaviest);
        d.bones[i].mass = r.bones[i].mass;
    }
    // Bones that never collide: those that overlap as the character stands
    // at rest (thighs at the hips, a spine's short bones), a bone and the
    // one two above it, and bones hanging from the same one.
    for (u32 i = 0; i < r.bones.size(); ++i) {
        const i32 pi = r.bones[i].parent;
        for (u32 k = i + 1; k < r.bones.size(); ++k) {
            const i32 pk = r.bones[k].parent;
            if (pk == static_cast<i32>(i)) continue; // (a bone and its parent never do)
            const bool siblings = pi == pk;
            const bool two_above = pk >= 0 && r.bones[static_cast<usize>(pk)].parent == static_cast<i32>(i);
            const bool overlap = segment_distance(at_rest[i].a, at_rest[i].b, at_rest[k].a, at_rest[k].b) <
                                 at_rest[i].radius + at_rest[k].radius;
            if (siblings || two_above || overlap) d.no_collide.push_back({i, k});
        }
    }
    // Where each joint is in its bone's space, as posed now: kept as the bone moves.
    r.hold.resize(joints);
    for (usize j = 0; j < joints; ++j) {
        const u32 b = r.bone_of[j];
        r.hold[j] = anim::affine_inverse(mat4_from_transform(origin[b], turn[b], {1.0f, 1.0f, 1.0f})) * posed[j];
    }
    r.root_start = d.bones.front().position;
    r.placed.resize(r.bones.size());
    r.model.assign(model.begin(), model.end());
    if (wild) {
        for (sim::RagdollBone& b : d.bones) {
            b.linear_velocity = velocity;
            b.angular_velocity = {};
        }
    }
    return out;
}

} // namespace

const char* RagdollState::to_string(Kind kind) {
    switch (kind) {
    case Kind::other: return "other";
    case Kind::spine: return "spine";
    case Kind::neck: return "neck";
    case Kind::head: return "head";
    case Kind::collar: return "collar";
    case Kind::upper_arm: return "upper_arm";
    case Kind::forearm: return "forearm";
    case Kind::hand: return "hand";
    case Kind::thigh: return "thigh";
    case Kind::shin: return "shin";
    case Kind::foot: return "foot";
    }
    return "other";
}

RagdollState::Kind RagdollState::kind_of(std::string_view joint_name) {
    std::string name(joint_name);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string_view n = name;
    // (The order matters: a forearm is not an arm, an "UpLeg" not a leg.)
    if (has(n, "head")) return Kind::head;
    if (has(n, "neck")) return Kind::neck;
    if (has(n, "forearm") || has(n, "lowerarm") || has(n, "lower_arm") || has(n, "lower arm") || has(n, "elbow")) {
        return Kind::forearm;
    }
    if (has(n, "shoulder") || has(n, "clavicle") || has(n, "collar")) return Kind::collar;
    if (has(n, "hand") || has(n, "wrist")) return Kind::hand;
    if (has(n, "arm") && !has(n, "armature")) return Kind::upper_arm;
    if (has(n, "thigh") || has(n, "upleg") || has(n, "upperleg") || has(n, "upper_leg") || has(n, "up_leg") ||
        has(n, "upper leg")) {
        return Kind::thigh;
    }
    if (has(n, "calf") || has(n, "shin") || has(n, "knee") || has(n, "leg")) return Kind::shin;
    if (has(n, "foot") || has(n, "ankle") || has(n, "toe")) return Kind::foot;
    if (has(n, "spine") || has(n, "chest") || has(n, "torso") || has(n, "abdomen") || has(n, "waist")) {
        return Kind::spine;
    }
    return Kind::other;
}

RagdollState::Limits RagdollState::limits_of(Kind kind) {
    switch (kind) {
    case Kind::other: return {30.0f, 20.0f, 0.0f};
    case Kind::spine: return {20.0f, 15.0f, 0.0f};
    case Kind::neck: return {30.0f, 30.0f, 0.0f};
    case Kind::head: return {30.0f, 35.0f, 0.0f};
    case Kind::collar: return {15.0f, 10.0f, 0.0f};
    case Kind::upper_arm: return {85.0f, 50.0f, 0.0f};
    case Kind::forearm: return {0.0f, 60.0f, 140.0f};
    case Kind::hand: return {50.0f, 20.0f, 0.0f};
    case Kind::thigh: return {60.0f, 25.0f, 0.0f};
    case Kind::shin: return {0.0f, 5.0f, 140.0f};
    case Kind::foot: return {30.0f, 15.0f, 0.0f};
    }
    return {};
}

RagdollState::RagdollState(PhysicsState& physics, AnimationState& animation, WalkerState* walkers)
    : m_physics(physics), m_animation(animation), m_walkers(walkers), m_clears(physics.world().clears()) {}

RagdollState::~RagdollState() {
    sim::PhysicsWorld& world = m_physics.world();
    if (world.clears() != m_clears) return; // (gone with the world)
    for (const auto& [id, r] : m_ragdolls) world.remove_ragdoll(r.id);
}

const RagdollState::Ragdoll* RagdollState::find(world::EntityId id) const {
    const auto it = m_ragdolls.find(id);
    return it != m_ragdolls.end() ? &it->second : nullptr;
}

void RagdollState::follow_world(const world::Scene& scene, const MeshLibrary& meshes) {
    const u64 clears = m_physics.world().clears();
    if (clears == m_clears) return;
    m_clears = clears;
    // (Their bodies went with the world: only our own records to drop.)
    for (auto& [id, r] : m_ragdolls) r.id = sim::k_no_ragdoll;
    clear(scene, meshes);
}

void RagdollState::drop(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id) {
    const auto it = m_ragdolls.find(id);
    if (it == m_ragdolls.end()) return;
    if (it->second.id != sim::k_no_ragdoll) m_physics.world().remove_ragdoll(it->second.id);
    m_ragdolls.erase(it);
    m_animation.release_pose(scene, meshes, id);
    if (m_poses.erase(id) != 0) m_redraw.push_back(id);
}

bool RagdollState::stop(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id) {
    follow_world(scene, meshes);
    if (!current(scene)) clear(scene, meshes); // (another scene or run: none of them is one any more)
    if (!m_ragdolls.contains(id)) return false;
    drop(scene, meshes, id);
    return true;
}

void RagdollState::clear(const world::Scene& scene, const MeshLibrary& meshes) {
    m_gone.clear();
    for (const auto& [id, r] : m_ragdolls) m_gone.push_back(id);
    for (const world::EntityId id : m_gone) drop(scene, meshes, id);
}

std::string RagdollState::start(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id,
                                std::optional<Vec3> velocity) {
    if (!m_physics.simulating()) return "not simulating (simulate on first): ragdolls fall with the physics";
    const world::Entity* e = scene.find(id);
    if (e == nullptr) return "no entity " + std::to_string(id);
    if (velocity && !(std::fabs(velocity->x) <= 1000.0f && std::fabs(velocity->y) <= 1000.0f && std::fabs(velocity->z) <= 1000.0f)) {
        return "not a velocity (each part within 1000 m/s)";
    }
    follow_world(scene, meshes);
    if (scene.identity() != m_scene || m_physics.runs() != m_run) {
        clear(scene, meshes);
        m_scene = scene.identity();
        m_run = m_physics.runs();
    }
    if (m_ragdolls.contains(id)) return "entity " + std::to_string(id) + " is a ragdoll already";
    if (m_ragdolls.size() >= k_max_ragdolls) return "too many ragdolls (" + std::to_string(k_max_ragdolls) + ")";
    // Where it is drawn now: a walker where it walks.
    world::Transform at = e->transform;
    Vec3 moving = velocity.value_or(Vec3{});
    const WalkerState::Walker* walker = m_walkers != nullptr ? m_walkers->find(id) : nullptr;
    if (walker != nullptr) {
        // (One made a walker this moment, got up or put back, has no pose yet: where it stands.)
        const auto p = m_walkers->poses().find(id);
        at = p != m_walkers->poses().end() ? p->second : WalkerState::stands(*walker);
        if (!velocity) {
            const auto s = m_physics.world().character_state(walker->character);
            moving = s ? s->velocity : walker->velocity;
        }
    }
    for (const float s : {at.scale.x, at.scale.y, at.scale.z}) {
        if (!(std::fabs(s) >= 1e-3f && std::fabs(s) <= 1e3f)) return "entity " + std::to_string(id) + " has a scale of nothing (or past 1000)";
    }
    const bool held_before = m_animation.holds_pose(id);
    if (const std::string error = m_animation.hold_pose(scene, meshes, id); !error.empty()) return error;
    const AnimationState::Animated* a = m_animation.find(id);
    // How its joints were moving, when its last two poses tell (worked out lately, a moment apart, no cut between
    // them or since; not while the animation is paused: it stands frozen).
    std::span<const Mat4> before;
    float gap = 0.0f;
    if (a != nullptr && !held_before && !a->cut && !m_animation.paused() && a->moved_seconds >= 1e-3 && a->moved_seconds <= 0.5 && a->since_model <= 0.25 &&
        a->before.size() == a->model.size()) {
        before = a->before;
        gap = static_cast<float>(a->moved_seconds);
    }
    std::optional<Made> made = a != nullptr ? make(*a->skinned, a->model, at, moving, before, gap) : std::nullopt;
    sim::RagdollId ragdoll = sim::k_no_ragdoll;
    if (made) {
        made->desc.user = id; // a shot or a ray that meets a bone names the entity
        ragdoll = m_physics.world().add_ragdoll(made->desc);
    }
    if (ragdoll == sim::k_no_ragdoll) {
        if (!held_before) m_animation.release_pose(scene, meshes, id);
        return made ? "the physics refused its ragdoll (the world is full, or its pose is not one)"
                    : "entity " + std::to_string(id) + "'s file has no bones to fall (no capsules fitted round its joints)";
    }
    if (walker != nullptr) m_walkers->stop(id); // it walks no more
    Ragdoll& r = m_ragdolls[id];
    r = std::move(made->ragdoll);
    r.entity = id;
    r.id = ragdoll;
    r.skinned = a->skinned;
    m_poses[id] = at;
    m_redraw.push_back(id); // its cell built again: drawn as moving from now
    if (m_targets != nullptr) m_targets->forget(id); // bullets meet its bones from now, not its capsules too
    return {};
}

std::optional<RagdollState::Resume> RagdollState::lies(const world::Scene& scene, world::EntityId id) const {
    const auto it = current(scene) && m_physics.world().clears() == m_clears ? m_ragdolls.find(id) : m_ragdolls.end();
    if (it == m_ragdolls.end()) return std::nullopt;
    const Ragdoll& r = it->second;
    const std::span<const sim::BodyId> bodies = m_physics.world().ragdoll_bodies(r.id);
    if (bodies.size() != r.bones.size() || bodies.empty()) return std::nullopt;
    Resume out;
    out.joints.reserve(bodies.size());
    out.bones.reserve(bodies.size());
    for (usize b = 0; b < bodies.size(); ++b) {
        const std::optional<sim::BodyState> s = m_physics.world().state(bodies[b]);
        if (!s) return std::nullopt;
        out.joints.push_back(r.bones[b].joint);
        out.bones.push_back(*s);
    }
    out.lying = r.lying;
    return out;
}

bool RagdollState::ready(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id) const {
    const world::Entity* e = scene.find(id);
    if (e == nullptr || e->shape != world::Shape::mesh || !e->visible) return false;
    const MeshLibrary::Mesh* mesh = meshes.find_normalized(e->mesh);
    return mesh != nullptr && mesh->skinned != nullptr;
}

std::string RagdollState::resume(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id, const Resume& resume) {
    if (resume.bones.empty() || resume.bones.size() != resume.joints.size() || !(resume.lying >= 0.0)) return "not a ragdoll's bones";
    // (start would stop a walker before the bones are known to be the same.)
    if (m_walkers != nullptr && m_walkers->find(id) != nullptr) return "entity " + std::to_string(id) + " is walking";
    // At rest as it is drawn now, then put as it lay.
    if (const std::string error = start(scene, meshes, id, Vec3{}); !error.empty()) return error;
    Ragdoll& r = m_ragdolls.at(id);
    bool same = r.bones.size() == resume.bones.size();
    for (usize b = 0; same && b < r.bones.size(); ++b) same = r.bones[b].joint == resume.joints[b];
    if (!same || !m_physics.world().place_ragdoll(r.id, resume.bones, true)) {
        drop(scene, meshes, id);
        return same ? "the physics refused where its bones lay (a number that is none)"
                    : "entity " + std::to_string(id) + "'s file makes other bones now than those that lay";
    }
    // Drawn from where its first bone was made (start's pose), carried to where it lies.
    r.lying = resume.lying;
    pose(r);
    return {};
}

std::string RagdollState::set_settings(const Settings& settings) {
    if (!(settings.shot_drop >= 0.0f && settings.shot_drop <= 1e6f)) return "shot_drop is not within 0 to 1e6 N s";
    if (!(settings.blast_drop >= 0.0f && settings.blast_drop <= 1e6f)) return "blast_drop is not within 0 to 1e6 N s";
    if (settings.starts_per_step < 1 || settings.starts_per_step > 64) return "starts_per_step is not within 1 to 64";
    if (!(settings.get_up_seconds >= 0.0f && settings.get_up_seconds <= 1e6f)) return "get_up_seconds is not within 0 to 1e6";
    if (!(settings.get_up_fade >= 0.0f && settings.get_up_fade <= 5.0f)) return "get_up_fade is not within 0 to 5 s";
    m_settings = settings;
    return {};
}

std::string RagdollState::joint_name(world::EntityId id, u32 bone) const {
    const auto it = m_ragdolls.find(id);
    if (it == m_ragdolls.end() || bone >= it->second.bones.size()) return {};
    const std::vector<std::string>& names = it->second.skinned->skeleton.names;
    const u32 joint = it->second.bones[bone].joint;
    return joint < names.size() ? names[joint] : std::string{};
}

namespace {

bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// How fast a blast throws a body, at most (m/s): its impulse is a game's number, not a body's.
constexpr float k_max_throw = 12.0f;

// How far a point is from a capsule's skin (0: inside).
double reach_of(const DVec3& point, const sim::HitCapsule& c) {
    const DVec3 ab = c.b - c.a;
    const double l2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    const DVec3 ap = point - c.a;
    const double t = l2 > 0.0 ? std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / l2, 0.0, 1.0) : 0.0;
    const DVec3 d = c.a + ab * t - point;
    return std::max(0.0, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) - static_cast<double>(c.radius));
}

double far_of(const DVec3& d) { return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); }

// The way from a blast to what it throws (up, for what sits on it).
Vec3 away_of(const DVec3& d, double far) {
    return far > 1e-4 ? Vec3{static_cast<float>(d.x / far), static_cast<float>(d.y / far), static_cast<float>(d.z / far)}
                      : Vec3{0.0f, 1.0f, 0.0f};
}

} // namespace

void RagdollState::strike(const Ragdoll& r, const Waiting& w) {
    sim::PhysicsWorld& world = m_physics.world();
    const std::span<const sim::BodyId> bodies = world.ragdoll_bodies(r.id);
    if (bodies.size() != r.bones.size() || bodies.empty()) return;
    if (!w.whole) {
        if (w.joint < r.bone_of.size()) {
            world.add_impulse(bodies[r.bone_of[w.joint]], w.impulse, w.point);
        } else if (w.joint == k_nearest_bone) {
            usize nearest = 0;
            double least = 0.0;
            bool found = false;
            for (usize b = 0; b < bodies.size(); ++b) {
                const std::optional<sim::BodyState> s = world.state(bodies[b]);
                if (!s) continue;
                const double far = far_of(s->position - w.point);
                if (!found || far < least) {
                    least = far;
                    nearest = b;
                    found = true;
                }
            }
            if (found) world.add_impulse(bodies[nearest], w.impulse, w.point);
        }
        return;
    }
    // A blast's on one it dropped: its bones thrown one way, each by its share.
    float mass = 0.0f;
    for (const Bone& b : r.bones) mass += b.mass;
    if (!(mass > 0.0f)) return;
    const float taken = std::min(w.blast, mass * k_max_throw) * w.reach;
    for (usize b = 0; b < bodies.size(); ++b) {
        if (const std::optional<sim::BodyState> s = world.state(bodies[b])) {
            world.add_impulse(bodies[b], w.impulse * (taken * r.bones[b].mass / mass), s->position);
        }
    }
}

void RagdollState::ask_in(u64 scene) {
    if (scene == m_waiting_scene && m_physics.runs() == m_waiting_run) return;
    m_waiting.clear(); // (asked in another scene or run: never made)
    m_waiting_scene = scene;
    m_waiting_run = m_physics.runs();
}

bool RagdollState::knock(u64 scene, world::EntityId id, u32 joint, const Vec3& impulse, const DVec3& point) {
    if (!m_physics.simulating() || !(m_settings.shot_drop > 0.0f) || !finite(impulse) || !finite(point)) return false;
    if (!(length(impulse) >= m_settings.shot_drop)) return false;
    ask_in(scene);
    if (m_waiting.size() >= k_max_waiting) return false;
    Waiting& w = m_waiting.emplace_back();
    w.entity = id;
    w.joint = joint;
    w.impulse = impulse;
    w.point = point;
    return true;
}

RagdollState::Blasted RagdollState::blast(u64 scene, const sim::HitShapes& standing, const DVec3& point, float radius,
                                          float impulse) {
    Blasted out;
    if (!m_physics.simulating() || !std::isfinite(radius) || !(radius > 0.0f) || !std::isfinite(impulse) ||
        !(impulse > 0.0f) || !finite(point)) {
        return out;
    }
    ask_in(scene);
    // Those lying about (of this scene and run: others are let go at the next step): each bone thrown from the blast.
    sim::PhysicsWorld& world = m_physics.world();
    const bool ours = scene == m_scene && m_physics.runs() == m_run && world.clears() == m_clears;
    if (ours) {
        for (const auto& [id, r] : m_ragdolls) {
            const std::span<const sim::BodyId> bodies = world.ragdoll_bodies(r.id);
            if (bodies.size() != r.bones.size()) continue;
            float mass = 0.0f;
            for (const Bone& b : r.bones) mass += b.mass;
            if (!(mass > 0.0f)) continue;
            const float most = std::min(impulse, mass * k_max_throw);
            bool thrown = false;
            for (usize b = 0; b < bodies.size(); ++b) {
                const std::optional<sim::BodyState> s = world.state(bodies[b]);
                if (!s) continue;
                const DVec3 d = s->position - point;
                const double far = far_of(d);
                if (far >= static_cast<double>(radius)) continue;
                const float taken = most * (1.0f - static_cast<float>(far) / radius) * (r.bones[b].mass / mass);
                world.add_impulse(bodies[b], away_of(d, far) * taken, s->position);
                thrown = true;
            }
            out.thrown += thrown ? 1u : 0u;
        }
    }
    if (!(m_settings.blast_drop > 0.0f)) return out;
    // Those standing it reaches hard enough: to drop, thrown from it.
    const auto drop = [&](world::EntityId id, double nearest, const DVec3& centre) {
        if (!(nearest < static_cast<double>(radius)) || m_waiting.size() >= k_max_waiting) return;
        const float reach = 1.0f - static_cast<float>(nearest) / radius;
        if (impulse * reach < m_settings.blast_drop) return;
        const DVec3 d = centre - point;
        Waiting& w = m_waiting.emplace_back();
        w.entity = id;
        w.whole = true;
        w.impulse = away_of(d, far_of(d));
        w.point = point;
        w.blast = impulse;
        w.reach = reach;
        ++out.dropped;
    };
    for (const auto& [id, serial] : m_animation.skinned()) {
        if (ours && m_ragdolls.contains(id)) continue;
        if (const sim::HitCharacter* c = standing.find(id); c != nullptr && !c->capsules.empty()) {
            double nearest = static_cast<double>(radius);
            DVec3 centre{};
            for (const sim::HitCapsule& capsule : c->capsules) {
                nearest = std::min(nearest, reach_of(point, capsule));
                centre = centre + (capsule.a + capsule.b) * (0.5 / static_cast<double>(c->capsules.size()));
            }
            drop(id, nearest, centre);
        } else if (m_walkers != nullptr && m_walkers->find(id) != nullptr) {
            // A walker (no capsules: bullets meet its body): by its middle, a metre up at scale 1, half a metre round.
            const auto p = m_walkers->poses().find(id);
            if (p == m_walkers->poses().end()) continue;
            const double size = static_cast<double>(std::fabs(p->second.scale.y));
            const DVec3 centre = p->second.position + DVec3{0.0, size, 0.0};
            drop(id, std::max(0.0, far_of(centre - point) - 0.5 * size), centre);
        }
    }
    return out;
}

void RagdollState::make_waiting(const world::Scene& scene, const MeshLibrary& meshes) {
    u32 started = 0;
    usize done = 0;
    for (; done < m_waiting.size(); ++done) {
        const Waiting w = m_waiting[done]; // (a copy: nothing here leans on start leaving the list be)
        auto it = current(scene) ? m_ragdolls.find(w.entity) : m_ragdolls.end();
        if (it == m_ragdolls.end()) {
            if (started >= m_settings.starts_per_step) break; // the rest wait their turn
            ++started; // (one that cannot be made counts too: the look cost its part)
            if (!start(scene, meshes, w.entity).empty()) continue; // gone, hidden, too many: it stands
            ++m_dropped;
            it = m_ragdolls.find(w.entity);
            if (it == m_ragdolls.end()) continue;
        }
        strike(it->second, w); // (one lying already, dropped by an earlier one: pushed all the same)
    }
    m_waiting.erase(m_waiting.begin(), m_waiting.begin() + static_cast<std::ptrdiff_t>(done));
}

void RagdollState::before_step(const world::Scene& scene, const MeshLibrary& meshes, double seconds) {
    // Drops asked for in another scene or run, or with the simulation stopped since: never made.
    if (scene.identity() != m_waiting_scene || !m_physics.simulating() || m_physics.runs() != m_waiting_run) m_waiting.clear();
    follow(scene, meshes);
    if (!m_waiting.empty()) make_waiting(scene, meshes);
    // How long each has lain still; those long enough get up (those that can: a couple a step).
    if (m_ragdolls.empty() || !(seconds > 0.0)) return;
    std::array<world::EntityId, 2> rise{};
    usize rising = 0;
    for (auto& [id, r] : m_ragdolls) {
        r.lying = r.asleep ? r.lying + seconds : 0.0;
        if (m_settings.get_up_seconds > 0.0f && r.lying >= static_cast<double>(m_settings.get_up_seconds) &&
            rising < rise.size() && m_walkers != nullptr) {
            const world::Entity* e = scene.find(id);
            if (e == nullptr || e->body != world::BodyKind::none) continue;
            // (Struck since it was last looked at, a blast's or a shot's this step: it lies on.)
            const std::span<const sim::BodyId> bodies = m_physics.world().ragdoll_bodies(r.id);
            const std::optional<sim::BodyState> root = !bodies.empty() ? m_physics.world().state(bodies.front()) : std::nullopt;
            if (!root || root->active) {
                r.lying = 0.0;
                continue;
            }
            rise[rising++] = id;
        }
    }
    for (usize k = 0; k < rising; ++k) {
        // (One the walkers refuse lies on, and is asked again only after as long.)
        if (!get_up(scene, meshes, rise[k]).empty()) {
            if (const auto it = m_ragdolls.find(rise[k]); it != m_ragdolls.end()) it->second.lying = 0.0;
        }
    }
}

std::string RagdollState::get_up(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id) {
    follow_world(scene, meshes);
    const auto it = current(scene) ? m_ragdolls.find(id) : m_ragdolls.end();
    if (it == m_ragdolls.end()) return "entity " + std::to_string(id) + " is not a ragdoll";
    if (m_walkers == nullptr) return "walkers are not available: one that gets up stands as a walker";
    const world::Entity* e = scene.find(id);
    if (e == nullptr) return "no entity " + std::to_string(id);
    if (e->body != world::BodyKind::none) {
        return "entity " + std::to_string(id) + " has a body where the scene has it (only one with body \"none\" gets up, as a walker)";
    }
    if (m_walkers->find(id) == nullptr && m_walkers->size() >= WalkerState::k_max_walkers) {
        return "too many walkers (" + std::to_string(WalkerState::k_max_walkers) + "): it lies on";
    }
    Ragdoll& r = it->second;
    sim::PhysicsWorld& world = m_physics.world();
    const std::span<const sim::BodyId> bodies = world.ragdoll_bodies(r.id);
    const std::optional<sim::BodyState> root = bodies.size() == r.bones.size() ? world.state(bodies.front()) : std::nullopt;
    if (!root || r.bones.empty() || r.bones.front().joint >= r.skinned->rest_model.size()) {
        return "entity " + std::to_string(id) + "'s ragdoll has gone";
    }
    r.asleep = false;
    r.posed = false;
    pose(r); // as its bones are now
    const auto drawn = m_poses.find(id);
    if (drawn == m_poses.end() || !r.posed) return "entity " + std::to_string(id) + "'s ragdoll has gone";
    const world::Transform lay = drawn->second;
    // The way its hips face and its spine points, by how its first bone has turned from rest.
    const Quat rest = turn_of(scaled(r.skinned->rest_model[r.bones.front().joint], r.start.scale));
    const Quat turned = normalize(root->rotation * conjugate(rest));
    const Vec3 forward = rotate(turned, Vec3{0.0f, 0.0f, 1.0f});
    const Vec3 spine = rotate(turned, Vec3{0.0f, 1.0f, 0.0f});
    // Standing it faces that way; lying on its back it sits up toward its feet, on its front it rises toward its head.
    Vec3 facing = forward;
    if (forward.x * forward.x + forward.z * forward.z < 0.25f) facing = forward.y > 0.0f ? spine * -1.0f : spine;
    const Vec3 before = rotate(lay.rotation, Vec3{0.0f, 0.0f, 1.0f});
    if (facing.x * facing.x + facing.z * facing.z < 1e-4f) facing = before; // (on its head: as it was put)
    const float yaw = std::atan2(facing.x, facing.z);
    // Its feet come on what is under its hips for good (the ground, what stands fixed: not another lying there).
    DVec3 feet = root->position - DVec3{0.0, 0.1, 0.0};
    sim::PhysicsWorld::RayFilter standing;
    standing.fixed_only = true;
    if (const auto ground = world.raycast(root->position + DVec3{0.0, 0.3, 0.0}, Vec3{0.0f, -1.0f, 0.0f}, 3.0, standing)) {
        feet = ground->point;
    }
    WalkerState::Resume stand;
    stand.feet = feet;
    stand.yaw = yaw;
    stand.target = feet;
    stand.phase = WalkerState::Phase::arrived;
    m_walkers->stop(id);
    const std::string refused = m_walkers->resume(scene, id, stand);
    const WalkerState::Walker* w = refused.empty() ? m_walkers->find(id) : nullptr;
    if (w == nullptr) return refused.empty() ? "the walkers refused entity " + std::to_string(id) : refused; // it lies on
    world.remove_ragdoll(r.id); // (its bodies and its character never met: no step between)
    r.id = sim::k_no_ragdoll;
    // Where it is drawn standing (as the walkers draw it), and its pose as it lay seen from there.
    const world::Transform stood = WalkerState::stands(*w);
    const Quat back = conjugate(stood.rotation);
    const DVec3 from = lay.position - stood.position;
    const Mat4 across = mat4_from_transform(rotate(back, Vec3{static_cast<float>(from.x), static_cast<float>(from.y), static_cast<float>(from.z)}),
                                            normalize(back * lay.rotation), {1.0f, 1.0f, 1.0f});
    const Vec3 shrink{1.0f / stood.scale.x, 1.0f / stood.scale.y, 1.0f / stood.scale.z};
    for (Mat4& m : r.model) m = scaled(across * scaled(m, lay.scale), shrink);
    m_animation.release_pose(scene, meshes, id, r.model, m_settings.get_up_fade);
    m_ragdolls.erase(it);
    m_poses.erase(id);
    m_redraw.push_back(id);
    ++m_risen;
    return {};
}

void RagdollState::follow(const world::Scene& scene, const MeshLibrary& meshes) {
    if (m_ragdolls.empty()) {
        m_clears = m_physics.world().clears();
        return;
    }
    follow_world(scene, meshes);
    if (!m_physics.simulating() || scene.identity() != m_scene || m_physics.runs() != m_run) {
        clear(scene, meshes);
        m_scene = scene.identity();
        return;
    }
    // Gone (removed, hidden, drawing another file: its held pose went with its copy): let go.
    m_gone.clear();
    for (const auto& [id, r] : m_ragdolls) {
        const AnimationState::Animated* a = m_animation.find(id);
        const world::Entity* e = scene.find(id);
        if (e == nullptr || !e->visible || a == nullptr || !a->held_pose || a->skinned != r.skinned ||
            !m_physics.world().contains_ragdoll(r.id)) {
            m_gone.push_back(id);
        }
    }
    for (const world::EntityId id : m_gone) drop(scene, meshes, id);
    // One sent walking since (a game's walkers put back): it lies where it fell, and walks no more.
    if (m_walkers != nullptr && m_walkers->size() > 0) {
        for (const auto& [id, r] : m_ragdolls) m_walkers->stop(id);
    }
}

void RagdollState::pose(Ragdoll& r) {
    const sim::PhysicsWorld& world = m_physics.world();
    const std::span<const sim::BodyId> bodies = world.ragdoll_bodies(r.id);
    if (bodies.size() != r.bones.size()) return;
    const std::optional<sim::BodyState> root = world.state(bodies.front());
    if (!root) return;
    r.asleep = !root->active;
    if (r.asleep && r.posed) return; // (its bones sleep together: nothing moved)
    // Drawn where its first bone has carried it.
    world::Transform at = r.start;
    at.position = r.start.position + (root->position - r.root_start);
    const Quat back = conjugate(r.start.rotation);
    const Vec3 shrink{1.0f / r.start.scale.x, 1.0f / r.start.scale.y, 1.0f / r.start.scale.z};
    for (usize b = 0; b < bodies.size(); ++b) {
        const std::optional<sim::BodyState> s = b == 0 ? root : world.state(bodies[b]);
        if (!s) return;
        const DVec3 from = s->position - at.position;
        const Vec3 rel = rotate(back, Vec3{static_cast<float>(from.x), static_cast<float>(from.y), static_cast<float>(from.z)});
        r.placed[b] = scaled(mat4_from_transform(rel, normalize(back * s->rotation), {1.0f, 1.0f, 1.0f}), shrink);
    }
    for (usize j = 0; j < r.model.size(); ++j) r.model[j] = r.placed[r.bone_of[j]] * r.hold[j];
    m_animation.set_held_pose(r.entity, r.model, &at.position);
    r.posed = true;
    world::Transform& drawn = m_poses[r.entity];
    if (!(drawn.position == at.position)) {
        drawn = at;
        m_moved.push_back(r.entity);
    }
}

void RagdollState::after_step() {
    if (m_ragdolls.empty() || m_physics.world().clears() != m_clears) return; // (forgotten at the next look)
    for (auto& [id, r] : m_ragdolls) pose(r);
}

std::vector<world::EntityId> RagdollState::take_moved() {
    std::vector<world::EntityId> out;
    out.swap(m_moved);
    return out;
}

std::vector<world::EntityId> RagdollState::take_redraw() {
    std::vector<world::EntityId> out;
    out.swap(m_redraw);
    return out;
}

} // namespace giant::editor
