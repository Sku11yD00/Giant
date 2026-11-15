#include "hair_state.h"

#include "pose_math.h"

#include "giant/core/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>

namespace giant::editor {

namespace {

// What a groom is grown from: a change to any of it grows another.
struct Seed {
    world::Shape shape = world::Shape::cube;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    i32 strands = 0;
    i32 guides = 0;
    float top = 0.0f;
    float length = 0.0f;
    float length_variation = 0.0f;
    float droop = 0.0f;
    float curl_radius = 0.0f;
    float curl_turns = 0.0f;
    float frizz = 0.0f;
    float clump = 0.0f;
    i32 seed = 0;
    const void* character = nullptr; // a mesh entity's skinned file as loaded (resolve)
    u32 joint = 0;                   // and its joint
    bool operator==(const Seed&) const = default;
};

Seed seed_of(const world::Entity& e) {
    const world::Hair& h = e.hair;
    return {e.shape,     e.transform.scale, h.strands,     h.guides, h.top,   h.length, h.length_variation,
            h.droop,     h.curl_radius,     h.curl_turns,  h.frizz,  h.clump, h.seed,   nullptr,
            0};
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool wears(const world::Entity& e) {
    // (A scale that is no number grows nothing, and would never equal itself.)
    // (A mesh entity may be a character: known once its file is found, resolve.)
    return e.visible && e.hair.on &&
           (e.shape == world::Shape::cube || e.shape == world::Shape::sphere || e.shape == world::Shape::mesh) &&
           finite(e.transform.scale);
}

using pose::apply;
using pose::stretch;

// A direction through a matrix (no move).
Vec3 turn(const Mat4& m, const Vec3& d) {
    return {m.at(0, 0) * d.x + m.at(0, 1) * d.y + m.at(0, 2) * d.z, m.at(1, 0) * d.x + m.at(1, 1) * d.y + m.at(1, 2) * d.z,
            m.at(2, 0) * d.x + m.at(2, 1) * d.y + m.at(2, 2) * d.z};
}

float largest(const Vec3& scale) { return std::max({std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)}); }

geometry::Vertex between(const geometry::Vertex& a, const geometry::Vertex& b, float t) {
    geometry::Vertex v = a;
    v.position = a.position + (b.position - a.position) * t;
    v.normal = a.normal + (b.normal - a.normal) * t;
    v.uv = {a.uv.u + (b.uv.u - a.uv.u) * t, a.uv.v + (b.uv.v - a.uv.v) * t};
    return v;
}

// A triangle cut at the height `from`: what is at or above it, 0, 3 or 4
// corners in the triangle's own order.
usize cut_above(const geometry::Vertex* const v[3], float from, std::array<geometry::Vertex, 4>& kept) {
    const auto above = [from](const geometry::Vertex& c) { return c.position.y >= from - 1e-6f; };
    usize count = 0;
    for (usize c = 0; c < 3; ++c) {
        const geometry::Vertex& a = *v[c];
        const geometry::Vertex& b = *v[(c + 1) % 3];
        const bool a_in = above(a);
        const bool b_in = above(b);
        if (a_in && count < kept.size()) kept[count++] = a;
        if (a_in != b_in && count < kept.size()) {
            const float fall = b.position.y - a.position.y;
            kept[count++] = between(a, b, fall != 0.0f ? std::clamp((from - a.position.y) / fall, 0.0f, 1.0f) : 0.0f);
        }
    }
    return count;
}

} // namespace

geometry::TriangleMesh hair_scalp(world::Shape shape, const Vec3& scale, float top) {
    geometry::TriangleMesh whole;
    if (shape == world::Shape::cube) {
        whole = geometry::make_cube();
    } else if (shape == world::Shape::sphere) {
        whole = geometry::make_sphere();
    } else {
        return {};
    }
    const float share = std::isfinite(top) ? std::clamp(top, 0.01f, 1.0f) : 0.5f;
    const Vec3 size{std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)};
    const bool sized = finite(size) && size.x > 0.0f && size.y > 0.0f && size.z > 0.0f;
    // (The shapes are 1 m, centred: from y = 0.5 down.)
    const float from = 0.5f - share;
    geometry::TriangleMesh scalp;
    const auto put = [&](geometry::Vertex v) {
        v.position = {v.position.x * size.x, v.position.y * size.y, v.position.z * size.z};
        // (A sized shape's normals turn the other way: by the inverse.)
        const Vec3 n = sized ? normalize_or_zero(Vec3{v.normal.x / size.x, v.normal.y / size.y, v.normal.z / size.z})
                             : normalize_or_zero(v.normal);
        v.normal = n.x == 0.0f && n.y == 0.0f && n.z == 0.0f ? Vec3{0.0f, 1.0f, 0.0f} : n;
        scalp.indices.push_back(static_cast<u32>(scalp.vertices.size()));
        scalp.vertices.push_back(v);
    };
    for (usize i = 0; i + 2 < whole.indices.size(); i += 3) {
        const geometry::Vertex* v[3] = {&whole.vertices[whole.indices[i]], &whole.vertices[whole.indices[i + 1]],
                                        &whole.vertices[whole.indices[i + 2]]};
        std::array<geometry::Vertex, 4> kept;
        const usize count = cut_above(v, from, kept);
        for (usize c = 2; c < count; ++c) {
            put(kept[0]);
            put(kept[c - 1]);
            put(kept[c]);
        }
    }
    return scalp;
}

