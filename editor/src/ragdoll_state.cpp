#include "ragdoll_state.h"

#include "pose_math.h"

#include "giant/anim/skeleton.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
// and drawn at `at`. Empty when no bone is worth a body.
std::optional<Made> make(const MeshLibrary::Skinned& s, std::span<const Mat4> model, const world::Transform& at,
                         const Vec3& velocity) {
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
        if (const auto p = m_walkers->poses().find(id); p != m_walkers->poses().end()) at = p->second;
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
    std::optional<Made> made = a != nullptr ? make(*a->skinned, a->model, at, moving) : std::nullopt;
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
    return {};
}

void RagdollState::before_step(const world::Scene& scene, const MeshLibrary& meshes) {
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