geometry::TriangleMesh hair_scalp(const MeshLibrary::Skinned& character, u32 joint, float scale, float top) {
    geometry::TriangleMesh scalp;
    const anim::Skeleton& skeleton = character.skeleton;
    const geometry::ClusterMesh& mesh = character.clusters.mesh;
    const std::vector<geometry::SkinWeights>& weights = character.clusters.weights;
    if (joint >= skeleton.size() || joint >= skeleton.inverse_bind.size() || weights.size() != mesh.vertices.size() ||
        !std::isfinite(scale)) {
        return scalp;
    }
    // The joint and those under it (parents come before their children).
    std::vector<u8> under(skeleton.size(), 0);
    for (usize j = 0; j < skeleton.size(); ++j) {
        const i32 parent = skeleton.parents[j];
        under[j] = j == joint || (parent >= 0 && static_cast<usize>(parent) < j && under[static_cast<usize>(parent)] != 0) ? 1 : 0;
    }
    // The share of a vertex's weight they hold.
    const auto held = [&](u32 vertex) {
        const geometry::SkinWeights& w = weights[vertex];
        u32 in = 0;
        u32 all = 0;
        for (usize k = 0; k < w.joints.size(); ++k) {
            all += w.weights[k];
            if (w.joints[k] < under.size() && under[w.joints[k]] != 0) in += w.weights[k];
        }
        return all > 0 ? static_cast<float>(in) / static_cast<float>(all) : 0.0f;
    };
    // The skin they move: the original triangles held by half or more, as
    // the joint holds them at rest in model space (y up, true sizes,
    // whatever way the file's bind space or the joint's own axes turn),
    // about the joint.
    if (joint >= character.rest_model.size()) return scalp;
    const Mat4 to_rest = character.rest_model[joint] * skeleton.inverse_bind[joint];
    const Vec3 origin = apply(character.rest_model[joint], Vec3{});
    const float size = std::fabs(scale);
    std::vector<geometry::Vertex> skin;
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    for (const geometry::Cluster& cluster : mesh.clusters) {
        if (cluster.group >= mesh.groups.size() || mesh.groups[cluster.group].depth != 0) continue;
        for (u32 t = 0; t < cluster.triangle_count; ++t) {
            const usize at = static_cast<usize>(cluster.first_index) + static_cast<usize>(t) * 3;
            if (at + 2 >= mesh.indices.size()) break;
            const u32 corners[3] = {mesh.indices[at], mesh.indices[at + 1], mesh.indices[at + 2]};
            if (corners[0] >= weights.size() || corners[1] >= weights.size() || corners[2] >= weights.size()) continue;
            if (held(corners[0]) + held(corners[1]) + held(corners[2]) < 1.5f) continue;
            for (const u32 corner : corners) {
                geometry::Vertex v = mesh.vertices[corner];
                v.position = apply(to_rest, v.position) - origin;
                const Vec3 n = normalize_or_zero(turn(to_rest, v.normal));
                v.normal = n.x == 0.0f && n.y == 0.0f && n.z == 0.0f ? Vec3{0.0f, 1.0f, 0.0f} : n;
                if (!finite(v.position)) v.position = {};
                low = std::min(low, v.position.y);
                high = std::max(high, v.position.y);
                skin.push_back(v);
            }
        }
    }
    if (skin.empty()) return scalp;
    const float share = std::isfinite(top) ? std::clamp(top, 0.01f, 1.0f) : 0.5f;
    const float from = high - share * (high - low);
    const auto put = [&](geometry::Vertex v) {
        v.position = v.position * size;
        scalp.indices.push_back(static_cast<u32>(scalp.vertices.size()));
        scalp.vertices.push_back(v);
    };
    for (usize i = 0; i + 2 < skin.size(); i += 3) {
        const geometry::Vertex* v[3] = {&skin[i], &skin[i + 1], &skin[i + 2]};
        std::array<geometry::Vertex, 4> kept;
        const usize count = cut_above(v, from, kept);
        for (usize c = 2; c < count; ++c) {
            put(kept[0]);
            put(kept[c - 1]);
            put(kept[c]);
        }
    }
    return scalp;
}

u32 hair_joint(const anim::Skeleton& skeleton, const std::string& name) {
    for (usize j = 0; j < skeleton.names.size() && j < skeleton.size(); ++j) {
        if (!name.empty()) {
            if (skeleton.names[j] == name) return static_cast<u32>(j);
            continue;
        }
        std::string lower = skeleton.names[j];
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("head") != std::string::npos) return static_cast<u32>(j);
    }
    return k_no_hair_joint;
}

geometry::GroomParams hair_params(const world::Hair& hair) {
    geometry::GroomParams p;
    p.strands = static_cast<u32>(std::clamp(hair.strands, 1, world::k_max_hair_strands));
    p.guides = std::min(static_cast<u32>(std::clamp(hair.guides, 1, world::k_max_hair_guides)), p.strands);
    p.length = hair.length;
    p.length_variation = hair.length_variation;
    p.droop = hair.droop;
    p.curl_radius = hair.curl_radius;
    p.curl_turns = hair.curl_turns;
    p.frizz = hair.frizz;
    p.clump = hair.clump;
    p.seed = static_cast<u32>(std::clamp(hair.seed, 1, 1000000));
    return p;
}

render::HairLook hair_look(const world::Hair& hair) {
    render::HairLook look;
    look.melanin = hair.melanin;
    look.redness = hair.redness;
    look.tint = hair.tint;
    look.roughness = hair.roughness;
    look.width = hair.width;
    look.tip_width = hair.tip_width;
    look.variation = hair.variation;
    return look;
}

sim::HairSettings hair_motion(const world::Hair& hair) {
    sim::HairSettings s;
    s.root_stiffness = hair.stiffness;
    s.tip_stiffness = hair.stiffness / 8.0f;
    s.damping = hair.damping;
    return s;
}

// A groom being grown: shared with the worker growing it, which leaves
// what the editor keeps of it (the whole groom is the worker's to free).
struct HairState::Grown {
    WaitGroup done;
    std::atomic<bool> cancel{false}; // not wanted any more: a job not yet started does nothing
    Seed seed;
    geometry::GroomParams params;
    bool pack = false; // for a renderer
    std::shared_ptr<const MeshLibrary::Skinned> character; // grown from its skin (else from the shape)
    // The worker's, read once `done`:
    bool made = false;
    u32 strands = 0;
    float longest = 0.0f;
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    std::vector<Vec3> rest; // its guides' points
    std::shared_ptr<render::PreparedGroom> packed;
};

namespace {

// A grown groom.
struct Coat {
    bool grown = false;
    Seed seed; // what it was grown from
    std::shared_ptr<const MeshLibrary::Skinned> character; // whose joint's space it is in (kept: `seed` names it)
    render::GroomId groom = render::k_no_groom;
    u32 strands = 0;
    float longest = 0.0f;
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    std::vector<Vec3> rest; // its guides' points at rest
};

} // namespace

struct HairState::Worn {
    world::EntityId id = 0;
    world::Hair hair; // as the entity has it
    world::Transform transform;
    Seed wanted;
    Coat coat;                  // shown
    Coat next;                  // grown since, shown once the renderer has it (with `has_next`)
    bool has_next = false;
    std::shared_ptr<Grown> job; // being grown (one at a time)
    std::unique_ptr<sim::HairSim> sim; // of `coat`
    sim::HairFrame frame;       // its next step's
    world::Shape shape = world::Shape::cube;
    std::string mesh;           // a mesh entity's file
    std::shared_ptr<const MeshLibrary::Skinned> character; // that file's, once found (resolve)
    u32 joint = k_no_hair_joint;                           // of it: the one the hair rides
    std::string problem;        // why it grows none (a mesh entity)
    DVec3 position{};           // where it is drawn (place)
    Quat rotation{};
    bool placed = false;        // by place(): from then on only place() says where it is
    std::vector<u32> felt;      // the character's capsules nearest the joint (indices)
    std::vector<sim::HairCapsule> capsules; // those, as posed, about `position`
    bool simulated = false;     // in the last update
    u32 idle = 0;               // updates `sim` has not moved
    bool left_out = false;
};

HairState::HairState(render::Renderer* renderer, const MeshLibrary* meshes) : m_renderer(renderer), m_meshes(meshes) {}

HairState::~HairState() {
    // Jobs not yet started do nothing; those running are waited for.
    for (const auto& [id, worn] : m_worn) {
        if (worn->job != nullptr) worn->job->cancel.store(true);
    }
    for (const auto& [id, worn] : m_worn) {
        if (worn->job != nullptr) m_jobs.wait(worn->job->done);
    }
    for (const std::shared_ptr<Grown>& job : m_abandoned) m_jobs.wait(job->done);
    if (m_renderer != nullptr) {
        for (const auto& [id, worn] : m_worn) {
            if (worn->coat.groom != render::k_no_groom) m_renderer->remove_groom(worn->coat.groom);
            if (worn->next.groom != render::k_no_groom) m_renderer->remove_groom(worn->next.groom);
        }
    }
}

// Everything grown or being grown for it let go.
void HairState::shed(Worn& worn) {
    if (m_renderer != nullptr) {
        if (worn.coat.groom != render::k_no_groom) m_renderer->remove_groom(worn.coat.groom);
        if (worn.next.groom != render::k_no_groom) m_renderer->remove_groom(worn.next.groom);
    }
    if (worn.sim != nullptr) m_resets += worn.sim->stats().resets;
    if (worn.job != nullptr) {
        worn.job->cancel.store(true);
        m_abandoned.push_back(std::move(worn.job)); // (its worker may still be at it)
        worn.job = nullptr;
    }
    worn.sim = nullptr;
    worn.simulated = false;
    worn.idle = 0;
    worn.coat = {};
    worn.next = {};
    worn.has_next = false;
}

void HairState::drop(world::EntityId id) {
    const auto it = m_worn.find(id);
    if (it == m_worn.end()) return;
    shed(*it->second);
    m_worn.erase(it);
}

// Who wears hair and who is left out: lowest ids first, under the bounds.
void HairState::rank() {
    m_left_out = 0;
    usize count = 0;
    u64 strands = 0;
    bool full = false;
    for (const auto& [id, held] : m_worn) {
        Worn& worn = *held;
        if (!worn.problem.empty()) { // (grows none: holds no place)
            worn.left_out = false;
            continue;
        }
        const u64 own = static_cast<u64>(std::clamp(worn.hair.strands, 1, world::k_max_hair_strands));
        // (Past the first that does not fit all are left out: lowest ids first, strictly.)
        const bool left = full || count >= k_max_hair || strands + own > k_max_strands;
        if (left) {
            full = true;
            ++m_left_out;
            if (!worn.left_out) shed(worn);
        } else {
            ++count;
            strands += own;
        }
        worn.left_out = left;
    }
}

void HairState::grow(Worn& worn) {
    auto job = std::make_shared<Grown>();
    job->seed = worn.wanted;
    job->params = hair_params(worn.hair);
    job->pack = m_renderer != nullptr;
    job->character = worn.character;
    worn.job = job;
    m_jobs.submit(job->done, [job] {
        if (job->cancel.load()) return;
        const geometry::TriangleMesh scalp =
            job->character != nullptr
                ? hair_scalp(*job->character, job->seed.joint, largest(job->seed.scale), job->seed.top)
                : hair_scalp(job->seed.shape, job->seed.scale, job->seed.top);
        const geometry::Groom groom = geometry::generate_groom(scalp, job->params);
        job->strands = groom.strand_count();
        job->longest = groom.longest;
        job->bounds_min = groom.bounds_min;
        job->bounds_max = groom.bounds_max;
        const usize guide_points = std::min(static_cast<usize>(groom.guide_count) * geometry::k_strand_points, groom.points.size());
        job->rest.assign(groom.points.begin(), groom.points.begin() + static_cast<std::ptrdiff_t>(guide_points));
        if (job->pack && job->strands > 0) job->packed = render::Renderer::prepare_groom(groom);
        job->made = true;
    });
}

// A finished job's groom kept as the next to show, if it is still the one wanted.
void HairState::take(Worn& worn) {
    const std::shared_ptr<Grown> job = std::move(worn.job);
    worn.job = nullptr;
    if (!job->made || !(job->seed == worn.wanted)) return; // (the next is grown instead)
    Coat coat;
    coat.grown = true;
    coat.seed = job->seed;
    coat.character = job->character;
    coat.strands = job->strands;
    coat.longest = job->longest;
    coat.bounds_min = job->bounds_min;
    coat.bounds_max = job->bounds_max;
    coat.rest = std::move(job->rest);
    if (coat.strands == 0) {
        log_warning("editor", "hair of entity " + std::to_string(worn.id) + ": nothing to grow from (no hair)");
    } else if (m_renderer != nullptr && job->packed != nullptr) {
        coat.groom = m_renderer->add_groom(*job->packed);
        if (coat.groom == render::k_no_groom) {
            log_warning("editor", "hair of entity " + std::to_string(worn.id) + ": the renderer refused its groom");
        }
    }
    if (m_renderer != nullptr && worn.next.groom != render::k_no_groom) m_renderer->remove_groom(worn.next.groom);
    worn.next = std::move(coat);
    worn.has_next = true;
}

// The next groom shown in place of the old: once the renderer has all of
// it (at once when there is nothing to show meanwhile, or when told `now`).
void HairState::swap(Worn& worn, bool now) {
    if (!worn.has_next) return;
    const bool waits = m_renderer != nullptr && worn.next.groom != render::k_no_groom &&
                       worn.coat.groom != render::k_no_groom && !m_renderer->groom_ready(worn.next.groom);
    if (waits && !now) return;
    if (m_renderer != nullptr && worn.coat.groom != render::k_no_groom) m_renderer->remove_groom(worn.coat.groom);
    if (worn.sim != nullptr) m_resets += worn.sim->stats().resets;
    worn.sim = nullptr; // (of the old groom's guides)
    worn.simulated = false;
    worn.idle = 0;
    worn.coat = std::move(worn.next);
    worn.next = {};
    worn.has_next = false;
}

namespace {

// Where it is drawn when its entity is at `t` and its character's joints
// at `model` (null: no character), and its body's capsules there.
void place_one(HairState::Worn& worn, const world::Transform& t, const MeshLibrary::Skinned* shown, u32 joint,
               const std::vector<Mat4>* model) {
    for (sim::HairCapsule& capsule : worn.capsules) {
        capsule = {};
        capsule.radius = std::numeric_limits<float>::quiet_NaN(); // (not felt, unless placed below)
    }
    if (shown == nullptr || model == nullptr || joint >= model->size()) {
        worn.position = t.position;
        worn.rotation = t.rotation;
        return;
    }
    // The groom keeps model space's directions about the joint at rest:
    // it turns as the joint has turned since.
    const anim::JointTransform at = anim::decompose((*model)[joint]);
    Quat since = at.rotation;
    if (joint < shown->rest_model.size()) {
        const Quat rest = anim::decompose(shown->rest_model[joint]).rotation;
        since = at.rotation * Quat{-rest.x, -rest.y, -rest.z, rest.w};
    }
    worn.position = pose::place(t, at.translation);
    worn.rotation = t.rotation * since;
    if (shown != worn.character.get()) return; // (an older groom still shown: the new body's capsules are not its)
    const float scale = largest(t.scale);
    const std::vector<anim::BoneCapsule>& bones = worn.character->capsules;
    for (usize k = 0; k < worn.felt.size() && k < worn.capsules.size(); ++k) {
        sim::HairCapsule& out = worn.capsules[k];
        if (worn.felt[k] >= bones.size()) continue;
        const anim::BoneCapsule& b = bones[worn.felt[k]];
        if (b.joint >= model->size()) continue;
        const Mat4& m = (*model)[b.joint];
        const DVec3 a = pose::place(t, apply(m, b.a)) - worn.position;
        const DVec3 c = pose::place(t, apply(m, b.b)) - worn.position;
        out.a = {static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z)};
        out.b = {static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z)};
        out.radius = b.radius * stretch(m) * scale;
    }
}

} // namespace

// A mesh entity's character and joint found (or why not).
void HairState::resolve(Worn& worn) {
    worn.problem.clear();
    std::shared_ptr<const MeshLibrary::Skinned> character;
    u32 joint = k_no_hair_joint;
    if (worn.shape == world::Shape::mesh) {
        const MeshLibrary::Mesh* mesh = m_meshes != nullptr ? m_meshes->find_normalized(worn.mesh) : nullptr;
        if (mesh == nullptr) {
            worn.problem = "its mesh file is not loaded";
        } else if (mesh->skinned == nullptr) {
            worn.problem = "its mesh file has no skeleton (hair grows on characters, cubes and spheres)";
        } else {
            character = mesh->skinned;
            joint = hair_joint(character->skeleton, worn.hair.joint);
            if (joint == k_no_hair_joint || joint >= character->rest_model.size()) {
                worn.problem = worn.hair.joint.empty()
                                   ? "no joint of its skeleton has \"head\" in its name: name one (hair.joint)"
                                   : "its skeleton has no joint named \"" + worn.hair.joint + "\"";
                character = nullptr;
            }
        }
        if (!worn.problem.empty()) shed(worn);
    }
    if (character != worn.character || joint != worn.joint) {
        worn.character = character;
        worn.joint = character != nullptr ? joint : k_no_hair_joint;
        worn.felt.clear();
        if (character != nullptr) {
            // The capsules it feels: the body's nearest the joint, at rest.
            const Vec3 head = apply(character->rest_model[joint], Vec3{});
            std::vector<std::pair<float, u32>> order;
            for (usize i = 0; i < character->capsules.size(); ++i) {
                const anim::BoneCapsule& b = character->capsules[i];
                if (b.joint >= character->rest_model.size()) continue;
                order.emplace_back(length(apply(character->rest_model[b.joint], (b.a + b.b) * 0.5f) - head), static_cast<u32>(i));
            }
            std::sort(order.begin(), order.end());
            if (order.size() > sim::k_max_hair_capsules) order.resize(sim::k_max_hair_capsules);
            for (const auto& [distance, index] : order) worn.felt.push_back(index);
        }
        worn.capsules.assign(worn.felt.size(), sim::HairCapsule{});
    }
    worn.wanted.character = worn.character.get();
    worn.wanted.joint = worn.character != nullptr ? worn.joint : 0;
    // Until a frame places it (place), it is at its entity, a character's at its joint at rest.
    if (!worn.placed) {
        const MeshLibrary::Skinned* shown = worn.coat.grown ? worn.coat.character.get() : worn.character.get();
        place_one(worn, worn.transform, shown, worn.coat.grown ? worn.coat.seed.joint : worn.joint,
                  shown != nullptr ? &shown->rest_model : nullptr);
    }
}

void HairState::place(const AnimationState* animation, const DrawnPoses* moving, const DrawnPoses* walking,
                      const DrawnPoses* swinging) {
    for (const auto& [id, held] : m_worn) {
        Worn& worn = *held;
        // (As the scene frame draws its entity.)
        const world::Transform* drawn = &worn.transform;
        for (const DrawnPoses* poses : {moving, walking, swinging}) {
            if (poses == nullptr) continue;
            if (const auto it = poses->find(id); it != poses->end()) {
                drawn = &it->second;
                break;
            }
        }
        // The groom shown is in the space of the joint it was grown round
        // (while another grows, still the old one's).
        const MeshLibrary::Skinned* shown = worn.coat.grown ? worn.coat.character.get() : worn.character.get();
        const u32 joint = worn.coat.grown ? worn.coat.seed.joint : worn.joint;
        const std::vector<Mat4>* model = nullptr;
        if (shown != nullptr) {
            // (As its body is drawn: its animation's pose once its copy shows, else at rest.)
            const AnimationState::Animated* a = animation != nullptr ? animation->find(id) : nullptr;
            // (A file loaded again while the old groom still shows: the new body's pose, if it has as many joints.)
            const bool posed = a != nullptr && a->posed && (a->drawn || !animation->draws()) && a->skinned != nullptr &&
                               a->model.size() == shown->skeleton.size();
            model = posed ? &a->model : &shown->rest_model;
        }
        place_one(worn, *drawn, shown, joint, model);
        worn.placed = true;
    }
    list_draws();
}

void HairState::follow_one(const world::Scene& scene, world::EntityId id) {
    const world::Entity* e = scene.find(id);
    if (e == nullptr || !wears(*e)) {
        drop(id);
        return;
    }
    auto it = m_worn.find(id);
    if (it == m_worn.end()) {
        it = m_worn.emplace(id, std::make_unique<Worn>()).first;
        it->second->id = id;
    }
    Worn& worn = *it->second;
    worn.hair = e->hair;
    worn.transform = e->transform;
    worn.wanted = seed_of(*e);
    worn.shape = e->shape;
    if (worn.mesh != e->mesh) worn.mesh = e->mesh;
    resolve(worn);
}

void HairState::follow(const world::Scene& scene, const world::SceneChanges& changed) {
    if (scene.identity() != m_scene || changed.all) {
        m_scene = scene.identity();
        std::vector<world::EntityId> ids;
        ids.reserve(m_worn.size());
        for (const auto& [id, worn] : m_worn) ids.push_back(id);
        for (const world::Entity& e : scene.entities()) {
            if (e.hair.on) ids.push_back(e.id);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const world::EntityId id : ids) follow_one(scene, id);
    } else if (changed.ids.empty()) {
        return;
    } else {
        for (const world::EntityId id : changed.ids) follow_one(scene, id);
    }
    // Drawn where it is now and as it looks now, this frame; nothing gone is named.
    rank();
    list_draws();
}

namespace {

bool wants_growing(const HairState::Worn& worn) {
    const Coat& latest = worn.has_next ? worn.next : worn.coat;
    return !(latest.grown && latest.seed == worn.wanted);
}

} // namespace

void HairState::finish() {
    if (m_meshes != nullptr && m_meshes->version() != m_meshes_seen) {
        m_meshes_seen = m_meshes->version();
        for (const auto& [id, worn] : m_worn) resolve(*worn);
    }
    rank();
    for (const auto& [id, worn] : m_worn) {
        if (worn->left_out || !worn->problem.empty()) continue;
        // (A job taken may not be the one wanted any more: until one is.)
        for (int tries = 0; tries < 4; ++tries) {
            if (worn->job != nullptr) {
                m_jobs.wait(worn->job->done);
                take(*worn);
            }
            if (!wants_growing(*worn)) break;
            grow(*worn);
        }
        swap(*worn, true);
    }
    list_draws();
}

void HairState::update(const WindState& wind, const DVec3& camera, double seconds) {
    std::erase_if(m_abandoned, [](const std::shared_ptr<Grown>& job) { return job->done.done(); });
    // (A file loaded or loaded again since: characters found, or changed.)
    if (m_meshes != nullptr && m_meshes->version() != m_meshes_seen) {
        m_meshes_seen = m_meshes->version();
        for (const auto& [id, worn] : m_worn) resolve(*worn);
    }
    rank();
    m_near.clear();
    u32 taken = 0;
    for (const auto& [id, held] : m_worn) {
        Worn& worn = *held;
        const bool moving = worn.simulated;
        worn.simulated = false;
        if (worn.left_out || !worn.problem.empty()) continue;
        // (A groom taken is a GPU buffer made: a few an update.)
        if (worn.job != nullptr && taken < k_taken_per_update && worn.job->done.done()) {
            take(worn);
            ++taken;
        }
        swap(worn, false);
        if (worn.job == nullptr && wants_growing(worn)) grow(worn);
        if (!worn.coat.grown || worn.coat.strands == 0) continue;
        if (worn.hair.moves && !worn.coat.rest.empty()) {
            const DVec3 away = worn.position - camera;
            const double distance = std::sqrt(away.x * away.x + away.y * away.y + away.z * away.z);
            // One already moving keeps its place until another is clearly nearer.
            if (distance <= (moving ? k_keep_simulating_within : k_simulate_within)) {
                m_near.emplace_back(moving ? distance * 0.9 : distance, &worn);
            }
        }
    }
    // The nearest move (ties by id, so the same frame is the same choice).
    std::sort(m_near.begin(), m_near.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second->id < b.second->id;
    });
    m_stepped.clear();
    usize guides = 0;
    u32 made = 0;
    for (const auto& [distance, worn] : m_near) {
        const usize own = worn->coat.rest.size() / geometry::k_strand_points;
        if (m_stepped.size() >= k_max_simulated || (!m_stepped.empty() && guides + own > k_max_simulated_guides)) break;
        if (worn->sim == nullptr) {
            if (made >= k_sims_made_per_update) continue; // (in a later update: it hangs as grown until then)
            ++made;
            worn->sim = std::make_unique<sim::HairSim>(worn->coat.rest, hair_motion(worn->hair));
        } else {
            const sim::HairSettings motion = hair_motion(worn->hair);
            worn->sim->set_motion(motion.root_stiffness, motion.tip_stiffness, motion.damping);
            if (worn->idle > 0) worn->sim->reset(); // (it hung as grown meanwhile: from there)
        }
        guides += own;
        worn->idle = 0;
        worn->frame = {};
        worn->frame.position = worn->position;
        worn->frame.rotation = worn->rotation;
        worn->frame.wind = wind_at(wind, worn->position);
        worn->frame.capsules = worn->capsules; // (a character's body; none otherwise)
        worn->simulated = true;
        m_stepped.push_back(worn);
    }
    m_simulated = static_cast<u32>(m_stepped.size());
    if (m_stepped.size() == 1) {
        m_stepped[0]->sim->step(seconds, m_stepped[0]->frame);
    } else if (!m_stepped.empty()) {
        // (Three shares: the two workers' and this thread's.)
        m_movers.parallel_for(m_stepped.size(), (m_stepped.size() + 2) / 3, [this, seconds](usize begin, usize end) {
            for (usize i = begin; i < end; ++i) m_stepped[i]->sim->step(seconds, m_stepped[i]->frame);
        });
    }
    for (const auto& [id, held] : m_worn) {
        Worn& worn = *held;
        // Out of reach (or told to keep still): it hangs as grown. Its
        // simulation is kept a while, so one coming and going makes no new one each time.
        if (!worn.simulated && worn.sim != nullptr && ++worn.idle > 120) {
            m_resets += worn.sim->stats().resets;
            worn.sim = nullptr;
            worn.idle = 0;
        }
    }
    list_draws();
}

void HairState::list_draws() {
    m_draws.clear();
    for (const auto& [id, held] : m_worn) {
        const Worn& worn = *held;
        if (worn.left_out || worn.coat.groom == render::k_no_groom) continue;
        render::HairDraw draw;
        draw.groom = worn.coat.groom;
        draw.position = worn.position;
        draw.rotation = worn.rotation;
        draw.look = hair_look(worn.hair);
        if (worn.simulated && worn.sim != nullptr) draw.guides = worn.sim->points();
        m_draws.push_back(draw);
    }
}

HairStats HairState::stats() const {
    HairStats s;
    s.left_out = m_left_out;
    s.simulated = m_simulated;
    s.drawn = static_cast<u32>(m_draws.size());
    s.resets = m_resets;
    for (const auto& [id, worn] : m_worn) {
        ++s.wearing;
        if (worn->coat.grown && worn->coat.strands > 0) {
            ++s.grown;
            s.strands += worn->coat.strands;
        }
        if (worn->job != nullptr || worn->has_next) ++s.growing;
        if (worn->sim != nullptr) s.resets += worn->sim->stats().resets;
    }
    return s;
}

bool HairState::view(world::EntityId id, View& out) const {
    const auto it = m_worn.find(id);
    if (it == m_worn.end()) return false;
    const Worn& worn = *it->second;
    const Coat& coat = worn.coat;
    out = {};
    out.grown = coat.grown && coat.strands > 0;
    out.growing = worn.job != nullptr || worn.has_next;
    out.ready = coat.groom != render::k_no_groom && m_renderer != nullptr && m_renderer->groom_ready(coat.groom);
    out.simulated = worn.simulated;
    out.left_out = worn.left_out;
    out.problem = worn.problem;
    if (worn.character != nullptr && worn.joint < worn.character->skeleton.names.size()) {
        out.joint = worn.character->skeleton.names[worn.joint];
    }
    out.position = worn.position;
    out.rotation = worn.rotation;
    out.capsules = static_cast<u32>(worn.capsules.size());
    out.strands = coat.strands;
    out.guides = static_cast<u32>(coat.rest.size() / geometry::k_strand_points);
    out.longest = coat.longest;
    out.bounds_min = coat.bounds_min;
    out.bounds_max = coat.bounds_max;
    out.guide_points = worn.simulated && worn.sim != nullptr ? worn.sim->points() : std::span<const Vec3>(coat.rest);
    return true;
}

} // namespace giant::editor
