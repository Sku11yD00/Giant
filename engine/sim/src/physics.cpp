// PhysicsWorld on Jolt: layers, the job system bridge and the conversions.
#include "giant/sim/physics.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/world/water.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <mutex>

namespace giant::sim {

namespace {

// --- Process-wide Jolt set-up (types, factory), counted by worlds ------------

std::mutex g_jolt_mutex;
int g_jolt_users = 0;

void jolt_trace(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    log_info("physics", text);
}

void jolt_acquire() {
    const std::lock_guard lock(g_jolt_mutex);
    if (g_jolt_users++ == 0) {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = jolt_trace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void jolt_release() {
    const std::lock_guard lock(g_jolt_mutex);
    if (--g_jolt_users == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

// --- Layers: what never moves, and what does ----------------------------------

namespace layers {
constexpr JPH::ObjectLayer fixed = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::uint count = 2;
} // namespace layers

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return layers::count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer.GetValue() == layers::fixed ? "fixed" : "moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer broad) const override {
        // Fixed things only meet moving ones.
        return object == layers::moving || broad.GetValue() == layers::moving;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == layers::moving || b == layers::moving;
    }
};

// --- Jolt's jobs on our job system --------------------------------------------

class JoltJobs final : public JPH::JobSystemWithBarrier {
public:
    explicit JoltJobs(giant::JobSystem& jobs) : JPH::JobSystemWithBarrier(JPH::cMaxPhysicsBarriers), m_jobs(jobs) {
        m_free.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs);
    }
    ~JoltJobs() override { m_jobs.wait(m_group); }
    giant::JobSystem& jobs() const { return m_jobs; }
    JoltJobs(const JoltJobs&) = delete;
    JoltJobs& operator=(const JoltJobs&) = delete;

    // Runs (or waits for) what is still queued: a job a barrier's waiting
    // thread ran itself stays queued, holding its slot, until a worker gets
    // to it. Called after each step, so busy workers can never let those
    // pile up across steps and run the free list out.
    void drain() { m_jobs.wait(m_group); }

    int GetMaxConcurrency() const override { return static_cast<int>(m_jobs.worker_count()) + 1; }

    JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn,
                             JPH::uint32 dependencies) override {
        JPH::uint32 index = m_free.ConstructObject(name, color, this, fn, dependencies);
        // Jolt asks for at most cMaxPhysicsJobs at once; more is a bug.
        GIANT_ASSERT(index != Free::cInvalidObjectIndex, "physics: out of jobs");
        Job* job = &m_free.Get(index);
        JPH::JobHandle handle(job); // a reference: the job may finish at once
        if (dependencies == 0) QueueJob(job);
        return handle;
    }

protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        m_jobs.submit(m_group, [job] {
            job->Execute(); // a job already run (by a barrier's waiting thread) runs nothing
            job->Release();
        });
    }
    void QueueJobs(Job** jobs, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) QueueJob(jobs[i]);
    }
    void FreeJob(Job* job) override { m_free.DestructObject(job); }

private:
    using Free = JPH::FixedSizeFreeList<Job>;
    giant::JobSystem& m_jobs;
    WaitGroup m_group;
    Free m_free;
};

// --- Conversions --------------------------------------------------------------

JPH::Vec3 to_jolt(const Vec3& v) { return {v.x, v.y, v.z}; }
JPH::RVec3 to_jolt(const DVec3& v) { return {v.x, v.y, v.z}; }
JPH::Quat to_jolt(const Quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w).Normalized(); }
Vec3 from_jolt(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
DVec3 from_jolt_r(JPH::RVec3Arg v) {
    return {static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ())};
}
Quat from_jolt(JPH::QuatArg q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

BodyId to_id(JPH::BodyID id) {
    // Jolt's invalid id is all ones: +1 wraps it to 0, k_no_body.
    return id.IsInvalid() ? k_no_body : id.GetIndexAndSequenceNumber() + 1u;
}
JPH::BodyID from_id(BodyId id) { return id == k_no_body ? JPH::BodyID() : JPH::BodyID(id - 1u); }

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(const Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) &&
           q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w > 1e-12f;
}

// Smallest size a shape may have (metres): Jolt needs some volume.
constexpr float k_min_size = 1e-3f;

JPH::RefConst<JPH::Shape> make_shape(const Shape& shape, bool moving) {
    JPH::ShapeSettings::ShapeResult result;
    if (const auto* box = std::get_if<BoxShape>(&shape)) {
        if (!finite(box->half_extents) || box->half_extents.x < 0.0f || box->half_extents.y < 0.0f ||
            box->half_extents.z < 0.0f) {
            return nullptr;
        }
        const Vec3 h{std::max(box->half_extents.x, k_min_size), std::max(box->half_extents.y, k_min_size),
                     std::max(box->half_extents.z, k_min_size)};
        // Convex radius at most a tenth of the smallest side, so thin slabs stay thin.
        const float radius = std::min(JPH::cDefaultConvexRadius, 0.1f * std::min({h.x, h.y, h.z}));
        result = JPH::BoxShapeSettings(to_jolt(h), radius).Create();
    } else if (const auto* sphere = std::get_if<SphereShape>(&shape)) {
        if (!std::isfinite(sphere->radius) || sphere->radius < 0.0f) return nullptr;
        result = JPH::SphereShapeSettings(std::max(sphere->radius, k_min_size)).Create();
    } else if (const auto* capsule = std::get_if<CapsuleShape>(&shape)) {
        if (!std::isfinite(capsule->half_height) || !std::isfinite(capsule->radius) ||
            capsule->radius < 0.0f || capsule->half_height < 0.0f) {
            return nullptr;
        }
        const float radius = std::max(capsule->radius, k_min_size);
        if (capsule->half_height < k_min_size) {
            result = JPH::SphereShapeSettings(radius).Create();
        } else {
            result = JPH::CapsuleShapeSettings(capsule->half_height, radius).Create();
        }
    } else if (const auto* convex = std::get_if<ConvexShape>(&shape)) {
        if (convex->points.size() < 4) return nullptr;
        JPH::Array<JPH::Vec3> points;
        points.reserve(convex->points.size());
        for (const Vec3& p : convex->points) {
            if (!finite(p)) return nullptr;
            points.push_back(to_jolt(p));
        }
        result = JPH::ConvexHullShapeSettings(points).Create();
    } else if (const auto* height = std::get_if<HeightShape>(&shape)) {
        // Fixed only: Jolt does not move height fields.
        if (moving || height->samples < 2 || !std::isfinite(height->spacing) || height->spacing < k_min_size ||
            height->heights.size() != static_cast<usize>(height->samples) * height->samples) {
            return nullptr;
        }
        // Jolt wants a sample count that its blocks (2 samples) divide: an odd
        // count gets one more row and column, without collision.
        const u32 n = height->samples + (height->samples % 2u);
        std::vector<float> samples(static_cast<usize>(n) * n, JPH::HeightFieldShapeConstants::cNoCollisionValue);
        bool any = false;
        for (u32 j = 0; j < height->samples; ++j) {
            for (u32 i = 0; i < height->samples; ++i) {
                const float h = height->heights[static_cast<usize>(j) * height->samples + i];
                if (std::isfinite(h)) {
                    samples[static_cast<usize>(j) * n + i] = h;
                    any = true;
                } else if (!std::isnan(h)) {
                    return nullptr; // an infinity is a mistake, not a hole
                }
            }
        }
        if (!any) return nullptr;
        JPH::HeightFieldShapeSettings settings(samples.data(), JPH::Vec3::sZero(),
                                               JPH::Vec3(height->spacing, 1.0f, height->spacing), n);
        settings.mBitsPerSample = 16; // millimetres over a few hundred metres of relief
        settings.mBlockSize = 2;
        result = settings.Create();
    }
    if (result.HasError()) return nullptr;
    return result.Get();
}

// Which part of a body's shape a contact is on (a compound's part, else 0).
u32 part_of(const JPH::Body& body, const JPH::SubShapeID& id) {
    const JPH::Shape* shape = body.GetShape();
    if (shape->GetType() != JPH::EShapeType::Compound) return 0;
    JPH::SubShapeID rest;
    return static_cast<const JPH::CompoundShape*>(shape)->GetSubShapeIndexFromID(id, rest);
}

// Collects hard new contacts during a step (on the physics workers).
class HitListener final : public JPH::ContactListener {
public:
    float min_impulse = 50.0f;

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        if (manifold.mRelativeContactPointsOn1.empty()) return;
        if (!a.IsDynamic() && !b.IsDynamic()) return;
        const JPH::RVec3 point = manifold.GetWorldSpaceContactPointOn1(0);
        // What it takes to stop each body at the point, along the normal:
        // its mass and, as it turns, its inertia (a long log struck at its
        // tip is stopped far more easily than its mass says).
        const auto inverse = [&](const JPH::Body& body) {
            if (!body.IsDynamic()) return 0.0f;
            const JPH::Vec3 r(point - body.GetCenterOfMassPosition());
            const JPH::Vec3 rn = r.Cross(manifold.mWorldSpaceNormal);
            return body.GetMotionProperties()->GetInverseMass() + rn.Dot(body.GetInverseInertia() * rn);
        };
        const float inverse_a = inverse(a);
        const float inverse_b = inverse(b);
        if (!(inverse_a + inverse_b > 0.0f)) return;
        // Closing speed along the normal (a's point toward b's).
        const float closing = (a.GetPointVelocity(point) - b.GetPointVelocity(point)).Dot(manifold.mWorldSpaceNormal);
        const float impulse = closing / (inverse_a + inverse_b);
        if (!(impulse >= min_impulse)) return;
        ContactHit hit;
        hit.a = to_id(a.GetID());
        hit.b = to_id(b.GetID());
        hit.part_a = part_of(a, manifold.mSubShapeID1);
        hit.part_b = part_of(b, manifold.mSubShapeID2);
        hit.impulse = impulse;
        hit.point = from_jolt_r(point);
        hit.normal = from_jolt(manifold.mWorldSpaceNormal);
        const std::lock_guard lock(m_mutex);
        m_hits.push_back(hit);
    }

    // The hits since the last take, in a fixed order (workers find them in any).
    void take(std::vector<ContactHit>& out) {
        const std::lock_guard lock(m_mutex);
        out.insert(out.end(), m_hits.begin(), m_hits.end());
        m_hits.clear();
    }

private:
    std::mutex m_mutex;
    std::vector<ContactHit> m_hits;
};

} // namespace

// --- Shapes made ahead --------------------------------------------------------

// A body's shape as Jolt holds it.
struct PreparedShapeData {
    JPH::RefConst<JPH::Shape> shape;
};

namespace {

bool valid_desc(const BodyDesc& d) {
    return finite(d.position) && finite(d.rotation) && finite(d.linear_velocity) && finite(d.angular_velocity) &&
           std::isfinite(d.mass) && d.mass >= 0.0f && std::isfinite(d.friction) && std::isfinite(d.restitution);
}

bool placed(const Part& p) { return p.offset != Vec3{} || !(p.rotation == Quat{}); }

// The body's parts as one shape: a part alone, a part moved or turned, or a
// compound of what parts could be made. Null when none could.
JPH::RefConst<JPH::Shape> body_shape(const BodyDesc& d) {
    if (d.parts.empty()) return nullptr;
    const bool moving = d.motion != Motion::fixed;
    if (d.parts.size() == 1 && !placed(d.parts[0])) return make_shape(d.parts[0].shape, moving);
    if (d.parts.size() == 1) {
        JPH::RefConst<JPH::Shape> inner = make_shape(d.parts[0].shape, moving);
        if (inner == nullptr || !finite(d.parts[0].offset) || !finite(d.parts[0].rotation)) return nullptr;
        JPH::ShapeSettings::ShapeResult r =
            JPH::RotatedTranslatedShapeSettings(to_jolt(d.parts[0].offset), to_jolt(d.parts[0].rotation), inner)
                .Create();
        return r.HasError() ? nullptr : r.Get();
    }
    JPH::StaticCompoundShapeSettings compound;
    u32 made = 0;
    for (const Part& p : d.parts) {
        JPH::RefConst<JPH::Shape> inner = make_shape(p.shape, moving);
        if (inner == nullptr || !finite(p.offset) || !finite(p.rotation)) continue; // left out, not the whole body
        compound.AddShape(to_jolt(p.offset), to_jolt(p.rotation), inner);
        ++made;
    }
    if (made == 0) return nullptr;
    JPH::ShapeSettings::ShapeResult r = compound.Create();
    return r.HasError() ? nullptr : r.Get();
}

// Counts this world among Jolt's users for as long as it lives (first in
// Impl: set up before, and torn down after, everything else).
struct JoltUser {
    JoltUser() { jolt_acquire(); }
    ~JoltUser() { jolt_release(); }
    JoltUser(const JoltUser&) = delete;
    JoltUser& operator=(const JoltUser&) = delete;
};

bool valid_character(const CharacterDesc& d) {
    return finite(d.position) && std::isfinite(d.height) && std::isfinite(d.radius) &&
           std::isfinite(d.max_slope_degrees) && std::isfinite(d.step_height) && std::isfinite(d.mass) &&
           std::isfinite(d.strength) && d.radius >= 0.05f && d.height >= 2.0f * d.radius + 0.01f &&
           d.height <= 100.0f && d.max_slope_degrees >= 0.0f && d.max_slope_degrees <= 89.0f &&
           d.step_height >= 0.0f && d.step_height <= 0.5f * d.height && d.mass > 0.0f && d.strength >= 0.0f &&
           std::isfinite(d.swim_depth) && d.swim_depth > 0.0f &&
           std::isfinite(d.swim_speed) && d.swim_speed >= 0.0f && d.swim_speed <= 100.0f;
}

// Fastest a character is told to go, jump or fall (metres a second): far
// past running, within what Jolt keeps exact.
constexpr float k_max_character_speed = 100.0f;

bool valid_input(const CharacterInput& in) {
    return finite(in.velocity) && std::fabs(in.velocity.x) <= k_max_character_speed &&
           std::fabs(in.velocity.y) <= k_max_character_speed && std::fabs(in.velocity.z) <= k_max_character_speed &&
           std::isfinite(in.acceleration) && in.acceleration >= 0.0f &&
           std::isfinite(in.air_acceleration) && in.air_acceleration >= 0.0f;
}

// A capsule standing on the origin (the character's feet).
JPH::RefConst<JPH::Shape> standing_capsule(float height, float radius) {
    const float half = 0.5f * height;
    JPH::ShapeSettings::ShapeResult capsule = JPH::CapsuleShapeSettings(half - radius, radius).Create();
    if (capsule.HasError()) return nullptr;
    JPH::ShapeSettings::ShapeResult r =
        JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, half, 0.0f), JPH::Quat::sIdentity(), capsule.Get())
            .Create();
    return r.HasError() ? nullptr : r.Get();
}

// `from` moved toward `to` by at most `step`.
JPH::Vec3 approach(JPH::Vec3Arg from, JPH::Vec3Arg to, float step) {
    const JPH::Vec3 d = to - from;
    const float length = d.Length();
    return length <= step || length < 1e-6f ? JPH::Vec3(to) : from + d * (step / length);
}

// How long a jump asked for in the air waits for the ground (seconds).
constexpr float k_jump_wait = 0.2f;

// Water (set_water): its density (kg per cubic metre), how it drags a
// body (Jolt's factors: through it, and turning in it, then a share of its
// speed against the water lost each second, by how much of it is under), and a swimmer's
// way through it: speeding up or slowing (m/s^2, across; up and down, and
// more the faster, so a fall into deep water stops within metres), floating up with no
// input (m/s), how hard it is held at the surface (per second), and how
// much shallower than swim_depth it stops swimming (no flicker at a shelf).
constexpr float k_water_density = 1000.0f;
constexpr float k_water_drag = 0.5f;
constexpr float k_water_turn_drag = 0.05f;
constexpr float k_water_damping = 3.0f; // per second, half or more under
// A space under something fixed with a floor this near (m) is a room the
// water does not reach (Impl::roofed).
constexpr float k_water_room_height = 6.0f;
constexpr float k_swim_acceleration = 4.0f;
constexpr float k_swim_vertical_acceleration = 15.0f;
constexpr float k_swim_drag = 0.5f; // per metre: a person's drag over their weight
constexpr float k_swim_float_up = 0.6f;
constexpr float k_swim_hold = 3.0f;
constexpr float k_swim_leave = 0.3f;
// Swimming out onto an edge (swim_out): one this much over its feet (m, or
// its step height if more) and up to this much over the surface
// (and shallow enough there to stand: under the surface by less than where
// it stops swimming).
constexpr float k_swim_out_least = 0.25f;
constexpr float k_swim_out_over = 0.4f;
// Wading: slowed from this depth (m) on, to k_wade_slowest of its speed at swim_depth.
constexpr float k_wade_from = 0.3f;
constexpr float k_wade_slowest = 0.5f;

float approach(float from, float to, float step) {
    return from < to ? std::min(from + step, to) : std::max(from - step, to);
}

// How deep a character swims: its swim_depth, at most three quarters of
// its height (a small one swims in shallower water).
float swim_depth(const CharacterDesc& d) { return std::min(d.swim_depth, 0.75f * d.height); }

} // namespace

// --- The world ----------------------------------------------------------------

struct PhysicsWorld::Impl {
    explicit Impl(JobSystem& job_system, const PhysicsSettings& s)
        : settings(s), jobs(job_system),
#if defined(JPH_DISABLE_TEMP_ALLOCATOR)
          temp(),
#else
          temp(16u << 20),
#endif
          system() {
        system.Init(s.max_bodies, 0, s.max_body_pairs, s.max_contacts, broad_layers, object_vs_broad, pairs);
        system.SetGravity(to_jolt(s.gravity));
        hits.min_impulse = s.min_hit_impulse;
        system.SetContactListener(&hits);
    }
    ~Impl() {
        // Characters (their bodies with them), then the bodies, before the
        // system that owns them (then Jolt's types, last).
        characters.clear();
        JPH::BodyIDVector all;
        system.GetBodies(all);
        if (!all.empty()) {
            bodies_interface().RemoveBodies(all.data(), static_cast<int>(all.size()));
            bodies_interface().DestroyBodies(all.data(), static_cast<int>(all.size()));
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    JoltUser user;
    PhysicsSettings settings;
    BroadPhaseLayers broad_layers;
    ObjectVsBroadPhase object_vs_broad;
    ObjectPairs pairs;
    JoltJobs jobs;
#if defined(JPH_DISABLE_TEMP_ALLOCATOR)
    JPH::TempAllocatorMalloc temp;
#else
    JPH::TempAllocatorImplWithMallocFallback temp; // a big step outgrows it: the heap, not a crash
#endif
    HitListener hits; // before the system: outlives its use
    JPH::PhysicsSystem system;
    double carried = 0.0; // time not yet stepped
    u64 steps = 0;
    u32 dropped = 0;
    double last_step_ms = 0.0;
    u32 bodies = 0;

    struct Character {
        CharacterDesc desc;
        JPH::Ref<JPH::CharacterVirtual> body;
        CharacterInput input;
        float jump_speed = 0.0f;
        float jump_wait = 0.0f; // seconds a jump asked for still waits for the ground
        // Plain types, not Jolt's SIMD ones (no alignment padding).
        Vec3 velocity{}; // how it moved in the last step (Jolt keeps the speed asked for)
        float vertical = 0.0f; // the speed up it was given in the last step
        DVec3 previous{}; // where it was before the last step
        bool held = false;
        // Climbing (CharacterInput::climb): on the wall with this normal (flat, unit).
        bool climbing = false;
        JPH::BodyID wall;
        Vec3 wall_normal{};
        float climb_wait = 0.0f; // seconds before it may take hold again (pushed off)
        // Pulling itself up onto a ledge: from, to, and how far (0 to 1);
        // with a ledge body, from and to are in its frame (a moving ledge).
        bool mantling = false;
        JPH::BodyID mantle_on;
        JPH::RefConst<JPH::Shape> room; // (clear_at's shape, made once)
        DVec3 mantle_from{};
        DVec3 mantle_to{};
        float mantle_t = 0.0f;
        // Carried (carry_character): by this body, held at `carry_at` in its frame.
        JPH::BodyID carrier;
        DVec3 carry_at{};
        // In water (as of its last step).
        bool swimming = false;
        float water_depth = 0.0f;
        bool was_wet = false; // (in water the step before: going in again is a splash)
        float surface_fall = 0.0f; // how fast the surface over its feet fell in its last step (m/s; a sea's swell)
    };
    std::map<CharacterId, Character> characters; // in id order: the same steps every run

    // A character's own body: moved with it, never by the body calls.
    // (Asked for every body a character's ray meets: a set, not a scan.)
    std::unordered_set<JPH::uint32> character_bodies;
    bool is_character_body(JPH::BodyID b) const { return character_bodies.contains(b.GetIndexAndSequenceNumber()); }
    void index_character(const Character& c) { character_bodies.insert(c.body->GetInnerBodyID().GetIndexAndSequenceNumber()); }
    CharacterId next_character = 1;

    // The water (set_water), cut into parts (a lake's shore, the sea, a
    // river's stretch), each with the ground it covers and its highest
    // surface, filed in a grid of k_water_cell squares (the sea, and a part
    // over k_water_cells_most squares, everywhere): a point finds the few
    // parts there, and a body over all of them none.
    struct WaterPart {
        u32 water = 0;
        u32 segment = 0;
        double x0 = 0.0, z0 = 0.0, x1 = 0.0, z1 = 0.0;
        bool everywhere = false;
        float top = 0.0f;
    };
    static constexpr double k_water_cell = 128.0;
    static constexpr double k_water_cells_most = 65536.0;
    std::vector<world::Water> water;
    std::vector<WaterPart> water_parts;
    std::vector<u32> water_everywhere;                      // parts looked at anywhere
    std::unordered_map<u64, std::vector<u32>> water_grid;   // the rest, by square
    float water_top = -std::numeric_limits<float>::infinity(); // the highest surface of all (still)
    // The sea's swell (set_swell), its clock (moved on by each step), and
    // how high it lifts the seas at most (sea_lift: the highest sea's waves).
    world::Swell swell;
    double swell_seconds = 0.0;
    float sea_lift = 0.0f;
    double woken_reach = 0.0; // the swell's reach when sleepers in the seas were last woken
    // How far over its still level any sea may rise now.
    double swell_reach() const { return static_cast<double>(sea_lift) * static_cast<double>(world::swell_most(swell)); }

    static i64 water_square(double v) { return static_cast<i64>(std::floor(v / k_water_cell)); }
    static u64 water_key(i64 x, i64 z) {
        return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u64>(static_cast<u32>(z));
    }

    void set_water(std::span<const world::Water> w) {
        std::vector<world::Water> copy(w.begin(), w.end()); // (w may be this world's own)
        water = std::move(copy);
        water_parts.clear();
        water_everywhere.clear();
        water_grid.clear();
        water_top = -std::numeric_limits<float>::infinity();
        sea_lift = 0.0f;
        for (u32 i = 0; i < water.size(); ++i) {
            const world::Water& b = water[i];
            if (!b.visible) continue; // (not there)
            if (b.kind == world::WaterKind::sea) sea_lift = std::max(sea_lift, world::swell_scale(b));
            if (b.kind == world::WaterKind::river) {
                for (u32 k = 0; k + 1 < b.path.size(); ++k) {
                    const world::RiverPoint& p = b.path[k];
                    const world::RiverPoint& q = b.path[k + 1];
                    const double half = 0.5 * std::max(p.width, q.width);
                    WaterPart part{i, k, std::min(p.x, q.x) - half, std::min(p.z, q.z) - half,
                                   std::max(p.x, q.x) + half, std::max(p.z, q.z) + half, false,
                                   std::max(p.level, q.level)};
                    water_parts.push_back(part);
                }
            } else {
                WaterPart part{i, 0, 0.0, 0.0, 0.0, 0.0, false, b.level};
                part.everywhere = !world::water_bounds(b, part.x0, part.z0, part.x1, part.z1);
                water_parts.push_back(part);
            }
        }
        for (u32 n = 0; n < water_parts.size(); ++n) {
            WaterPart& part = water_parts[n];
            if (!std::isfinite(part.top)) continue; // (unchecked data)
            // (Unchecked data: a part out past any world, or not a number, looked at everywhere.)
            constexpr double k_far = 1.0e12;
            if (!(std::fabs(part.x0) < k_far && std::fabs(part.x1) < k_far && std::fabs(part.z0) < k_far &&
                  std::fabs(part.z1) < k_far)) {
                part.everywhere = true;
            }
            water_top = std::max(water_top, part.top);
            if (!part.everywhere) {
                const double across = std::floor(part.x1 / k_water_cell) - std::floor(part.x0 / k_water_cell) + 1.0;
                const double down = std::floor(part.z1 / k_water_cell) - std::floor(part.z0 / k_water_cell) + 1.0;
                if (!(across * down <= k_water_cells_most)) part.everywhere = true;
            }
            if (part.everywhere) {
                water_everywhere.push_back(n);
                continue;
            }
            for (i64 z = water_square(part.z0); z <= water_square(part.z1); ++z) {
                for (i64 x = water_square(part.x0); x <= water_square(part.x1); ++x) water_grid[water_key(x, z)].push_back(n);
            }
        }
    }

    // The water at (x, z) as world::water_at has it (each body's nearest
    // stretch, then the highest surface), if its surface is over `bottom`.
    struct WaterFound {
        u32 water = 0;
        double nearness = 0.0;
        world::WaterHere here;
    };
    mutable std::vector<WaterFound> water_found; // (water_over's scratch: the stepping thread's)
    mutable std::vector<WaterFound> water_asked; // (PhysicsWorld::water_at's: the caller's thread, between steps)
    std::optional<world::WaterHere> water_over(double x, double z, double bottom) const {
        return water_over(x, z, bottom, water_found);
    }
    std::optional<world::WaterHere> water_over(double x, double z, double bottom,
                                               std::vector<WaterFound>& found) const {
        if (!(static_cast<double>(water_top) + swell_reach() > bottom) || !std::isfinite(x) || !std::isfinite(z)) {
            return std::nullopt;
        }
        found.clear();
        const auto offer = [&](u32 n) {
            const WaterPart& part = water_parts[n];
            if (!part.everywhere && (x < part.x0 || x > part.x1 || z < part.z0 || z > part.z1)) return;
            double nearness = 0.0;
            std::optional<world::WaterHere> h = world::water_part_at(water[part.water], part.segment, x, z, nearness);
            if (!h) return;
            if (water[part.water].kind == world::WaterKind::sea) {
                // Risen or fallen with the swell, leaning with it.
                const double scale = world::swell_scale(water[part.water]);
                if (scale > 0.0) {
                    const world::SwellHere sw = world::swell_at(swell, x, z, swell_seconds);
                    h->level = static_cast<float>(static_cast<double>(h->level) + scale * sw.height);
                    h->slope_x = static_cast<float>(scale * sw.slope_x);
                    h->slope_z = static_cast<float>(scale * sw.slope_z);
                }
            }
            for (WaterFound& f : found) {
                if (f.water != part.water) continue;
                if (nearness < f.nearness) f = {part.water, nearness, *h}; // (a nearer stretch of the same river)
                return;
            }
            found.push_back({part.water, nearness, *h});
        };
        for (const u32 n : water_everywhere) offer(n);
        const auto square = water_grid.find(water_key(water_square(x), water_square(z)));
        if (square != water_grid.end()) {
            for (const u32 n : square->second) offer(n);
        }
        std::optional<world::WaterHere> best;
        for (const WaterFound& f : found) {
            if (!best || f.here.level > best->level) best = f.here;
        }
        if (!best || !(static_cast<double>(best->level) > bottom)) return std::nullopt;
        return best;
    }

    // Whether `from` (top of a body or a head, under the surface) is in a
    // room the water does not reach: something fixed over it before the
    // surface (a ceiling) and something fixed under it (a floor) at most
    // k_water_room_height apart, `low` metres under `from` being its own
    // bottom. A tunnel or a basement under a lake is dry; the open water
    // under a pontoon, an overhang, a pier or a hull's bottom (no floor
    // near, the bed itself no floor: the terrain's heightfield) is not.
    bool roofed(const JPH::RVec3& from, double level, float low) const {
        const double up = level - static_cast<double>(from.GetY());
        if (!(up > 0.0)) return false;
        const JPH::SpecifiedBroadPhaseLayerFilter broad{JPH::BroadPhaseLayer(layers::fixed)};
        const JPH::SpecifiedObjectLayerFilter object{layers::fixed};
        JPH::RayCastResult ceiling;
        const float reach = std::min(static_cast<float>(up), k_water_room_height);
        if (!system.GetNarrowPhaseQuery().CastRay(JPH::RRayCast(from, JPH::Vec3(0.0f, reach, 0.0f)), ceiling, broad, object)) {
            return false;
        }
        const float over = ceiling.mFraction * reach;
        const float under = k_water_room_height - over;
        if (!(under > low)) return false;
        JPH::RayCastResult floor;
        if (!system.GetNarrowPhaseQuery().CastRay(JPH::RRayCast(from, JPH::Vec3(0.0f, -under, 0.0f)), floor, broad, object)) {
            return false;
        }
        // (Between steps, nothing moving bodies: no lock needed to look.)
        const JPH::RefConst<JPH::Shape> shape = system.GetBodyInterfaceNoLock().GetShape(floor.mBodyID);
        return shape == nullptr || shape->GetSubType() != JPH::EShapeSubType::HeightField;
    }

    // Before a step: each dynamic body awake in water buoyed up, slowed
    // and carried along (Jolt's buoyancy: the water it displaces under a
    // flat surface at the level over its centre).
    std::vector<WaterEntry> entries; // since the last step() call began (WaterEntry: splashes)
    u64 floated = 0; // float_bodies' calls: the fixed steps
    // The step each awake moving body was last seen dry (above the water:
    // none of its own shape under it; by its id's index, its sequence number
    // with it), and the step it last went in: wet now and dry the step
    // before, it went in. A body asleep is not seen, so one woken wet, or
    // made under water, was not dry the step before: no splash; nor one in
    // a room under the water (not dry: under it). One that went in does not
    // again within k_splash_again steps (landing in a rough sea it touches
    // and leaves the falling surface a few times over: one splash). (Grown
    // by doubling up to the most bodies; 12 bytes a body.)
    struct Dry {
        u32 sequence = 0;
        u32 step = 0;
        u32 splashed = 0; // the step it last went in, plus 1 (0: never)
    };
    std::vector<Dry> dry;
    static constexpr u32 k_splash_again = 30; // half a second
    Dry& dry_of(JPH::BodyID id) {
        const u32 index = id.GetIndex();
        if (index >= dry.size()) {
            const usize most = std::max<usize>(settings.max_bodies, 64);
            dry.resize(std::max<usize>(index + 1, std::min<usize>(std::max<usize>(dry.size() * 2, 64), most)));
        }
        Dry& d = dry[index];
        if (d.sequence != id.GetSequenceNumber()) d = {id.GetSequenceNumber(), 0, 0}; // (another body there now)
        return d;
    }
    void seen_dry(JPH::BodyID id) { dry_of(id).step = static_cast<u32>(floated); }
    bool dry_before(JPH::BodyID id) const {
        const u32 index = id.GetIndex();
        return index < dry.size() && dry[index].sequence == id.GetSequenceNumber() &&
               dry[index].step == static_cast<u32>(floated - 1);
    }

    // How fast the surface at (x, z) fell over the last `dt` seconds (m/s;
    // rising: less than 0): a sea's swell; lakes and rivers stand.
    float surface_fall(const world::WaterHere& here, double x, double z, double dt) const {
        if (here.water == nullptr || here.water->kind != world::WaterKind::sea) return 0.0f;
        const double scale = world::swell_scale(*here.water);
        if (!(scale > 0.0) || !(dt > 0.0)) return 0.0f;
        const double before = world::swell_height(swell, x, z, swell_seconds - dt);
        const double now = world::swell_height(swell, x, z, swell_seconds);
        const double fall = scale * (before - now) / dt;
        return std::isfinite(fall) ? static_cast<float>(fall) : 0.0f;
    }

    void float_bodies(float dt) {
        ++floated;
        if (water_parts.empty()) return; // (no water: nothing seen dry; water set, the first step makes none)
        JPH::BodyIDVector active;
        system.GetActiveBodies(JPH::EBodyType::RigidBody, active);
        const JPH::Vec3 gravity = system.GetGravity();
        for (const JPH::BodyID id : active) {
            // A first look without the lock (only this thread moves bodies
            // between steps): most bodies are in no water.
            std::optional<world::WaterHere> here;
            bool roofed_over = false; // (in a room under the water: not dry)
            {
                const JPH::BodyLockRead look(system.GetBodyLockInterfaceNoLock(), id);
                if (!look.Succeeded() || !look.GetBody().IsDynamic()) continue;
                const JPH::Body& b = look.GetBody();
                const JPH::RVec3 centre = b.GetCenterOfMassPosition();
                const JPH::AABox& box = b.GetWorldSpaceBounds();
                here = water_over(static_cast<double>(centre.GetX()), static_cast<double>(centre.GetZ()),
                                  static_cast<double>(box.mMin.GetY()));
                // All under: a roof between it and the surface keeps the water out.
                if (here && static_cast<double>(box.mMax.GetY()) < static_cast<double>(here->level) &&
                    roofed(JPH::RVec3(centre.GetX(), static_cast<JPH::Real>(box.mMax.GetY()) + 0.01f, centre.GetZ()),
                           static_cast<double>(here->level), box.mMax.GetY() - box.mMin.GetY() + 0.02f)) {
                    here.reset();
                    roofed_over = true;
                }
            }
            if (!here) {
                if (!roofed_over) seen_dry(id);
                continue;
            }
            const JPH::BodyLockWrite lock(system.GetBodyLockInterface(), id);
            if (!lock.Succeeded()) continue;
            JPH::Body& b = lock.GetBody();
            const JPH::RVec3 centre = b.GetCenterOfMassPosition();
            const JPH::RVec3 surface(centre.GetX(), static_cast<JPH::Real>(here->level), centre.GetZ());
            // (Leaning with the swell: a body on it tilts with it.)
            const JPH::Vec3 up = JPH::Vec3(-here->slope_x, 1.0f, -here->slope_z).Normalized();
            float volume = 0.0f, submerged = 0.0f;
            JPH::Vec3 buoyed_at;
            b.GetSubmergedVolume(surface, up, volume, submerged, buoyed_at);
            const float inverse_mass = b.GetMotionProperties()->GetInverseMass();
            if (!(volume > 0.0f) || !(submerged > 0.0f) || !(inverse_mass > 0.0f)) {
                seen_dry(id);
                continue;
            }
            // Just gone in, going down fast enough: a splash. Wet now, seen dry the step before (its
            // own shape, as turned), going down at k_entry_speed or more both by itself and against the
            // surface (riding a falling swell is not going in, nor is the swell rising round it). A
            // body woken wet, pushed down, or made under water was not seen dry: none. One that went
            // in lately is not again.
            if (dry_before(id)) {
                const JPH::Vec3 v = b.GetLinearVelocity();
                const float down = -v.GetY();
                if (down >= PhysicsWorld::k_entry_speed && std::isfinite(down)) {
                    const double cx = static_cast<double>(centre.GetX()), cz = static_cast<double>(centre.GetZ());
                    const float against = down - surface_fall(*here, cx, cz, static_cast<double>(dt));
                    Dry& d = dry_of(id);
                    const bool lately = d.splashed != 0 && static_cast<u32>(floated) + 1 - d.splashed <= k_splash_again;
                    if (against >= PhysicsWorld::k_entry_speed && !lately) {
                        d.splashed = static_cast<u32>(floated) + 1;
                        const JPH::AABox& box = b.GetWorldSpaceBounds();
                        WaterEntry& e = entries.emplace_back();
                        e.body = to_id(id);
                        // (Where it broke the surface: the middle of what is under so far, a log
                        // end first at its end.)
                        // (buoyed_at: from the centre of mass.)
                        e.point = {cx + (std::isfinite(buoyed_at.GetX()) ? static_cast<double>(buoyed_at.GetX()) : 0.0),
                                   static_cast<double>(here->level),
                                   cz + (std::isfinite(buoyed_at.GetZ()) ? static_cast<double>(buoyed_at.GetZ()) : 0.0)};
                        e.speed = against;
                        e.across = std::hypot(v.GetX(), v.GetZ());
                        e.mass = 1.0f / inverse_mass;
                        e.size = std::max(box.mMax.GetX() - box.mMin.GetX(), box.mMax.GetZ() - box.mMin.GetZ());
                    }
                }
            }
            // Jolt's factor: 1 is a body as dense as the water.
            const float buoyancy = k_water_density * volume * inverse_mass;
            // Riding the swell: kept awake (its turns, where it stops a
            // moment at the top and the bottom, would otherwise let it sleep).
            // (Only what floats: a body sunk to the bed sleeps.)
            if ((here->slope_x != 0.0f || here->slope_z != 0.0f) && (submerged < volume * 0.999f || buoyancy > 1.0f)) {
                b.ResetSleepTimer();
            }
            const JPH::Vec3 flow(here->flow_x, 0.0f, here->flow_z);
            // Bobbing settles (the waves a moving body makes carry its
            // motion off, which quadratic drag alone leaves for a long
            // time): its speed against the water lost by a share, by how
            // much is under and how light it is for its size (a sunk rock
            // is not swept along a river bed). Before the buoyancy, so what
            // holds it up against gravity is not taken off with it.
            const float under = std::min(2.0f * submerged / volume, 1.0f);
            const float keep = std::max(0.0f, 1.0f - k_water_damping * under * std::min(buoyancy, 1.0f) * dt);
            // Towards the flow only what floats (or nearly): a sunk body is
            // left to the drag (a rock rolls only in a fast river).
            const JPH::Vec3 along = flow * std::clamp((buoyancy - 0.9f) / 0.1f, 0.0f, 1.0f);
            b.SetLinearVelocityClamped(along + (b.GetLinearVelocity() - along) * keep);
            b.SetAngularVelocityClamped(b.GetAngularVelocity() * keep);
            b.ApplyBuoyancyImpulse(volume, submerged, buoyed_at, buoyancy, k_water_drag, k_water_turn_drag, flow,
                                   gravity, dt);
        }
    }

    // Wakes the sleeping moving bodies the water reaches (each part's
    // ground, from the deep up to its top: the sea's everywhere under it),
    // so they float, or fall where it went.
    void wake_in_water() {
        for (const WaterPart& part : water_parts) {
            constexpr float far = 1.0e7f;
            const bool sea = water[part.water].kind == world::WaterKind::sea;
            const float top = std::isfinite(part.top) ? part.top + (sea ? static_cast<float>(swell_reach()) : 0.0f) : far;
            const JPH::AABox box = part.everywhere
                                       ? JPH::AABox(JPH::Vec3(-far, -far, -far), JPH::Vec3(far, top, far))
                                       : JPH::AABox(JPH::Vec3(static_cast<float>(part.x0), -far, static_cast<float>(part.z0)),
                                                    JPH::Vec3(static_cast<float>(part.x1), top, static_cast<float>(part.z1)));
            wake_in(box);
        }
    }

    JPH::BodyInterface& bodies_interface() { return system.GetBodyInterface(); }
    const JPH::BodyInterface& bodies_interface() const { return system.GetBodyInterface(); }

    JPH::Body* create(const BodyDesc& d, const JPH::RefConst<JPH::Shape>& shape) {
        if (shape == nullptr || !valid_desc(d)) return nullptr;
        if (d.motion != Motion::fixed && shape->GetSubType() == JPH::EShapeSubType::HeightField) return nullptr;
        const bool moving = d.motion != Motion::fixed;
        const JPH::EMotionType motion = d.motion == Motion::fixed       ? JPH::EMotionType::Static
                                        : d.motion == Motion::kinematic ? JPH::EMotionType::Kinematic
                                                                        : JPH::EMotionType::Dynamic;
        JPH::BodyCreationSettings s(shape, to_jolt(d.position), to_jolt(d.rotation), motion,
                                    moving ? layers::moving : layers::fixed);
        s.mFriction = std::max(d.friction, 0.0f);
        s.mRestitution = std::clamp(d.restitution, 0.0f, 1.0f);
        s.mLinearVelocity = to_jolt(d.linear_velocity);
        s.mAngularVelocity = to_jolt(d.angular_velocity);
        s.mUserData = d.user;
        s.mUseManifoldReduction = !d.report_parts; // merged manifolds keep one part's id
        if (d.motion == Motion::dynamic) {
            if (d.mass > 0.0f) {
                s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                s.mMassPropertiesOverride.mMass = d.mass;
            }
        }
        if (d.motion == Motion::kinematic) {
            // A giantess's foot pushes sleeping buildings awake.
            s.mCollideKinematicVsNonDynamic = false;
        }
        return bodies_interface().CreateBody(s);
    }

    // Wakes the sleeping moving bodies in or touching a box (what rested on
    // a fixed body that goes or moves).
    void wake_in(const JPH::AABox& box) {
        JPH::AABox grown = box;
        grown.ExpandBy(JPH::Vec3::sReplicate(0.05f));
        bodies_interface().ActivateBodiesInAABox(grown, JPH::SpecifiedBroadPhaseLayerFilter(JPH::BroadPhaseLayer(layers::moving)),
                                                 JPH::SpecifiedObjectLayerFilter(layers::moving));
    }
    // Around a fixed or dynamic body (a crate under another one); a
    // kinematic one pushes what it meets itself.
    void wake_around(JPH::BodyID b) {
        if (bodies_interface().GetMotionType(b) == JPH::EMotionType::Kinematic) return;
        const JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), b);
        if (lock.Succeeded()) wake_in(lock.GetBody().GetWorldSpaceBounds());
    }

    // A character's Jolt half, at `position` moving at `velocity`; null
    // when its shapes could not be made.
    JPH::Ref<JPH::CharacterVirtual> make_character(const CharacterDesc& d, const DVec3& position, const Vec3& velocity) {
        JPH::RefConst<JPH::Shape> shape = standing_capsule(d.height, d.radius);
        // The body others meet: a little narrower than the character (it
        // keeps that far from things, so it never holds the character back
        // against a wall), and reaching past the top of its padded shape, so
        // something resting on its head rests clear of it: a crate on the
        // head inside the shape would pin it, the floor below and the crate
        // above leaving no way to move.
        JPH::RefConst<JPH::Shape> inner = standing_capsule(d.height + 0.03f, d.radius - 0.02f);
        if (shape == nullptr || inner == nullptr) return nullptr;
        JPH::Ref<JPH::CharacterVirtualSettings> s = new JPH::CharacterVirtualSettings();
        s->mShape = shape;
        s->mMaxSlopeAngle = JPH::DegreesToRadians(d.max_slope_degrees);
        s->mMass = d.mass;
        s->mMaxStrength = d.strength;
        // What touches the lower half-sphere holds it up; the rest only stops it.
        s->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -d.radius);
        s->mEnhancedInternalEdgeRemoval = true; // no snagging on a height field's triangle edges
        s->mInnerBodyShape = inner;
        s->mInnerBodyLayer = layers::moving;
        JPH::Ref<JPH::CharacterVirtual> c =
            new JPH::CharacterVirtual(s, to_jolt(position), JPH::Quat::sIdentity(), d.user, &system);
        c->SetLinearVelocity(to_jolt(velocity));
        return c;
    }

    // A ray for a character's own looks (a wall, a ledge): what it meets
    // first but the character's own body (and other characters').
    // (Plain types, not Jolt's SIMD ones: no alignment padding.)
    struct Hit {
        float distance = 0.0f;
        DVec3 point{};
        Vec3 normal{};
        JPH::BodyID body;
        bool dynamic = false; // pushed about: nothing to hold on to
    };
    std::optional<Hit> look(const JPH::RVec3& from, const JPH::Vec3& along, float length) const {
        const JPH::RRayCast ray(from, along * length);
        JPH::RayCastResult result;
        struct NotCharacters final : JPH::BodyFilter {
            const Impl* impl = nullptr;
            bool ShouldCollideLocked(const JPH::Body& body) const override { return !impl->is_character_body(body.GetID()); }
        } filter;
        filter.impl = this;
        if (!system.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, filter)) return std::nullopt;
        Hit hit;
        hit.distance = result.mFraction * length;
        const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
        hit.point = from_jolt_r(point);
        hit.body = result.mBodyID;
        const JPH::BodyLockRead lock(system.GetBodyLockInterface(), result.mBodyID);
        if (!lock.Succeeded()) return std::nullopt;
        hit.normal = from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
        hit.dynamic = lock.GetBody().IsDynamic();
        return hit;
    }

    // How fast the surface of `body` at `point` moves (a giant's leg climbed).
    JPH::Vec3 surface_velocity(JPH::BodyID body, const JPH::RVec3& point) const {
        const JPH::BodyLockRead lock(system.GetBodyLockInterface(), body);
        return lock.Succeeded() ? lock.GetBody().GetPointVelocity(point) : JPH::Vec3::sZero();
    }

    // Where a mantle has the feet: up by `rise`, over by `over` (0 to 1).
    static DVec3 mantle_point(const Character& c, double rise, double over) {
        return {c.mantle_from.x + (c.mantle_to.x - c.mantle_from.x) * over,
                c.mantle_from.y + (c.mantle_to.y - c.mantle_from.y) * rise,
                c.mantle_from.z + (c.mantle_to.z - c.mantle_from.z) * over};
    }

    // Room for the character standing at `feet` (its shape a little smaller:
    // touching is room). Characters' bodies are left out.
    bool clear_at(Character& c, const JPH::RVec3& feet) const {
        if (c.room == nullptr) c.room = standing_capsule(c.desc.height - 0.06f, c.desc.radius - 0.03f);
        const JPH::RefConst<JPH::Shape>& shape = c.room;
        if (shape == nullptr) return false;
        const JPH::RVec3 at = feet + JPH::Vec3(0.0f, 0.03f, 0.0f);
        JPH::CollideShapeSettings collide;
        collide.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> hit;
        struct NotCharacters final : JPH::BodyFilter {
            const Impl* impl = nullptr;
            bool ShouldCollideLocked(const JPH::Body& body) const override { return !impl->is_character_body(body.GetID()); }
        } filter;
        filter.impl = this;
        system.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sOne(),
                                                  JPH::RMat44::sTranslation(at) * JPH::Mat44::sTranslation(shape->GetCenterOfMass()),
                                                  collide, at, hit, {}, {}, filter);
        return !hit.HadHit();
    }

    // Pulling itself up from `feet` onto `ledge` (not a dynamic body) at `to`.
    void start_mantle(Character& c, const JPH::RVec3& feet, const JPH::RVec3& to, const Hit& ledge) {
        c.mantling = true;
        c.mantle_t = 0.0f;
        c.mantle_from = from_jolt_r(feet);
        c.mantle_to = from_jolt_r(to);
        c.mantle_on = JPH::BodyID();
        const JPH::BodyLockRead lock(system.GetBodyLockInterface(), ledge.body);
        if (lock.Succeeded() && !lock.GetBody().IsStatic()) { // a moving ledge: in its frame
            const JPH::RMat44 into = lock.GetBody().GetWorldTransform().Inversed();
            c.mantle_on = ledge.body;
            c.mantle_from = from_jolt_r(into * feet);
            c.mantle_to = from_jolt_r(into * to);
        }
    }

    // Swimming into a bank or an edge whose top is out of reach of
    // walking up (over k_swim_out_least above its feet, up to
    // k_swim_out_over over the surface: a jetty, a shelf): pulls itself up
    // onto it, as onto a ledge climbed. Higher banks are climbed (climb
    // input) or not left by.
    bool swim_out(Character& c, const world::WaterHere& wet, JPH::Vec3Arg want) {
        if (want.LengthSq() < 0.25f) return false;
        const JPH::CharacterVirtual& cv = *c.body;
        const JPH::Vec3 up = JPH::Vec3::sAxisY();
        const JPH::Vec3 dir = want.Normalized();
        const JPH::RVec3 feet = cv.GetPosition();
        // Only at the surface (not diving under a jetty), onto an edge
        // shallow enough to stand on (not more of the bed), out of reach of
        // walking up.
        const double surface = static_cast<double>(wet.level);
        if (static_cast<double>(feet.GetY()) < surface - static_cast<double>(swim_depth(c.desc)) - 0.2) return false;
        const double highest = surface + k_swim_out_over;
        const double lowest = std::max({static_cast<double>(feet.GetY()) + std::max(k_swim_out_least, c.desc.step_height),
                                        surface - static_cast<double>(swim_depth(c.desc) - k_swim_leave)});
        if (!(highest > lowest)) return false;
        const JPH::RVec3 above(feet.GetX(), static_cast<JPH::Real>(highest + 0.05), feet.GetZ());
        // Room over the ledge's edge first (no reaching through a wall).
        const float ahead = c.desc.radius + 0.3f;
        if (look(above, dir, ahead)) return false;
        const std::optional<Hit> ledge = look(above + dir * ahead, -up, static_cast<float>(highest + 0.05 - lowest));
        const float steep = std::cos(JPH::DegreesToRadians(c.desc.max_slope_degrees));
        if (!ledge || ledge->dynamic || ledge->normal.y < steep) return false;
        const JPH::RVec3 to = to_jolt(ledge->point) + up * 0.02f;
        const JPH::RVec3 risen(feet.GetX(), to.GetY(), feet.GetZ());
        if (!clear_at(c, (feet + risen) * 0.5) || !clear_at(c, risen) || !clear_at(c, (risen + to) * 0.5) ||
            !clear_at(c, to)) {
            return false;
        }
        start_mantle(c, feet, to, *ledge);
        return true;
    }

    // One fixed step on a wall, or onto its ledge. False: not climbing (any
    // more): the step goes on as walking or falling.
    bool climb_step(Character& c, float dt) {
        JPH::CharacterVirtual& cv = *c.body;
        const JPH::Vec3 up = JPH::Vec3::sAxisY();
        const float h = c.desc.height;
        const float r = c.desc.radius;
        const float steep = std::cos(JPH::DegreesToRadians(c.desc.max_slope_degrees));
        c.climb_wait = std::max(0.0f, c.climb_wait - dt);
        if (c.mantling) {
            // Up first, then over the edge (a jump pressed meanwhile lasts its fifth of a second).
            c.jump_wait = std::max(0.0f, c.jump_wait - dt);
            c.mantle_t = std::min(1.0f, c.mantle_t + dt / 0.45f);
            const double t = c.mantle_t;
            const double rise = std::min(1.0, t / 0.6);
            const double over = std::clamp((t - 0.6) / 0.4, 0.0, 1.0);
            DVec3 at = mantle_point(c, rise, over);
            const JPH::RVec3 from = cv.GetPosition();
            if (!c.mantle_on.IsInvalid()) {
                // On a moving ledge: where its frame has the point now.
                const JPH::BodyLockRead lock(system.GetBodyLockInterface(), c.mantle_on);
                if (lock.Succeeded()) {
                    at = from_jolt_r(lock.GetBody().GetWorldTransform() * to_jolt(at));
                } else { // the ledge went: falls from where it is
                    c.mantling = false;
                    c.climbing = false;
                    c.mantle_on = JPH::BodyID();
                }
            }
            if (!c.mantling) {
                refresh(cv);
                return false;
            }
            cv.SetPosition(to_jolt(at));
            c.velocity = from_jolt(JPH::Vec3(cv.GetPosition() - from) / dt);
            cv.SetLinearVelocity(JPH::Vec3::sZero());
            c.vertical = 0.0f;
            if (c.mantle_t >= 1.0f) {
                c.mantling = false;
                c.climbing = false;
                c.mantle_on = JPH::BodyID();
                c.velocity = {};
                refresh(cv);
            }
            return true;
        }
        const JPH::Vec3 want(c.input.velocity.x, 0.0f, c.input.velocity.z);
        const JPH::RVec3 feet = cv.GetPosition();
        const JPH::RVec3 chest = feet + up * (0.5f * h);
        if (!c.climbing) {
            // Taking hold: walking into something too steep to walk on.
            // (A jump asked for is a jump, not a push off a wall just met.)
            if (!c.input.climb || c.climb_wait > 0.0f || c.jump_wait > 0.0f || want.LengthSq() < 0.25f) return false;
            const JPH::Vec3 dir = want.Normalized();
            const std::optional<Hit> wall = look(chest, dir, r + 0.35f);
            if (!wall || wall->dynamic) return false; // (a crate walked into is pushed)
            JPH::Vec3 flat(wall->normal.x, 0.0f, wall->normal.z);
            if (std::fabs(wall->normal.y) >= steep || flat.LengthSq() < 1e-4f) return false;
            flat = flat.Normalized();
            if (flat.Dot(dir) > -0.5f) return false; // along it, not into it
            c.climbing = true;
            c.wall = wall->body;
            c.wall_normal = from_jolt(flat);
        }
        const JPH::Vec3 n = to_jolt(c.wall_normal);
        if (!c.input.climb) {
            c.climbing = false;
            return false;
        }
        // Pushed off: away and up, falling from there.
        if (c.jump_wait > 0.0f) {
            const JPH::Vec3 push = n * (0.5f * c.jump_speed) + up * (0.7f * c.jump_speed);
            c.climbing = false;
            c.climb_wait = 0.4f;
            c.jump_wait = 0.0f;
            c.velocity = from_jolt(push);
            c.vertical = push.GetY();
            cv.SetLinearVelocity(push);
            return false;
        }
        const std::optional<Hit> wall = look(chest, -n, r + 0.6f);
        const JPH::Vec3 across = up.Cross(n).Normalized();
        float climb = want.Dot(-n); // into it: up
        float side = want.Dot(across);
        const float speed = std::sqrt(climb * climb + side * side);
        if (speed > c.desc.climb_speed) {
            climb *= c.desc.climb_speed / speed;
            side *= c.desc.climb_speed / speed;
        }
        // The top: the wall goes on at the chest but not over the head:
        // up onto the ledge, if there is ground there with room to stand.
        if (wall && !wall->dynamic && climb > 0.0f && !look(feet + up * (h + 0.05f), -n, r + 0.6f)) {
            const JPH::RVec3 above = feet + up * (h + 0.6f) - n * (r + 0.4f);
            const std::optional<Hit> ledge = look(above, -up, h + 0.6f);
            if (ledge && ledge->normal.y >= steep && ledge->point.y > feet.GetY() + 0.3) {
                const JPH::RVec3 to = to_jolt(ledge->point) + up * 0.02f;
                // Room on the way: risen beside the wall, halfway over, and there.
                const JPH::RVec3 risen(feet.GetX(), to.GetY(), feet.GetZ());
                if (clear_at(c, risen) && clear_at(c, (risen + to) * 0.5) && clear_at(c, to)) {
                    start_mantle(c, feet, to, *ledge);
                    return climb_step(c, dt);
                }
            }
        }
        if (!wall || wall->dynamic || std::fabs(wall->normal.y) >= steep) {
            // Nothing to hold: past an edge, onto ground it can walk, or what
            // it held came loose (a wall knocked down).
            c.climbing = false;
            return false;
        }
        JPH::Vec3 flat(wall->normal.x, 0.0f, wall->normal.z);
        if (flat.LengthSq() > 1e-4f) c.wall_normal = from_jolt(flat.Normalized());
        c.wall = wall->body;
        // Down on the ground, climbing down: lets go.
        if (climb < 0.0f && cv.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) {
            c.climbing = false;
            return false;
        }
        // Kept just off the wall, moving with it.
        const float gap = wall->distance - r;
        const JPH::Vec3 hold = -n * std::clamp((gap - 0.05f) * 10.0f, -1.0f, 3.0f);
        JPH::Vec3 out = up * climb + across * side + hold + surface_velocity(wall->body, to_jolt(wall->point));
        const float fast = out.Length();
        if (!(fast <= k_max_character_speed)) out = fast > 0.0f ? out * (k_max_character_speed / fast) : JPH::Vec3::sZero();
        cv.SetLinearVelocity(out);
        const JPH::RVec3 from = cv.GetPosition();
        cv.Update(dt, JPH::Vec3::sZero(), system.GetDefaultBroadPhaseLayerFilter(layers::moving),
                  system.GetDefaultLayerFilter(layers::moving), {}, {}, temp);
        c.velocity = from_jolt(JPH::Vec3(cv.GetPosition() - from) / dt);
        c.vertical = 0.0f;
        return true;
    }

    // After a step: a carried character where its carrier has it now.
    // Its speed is the carrier's there (a body put somewhere at once
    // carries it there at once, not flinging it on when let go).
    void carry(Character& c) {
        JPH::RVec3 to;
        JPH::Vec3 v;
        {
            // (Let go before moving the character: that locks its own body.)
            const JPH::BodyLockRead lock(system.GetBodyLockInterface(), c.carrier);
            if (!lock.Succeeded()) { // the carrier went: let go
                c.carrier = JPH::BodyID();
                return;
            }
            to = lock.GetBody().GetWorldTransform() * to_jolt(c.carry_at);
            v = lock.GetBody().GetPointVelocity(to);
        }
        const float speed = v.Length();
        if (!(speed <= k_max_character_speed)) v = speed > 0.0f ? v * (k_max_character_speed / speed) : JPH::Vec3::sZero();
        c.body->SetPosition(to);
        c.body->SetLinearVelocity(v);
        refresh(*c.body); // what it touches where it is held (held up in the air: not on the ground)
        c.velocity = from_jolt(v);
        c.vertical = v.GetY();
    }

    void refresh(JPH::CharacterVirtual& c) {
        c.RefreshContacts(system.GetDefaultBroadPhaseLayerFilter(layers::moving),
                          system.GetDefaultLayerFilter(layers::moving), {}, {}, temp);
    }

    // One fixed step of a character: its velocity from its input, then
    // moved (sliding along what it meets, up steps, down onto the floor).
    void step_character(Character& c, float dt) {
        // The water over its feet: deep enough, it swims (until a little
        // shallower: no flicker at the edge of a shelf).
        const JPH::RVec3 feet = c.body->GetPosition();
        std::optional<world::WaterHere> wet =
            water_parts.empty() ? std::nullopt
                                : water_over(static_cast<double>(feet.GetX()), static_cast<double>(feet.GetZ()),
                                             static_cast<double>(feet.GetY()));
        // Under a roof (a tunnel under the lake): dry. (From the head up: all under.)
        const JPH::RVec3 head = feet + JPH::Vec3(0.0f, c.desc.height + 0.01f, 0.0f);
        if (wet && static_cast<double>(head.GetY()) < static_cast<double>(wet->level) &&
            roofed(head, static_cast<double>(wet->level), c.desc.height + 0.02f)) {
            wet.reset();
        }
        const float deep = swim_depth(c.desc);
        c.surface_fall = wet ? surface_fall(*wet, static_cast<double>(feet.GetX()), static_cast<double>(feet.GetZ()), dt) : 0.0f;
        c.water_depth = wet ? static_cast<float>(static_cast<double>(wet->level) - static_cast<double>(feet.GetY())) : 0.0f;
        c.swimming = wet && c.water_depth > (c.swimming ? deep - k_swim_leave : deep);
        // Climbing down into water deep enough to swim: lets go of the wall.
        if (c.climbing && !c.mantling && c.water_depth > deep &&
            to_jolt(c.input.velocity).Dot(to_jolt(c.wall_normal)) > 0.1f) {
            c.climbing = false;
        }
        if (climb_step(c, dt)) {
            c.swimming = false; // (holding on, out of it)
            return;
        }
        if (c.swimming && wet) {
            // (A plain copy: GCC's optimizer cannot tell the optional is set.)
            const world::WaterHere water_here = wet.value_or(world::WaterHere{});
            if (swim_out(c, water_here, to_jolt(c.input.velocity) * JPH::Vec3(1.0f, 0.0f, 1.0f)) && climb_step(c, dt)) {
                c.swimming = false;
                return;
            }
            swim_step(c, water_here, dt);
            return;
        }
        JPH::CharacterVirtual& cv = *c.body;
        const JPH::Vec3 up = JPH::Vec3::sAxisY();
        const JPH::Vec3 gravity = system.GetGravity();
        cv.UpdateGroundVelocity(); // what it stands on may have changed speed
        // How it really moved: a wall, a ceiling or a slope stopped what it asked for.
        const JPH::Vec3 v = to_jolt(c.velocity);
        const JPH::Vec3 ground = cv.GetGroundVelocity();
        JPH::Vec3 want(c.input.velocity.x, 0.0f, c.input.velocity.z);
        if (c.water_depth > k_wade_from) { // wading: the deeper, the slower
            const float wading = std::min((c.water_depth - k_wade_from) / std::max(deep - k_wade_from, 0.01f), 1.0f);
            want *= 1.0f - (1.0f - k_wade_slowest) * wading;
        }
        // Not on its way up off the ground (a jump), by the speed it was
        // given: walking up a slope rises without leaving it.
        const bool towards_ground = c.vertical - ground.GetY() < 0.1f;
        JPH::Vec3 out;
        if (cv.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround && towards_ground) {
            // Walking: its speed over the ground, carried along with it.
            const JPH::Vec3 ground_flat(ground.GetX(), 0.0f, ground.GetZ());
            const JPH::Vec3 over = JPH::Vec3(v.GetX(), 0.0f, v.GetZ()) - ground_flat;
            out = ground + approach(over, want, c.input.acceleration * dt);
            if (c.jump_wait > 0.0f) {
                out += up * c.jump_speed;
                c.jump_wait = 0.0f;
            }
        } else {
            // In the air: its own speed up or down, slowed where something
            // stopped it (a ceiling, a slope it slides on), never turned
            // round by how a step up or an edge moved it.
            float vy = c.vertical;
            if (vy > 0.0f) vy = std::min(vy, std::max(v.GetY(), 0.0f));
            else vy = std::max(vy, std::min(v.GetY(), 0.0f));
            const JPH::Vec3 flat(v.GetX(), 0.0f, v.GetZ());
            out = approach(flat, want, c.input.air_acceleration * dt) + up * vy;
            c.jump_wait = std::max(0.0f, c.jump_wait - dt);
        }
        out += gravity * dt;
        // A fall's speed has an end (air): no endless speed-up into a world still empty.
        out.SetY(std::clamp(out.GetY(), -k_max_character_speed, k_max_character_speed));
        c.vertical = out.GetY();
        cv.SetLinearVelocity(out);
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(c.desc.step_height, 0.05f), 0.0f);
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, c.desc.step_height, 0.0f);
        const JPH::RVec3 from = cv.GetPosition();
        cv.ExtendedUpdate(dt, gravity, update, system.GetDefaultBroadPhaseLayerFilter(layers::moving),
                          system.GetDefaultLayerFilter(layers::moving), {}, {}, temp);
        c.velocity = from_jolt(JPH::Vec3(cv.GetPosition() - from) / dt);
    }

    // A fixed step swimming: through the water at its input (carried by the
    // flow), up to the surface and no higher, down diving; with no input up
    // or down it floats up to where it rests (feet swim_depth under the surface).
    void swim_step(Character& c, const world::WaterHere& wet, float dt) {
        JPH::CharacterVirtual& cv = *c.body;
        const JPH::Vec3 v = to_jolt(c.velocity);
        const JPH::Vec3 flow(wet.flow_x, 0.0f, wet.flow_z);
        JPH::Vec3 want(c.input.velocity.x, 0.0f, c.input.velocity.z);
        const float across = want.Length();
        if (across > c.desc.swim_speed) want *= c.desc.swim_speed / across;
        const JPH::Vec3 flat(v.GetX(), 0.0f, v.GetZ());
        JPH::Vec3 out = flow + approach(flat - flow, want, k_swim_acceleration * dt);
        // How far under where it rests its feet are (less than 0: above, as a fall in).
        const float under = (wet.level - swim_depth(c.desc)) - static_cast<float>(cv.GetPosition().GetY());
        const float asked = std::clamp(c.input.velocity.y, -c.desc.swim_speed, c.desc.swim_speed);
        const float to_rest = k_swim_hold * under; // up to where it rests, never past it
        const float wanted = asked < 0.0f ? asked : std::min(std::max(asked, k_swim_float_up), to_rest);
        // (Water's drag growing with the speed squared: a fall in slows within metres.)
        const float brake = k_swim_vertical_acceleration + k_swim_drag * v.GetY() * v.GetY();
        out.SetY(approach(v.GetY(), std::max(wanted, -k_max_character_speed), brake * dt));
        c.jump_wait = 0.0f; // (no jumping out of the water)
        c.vertical = out.GetY();
        cv.SetLinearVelocity(out);
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mStickToFloorStepDown = JPH::Vec3::sZero(); // afloat: not pulled down onto the bed
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, c.desc.step_height, 0.0f);
        const JPH::RVec3 from = cv.GetPosition();
        cv.ExtendedUpdate(dt, system.GetGravity(), update, system.GetDefaultBroadPhaseLayerFilter(layers::moving),
                          system.GetDefaultLayerFilter(layers::moving), {}, {}, temp);
        c.velocity = from_jolt(JPH::Vec3(cv.GetPosition() - from) / dt);
    }

    std::vector<BodyId> add(std::span<const BodyDesc> descs, std::span<const JPH::RefConst<JPH::Shape>> shapes) {
        std::vector<BodyId> out(descs.size(), k_no_body);
        std::vector<JPH::BodyID> moving;
        std::vector<JPH::BodyID> fixed;
        for (usize i = 0; i < descs.size(); ++i) {
            JPH::Body* body = create(descs[i], shapes[i]);
            if (body == nullptr) continue; // bad description, or the world is full
            (descs[i].motion == Motion::fixed ? fixed : moving).push_back(body->GetID());
            out[i] = to_id(body->GetID());
        }
        // Moving bodies start awake (they may be falling), fixed ones asleep.
        JPH::BodyInterface& bi = bodies_interface();
        for (auto* group : {&fixed, &moving}) {
            if (group->empty()) continue;
            const JPH::BodyInterface::AddState state = bi.AddBodiesPrepare(group->data(), static_cast<int>(group->size()));
            bi.AddBodiesFinalize(group->data(), static_cast<int>(group->size()), state,
                                 group == &moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        }
        bodies += static_cast<u32>(fixed.size() + moving.size());
        return out;
    }
};

PhysicsWorld::PhysicsWorld(JobSystem& jobs, const PhysicsSettings& settings)
    : m_impl(std::make_unique<Impl>(jobs, settings)) {}

struct PhysicsWorld::Disposal {
    std::thread thread;
    std::atomic<bool> done{false};
};

PhysicsWorld::~PhysicsWorld() {
    m_impl.reset();
    for (const std::unique_ptr<Disposal>& d : m_disposals) d->thread.join();
}

void PhysicsWorld::clear() {
    auto fresh = std::make_unique<Impl>(m_impl->jobs.jobs(), m_impl->settings);
    fresh->steps = m_impl->steps;
    fresh->dropped = m_impl->dropped;
    fresh->next_character = m_impl->next_character;
    fresh->carried = m_impl->carried;
    fresh->set_water(m_impl->water);
    fresh->swell = m_impl->swell;
    fresh->swell_seconds = m_impl->swell_seconds;
    // Characters carry on in the fresh world where they were.
    for (auto& [id, c] : m_impl->characters) {
        Impl::Character moved = c;
        moved.carrier = JPH::BodyID(); // (its carrier went with the old world)
        moved.climbing = false;
        moved.mantling = false;
        moved.body = fresh->make_character(c.desc, from_jolt_r(c.body->GetPosition()), c.velocity);
        if (moved.body != nullptr) {
            fresh->index_character(moved);
            fresh->characters.emplace(id, std::move(moved));
        }
    }
    m_impl->characters.clear(); // their bodies out of the old world here, not on its thread
    std::swap(m_impl, fresh);
    m_moved.clear();
    m_hits.clear();
    // Letting tens of thousands of bodies go takes tens of milliseconds: on
    // a thread of its own, never waited for here (one still going when the
    // next comes just runs beside it).
    std::erase_if(m_disposals, [](const std::unique_ptr<Disposal>& d) {
        if (!d->done.load(std::memory_order_acquire)) return false;
        d->thread.join(); // finished: at once
        return true;
    });
    // At most two old worlds in memory (a script loading scenes in a loop
    // waits here instead of piling them up).
    while (m_disposals.size() >= 2) {
        m_disposals.front()->thread.join();
        m_disposals.erase(m_disposals.begin());
    }
    auto disposal = std::make_unique<Disposal>();
    Disposal* d = disposal.get();
    d->thread = std::thread([d, old = std::move(fresh)]() mutable {
        old.reset();
        d->done.store(true, std::memory_order_release);
    });
    m_disposals.push_back(std::move(disposal));
}

BodyId PhysicsWorld::add(const BodyDesc& desc) {
    const std::vector<BodyId> ids = add_many(std::span<const BodyDesc>(&desc, 1));
    return ids.front();
}

std::vector<BodyId> PhysicsWorld::add_many(std::span<const BodyDesc> descs) {
    std::vector<JPH::RefConst<JPH::Shape>> shapes;
    shapes.reserve(descs.size());
    for (const BodyDesc& d : descs) shapes.push_back(valid_desc(d) ? body_shape(d) : nullptr);
    return m_impl->add(descs, shapes);
}

PreparedShape PhysicsWorld::prepare(const BodyDesc& desc) const {
    PreparedShape out;
    if (!valid_desc(desc)) return out;
    JPH::RefConst<JPH::Shape> shape = body_shape(desc);
    if (shape != nullptr) out.m_data = std::make_shared<const PreparedShapeData>(PreparedShapeData{std::move(shape)});
    return out;
}

std::vector<BodyId> PhysicsWorld::add_many(std::span<const BodyDesc> descs, std::span<const PreparedShape> shapes) {
    GIANT_ASSERT(descs.size() == shapes.size(), "physics: a prepared shape for each body");
    std::vector<JPH::RefConst<JPH::Shape>> made;
    made.reserve(shapes.size());
    for (const PreparedShape& s : shapes) made.push_back(s.valid() ? s.m_data->shape : nullptr);
    return m_impl->add(descs, made);
}

void PhysicsWorld::remove(BodyId id) { remove_many(std::span<const BodyId>(&id, 1)); }

void PhysicsWorld::remove_many(std::span<const BodyId> ids, bool wake_neighbours) {
    std::vector<JPH::BodyID> gone;
    gone.reserve(ids.size());
    JPH::BodyInterface& bi = m_impl->bodies_interface();
    for (const BodyId id : ids) {
        const JPH::BodyID b = from_id(id);
        if (!b.IsInvalid() && bi.IsAdded(b) && !m_impl->is_character_body(b)) gone.push_back(b);
    }
    std::sort(gone.begin(), gone.end());
    gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
    if (gone.empty()) return;
    // Jolt leaves what slept on a removed body asleep, in the air.
    if (wake_neighbours) {
        for (const JPH::BodyID b : gone) m_impl->wake_around(b);
    }
    bi.RemoveBodies(gone.data(), static_cast<int>(gone.size()));
    bi.DestroyBodies(gone.data(), static_cast<int>(gone.size()));
    m_impl->bodies -= static_cast<u32>(gone.size());
}

bool PhysicsWorld::contains(BodyId id) const {
    const JPH::BodyID b = from_id(id);
    return !b.IsInvalid() && m_impl->bodies_interface().IsAdded(b);
}

usize PhysicsWorld::size() const { return m_impl->bodies; }

std::optional<BodyState> PhysicsWorld::state(BodyId id) const {
    if (!contains(id)) return std::nullopt;
    const JPH::BodyInterface& bi = m_impl->bodies_interface();
    const JPH::BodyID b = from_id(id);
    BodyState s;
    JPH::RVec3 position;
    JPH::Quat rotation;
    bi.GetPositionAndRotation(b, position, rotation);
    s.position = from_jolt_r(position);
    s.rotation = from_jolt(rotation);
    s.linear_velocity = from_jolt(bi.GetLinearVelocity(b));
    s.angular_velocity = from_jolt(bi.GetAngularVelocity(b));
    s.active = bi.IsActive(b);
    return s;
}

u64 PhysicsWorld::user(BodyId id) const {
    return contains(id) ? m_impl->bodies_interface().GetUserData(from_id(id)) : 0;
}

void PhysicsWorld::set_transform(BodyId id, const DVec3& position, const Quat& rotation) {
    if (!contains(id) || !finite(position) || !finite(rotation) || m_impl->is_character_body(from_id(id))) return;
    m_impl->wake_around(from_id(id)); // a fixed body: what rested on it where it was
    m_impl->bodies_interface().SetPositionAndRotation(from_id(id), to_jolt(position), to_jolt(rotation),
                                                      JPH::EActivation::Activate);
    m_impl->wake_around(from_id(id)); // and what it now touches
}

void PhysicsWorld::set_velocity(BodyId id, const Vec3& linear, const Vec3& angular) {
    if (!contains(id) || !finite(linear) || !finite(angular) || m_impl->is_character_body(from_id(id))) return;
    m_impl->bodies_interface().SetLinearAndAngularVelocity(from_id(id), to_jolt(linear), to_jolt(angular));
}

void PhysicsWorld::add_impulse(BodyId id, const Vec3& impulse, const DVec3& point) {
    if (!contains(id) || !finite(impulse) || !finite(point) || m_impl->is_character_body(from_id(id))) return;
    JPH::BodyInterface& bodies = m_impl->bodies_interface();
    if (bodies.GetMotionType(from_id(id)) != JPH::EMotionType::Dynamic) return;
    bodies.AddImpulse(from_id(id), to_jolt(impulse), to_jolt(point));
}

void PhysicsWorld::move_kinematic(BodyId id, const DVec3& position, const Quat& rotation) {
    if (!contains(id) || !finite(position) || !finite(rotation) || m_impl->is_character_body(from_id(id))) return;
    m_impl->bodies_interface().MoveKinematic(from_id(id), to_jolt(position), to_jolt(rotation),
                                             static_cast<float>(k_step_seconds));
}

u32 PhysicsWorld::step(double seconds) {
    const auto start = std::chrono::steady_clock::now();
    m_moved.clear();
    m_hits.clear();
    m_impl->entries.clear();
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        m_impl->last_step_ms = 0.0;
        return 0;
    }
    m_impl->carried += seconds;
    u32 run = 0;
    while (m_impl->carried >= k_step_seconds) {
        if (run == m_impl->settings.max_steps_per_call) {
            // Too far behind: drop the rest (slow motion, not a spiral of death).
            const double left = std::floor(m_impl->carried / k_step_seconds);
            m_impl->dropped += static_cast<u32>(std::min(left, 1e9));
            m_impl->carried = std::fmod(m_impl->carried, k_step_seconds);
            break;
        }
        m_impl->carried -= k_step_seconds;
        const usize step_entries = m_impl->entries.size();
        m_impl->float_bodies(static_cast<float>(k_step_seconds));
        // (This step's bodies in id order: found in the active list's. The characters follow, in id order.)
        std::sort(m_impl->entries.begin() + static_cast<std::ptrdiff_t>(step_entries), m_impl->entries.end(),
                  [](const WaterEntry& x, const WaterEntry& y) { return x.body < y.body; });
        m_impl->swell_seconds += k_step_seconds;
        for (auto& [id, c] : m_impl->characters) {
            c.previous = from_jolt_r(c.body->GetPosition());
            if (!c.carrier.IsInvalid() || c.held) { // (not stepped: out of the water as far as it knows)
                c.swimming = false;
                c.water_depth = 0.0f;
                // (Held: not moved, so wet or not as it was: let go in the water, no splash. Carried:
                // as where it is carried, after the step.)
            }
            if (!c.carrier.IsInvalid()) {
                c.jump_wait = 0.0f; // (carried after the step, below)
            } else if (!c.held) {
                // Its own speed down as it came (a fall or a jump's; not how a step down
                // stairs or a slope moved it, nor the ground it stands on going down: a
                // sinking raft, a lift), and across.
                float down = -c.vertical;
                if (c.body->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) {
                    down += c.body->GetGroundVelocity().GetY();
                }
                const float across = std::hypot(c.velocity.x, c.velocity.z);
                m_impl->step_character(c, static_cast<float>(k_step_seconds));
                const bool wet = c.water_depth > 0.0f;
                if (wet && !c.was_wet && std::isfinite(down) && down >= k_entry_speed &&
                    down - c.surface_fall >= k_entry_speed) {
                    // (Its depth was measured from where its feet were as the step began.)
                    WaterEntry& e = m_impl->entries.emplace_back();
                    e.character = id;
                    e.point = {c.previous.x, c.previous.y + static_cast<double>(c.water_depth), c.previous.z};
                    e.speed = down - c.surface_fall;
                    e.across = std::isfinite(across) ? across : 0.0f;
                    e.mass = c.desc.mass;
                    e.size = 2.0f * c.desc.radius;
                }
                c.was_wet = wet;
            } else {
                // A jump pressed while held still lasts only its fifth of a second.
                c.jump_wait = std::max(0.0f, c.jump_wait - static_cast<float>(k_step_seconds));
            }
        }
        const JPH::EPhysicsUpdateError error =
            m_impl->system.Update(static_cast<float>(k_step_seconds), 1, &m_impl->temp, &m_impl->jobs);
        m_impl->jobs.drain();
        for (auto& [id, c] : m_impl->characters) {
            if (!c.carrier.IsInvalid()) {
                m_impl->carry(c);
                // (Carried into the water: wet as let go there, no splash then.)
                const JPH::RVec3 feet = c.body->GetPosition();
                c.was_wet = !m_impl->water_parts.empty() &&
                            m_impl->water_over(static_cast<double>(feet.GetX()), static_cast<double>(feet.GetZ()),
                                               static_cast<double>(feet.GetY()))
                                .has_value();
            }
        }
        m_impl->hits.take(m_hits);
        if (error != JPH::EPhysicsUpdateError::None) {
            log_warning("physics", "a step ran short: too many body pairs or contacts");
        }
        ++run;
        // Awake in this step (one falling asleep in the last moved in it too).
        JPH::BodyIDVector active;
        m_impl->system.GetActiveBodies(JPH::EBodyType::RigidBody, active);
        for (const JPH::BodyID b : active) m_moved.push_back(to_id(b));
    }
    m_impl->steps += run;
    std::sort(m_hits.begin(), m_hits.end(), [](const ContactHit& x, const ContactHit& y) {
        return std::tie(x.a, x.b, x.part_a, x.part_b, x.impulse) < std::tie(y.a, y.b, y.part_a, y.part_b, y.impulse);
    });
    std::sort(m_moved.begin(), m_moved.end()); // the same order every run
    m_moved.erase(std::unique(m_moved.begin(), m_moved.end()), m_moved.end());
    m_impl->last_step_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return run;
}

CharacterId PhysicsWorld::add_character(const CharacterDesc& desc) {
    if (!valid_character(desc)) return k_no_character;
    Impl::Character c;
    c.desc = desc;
    c.body = m_impl->make_character(desc, desc.position, {});
    c.previous = desc.position;
    if (c.body == nullptr || c.body->GetInnerBodyID().IsInvalid()) return k_no_character; // full
    m_impl->refresh(*c.body); // standing (or not) from the start
    const CharacterId id = m_impl->next_character++;
    m_impl->index_character(c);
    m_impl->characters.emplace(id, std::move(c));
    return id;
}

void PhysicsWorld::remove_character(CharacterId id) {
    const auto it = m_impl->characters.find(id);
    if (it == m_impl->characters.end()) return;
    m_impl->wake_in(it->second.body->GetShape()->GetWorldSpaceBounds(it->second.body->GetCenterOfMassTransform(),
                                                                      JPH::Vec3::sOne()));
    m_impl->character_bodies.erase(it->second.body->GetInnerBodyID().GetIndexAndSequenceNumber());
    m_impl->characters.erase(it);
}

bool PhysicsWorld::contains_character(CharacterId id) const { return m_impl->characters.count(id) != 0; }

void PhysicsWorld::set_character_input(CharacterId id, const CharacterInput& input) {
    const auto it = m_impl->characters.find(id);
    if (it == m_impl->characters.end() || !valid_input(input)) return;
    it->second.input = input;
}

void PhysicsWorld::jump(CharacterId id, float speed) {
    const auto it = m_impl->characters.find(id);
    if (it == m_impl->characters.end() || !std::isfinite(speed) || speed <= 0.0f) return;
    it->second.jump_speed = std::min(speed, k_max_character_speed);
    it->second.jump_wait = k_jump_wait;
}

void PhysicsWorld::set_character_position(CharacterId id, const DVec3& position) {
    const auto it = m_impl->characters.find(id);
    if (it == m_impl->characters.end() || !finite(position)) return;
    it->second.body->SetPosition(to_jolt(position));
    it->second.previous = position;
    it->second.body->SetLinearVelocity(JPH::Vec3::sZero());
    it->second.velocity = {};
    it->second.vertical = 0.0f;
    it->second.jump_wait = 0.0f;
    it->second.climbing = false;
    it->second.mantling = false;
    it->second.carrier = JPH::BodyID();
    m_impl->refresh(*it->second.body);
}

void PhysicsWorld::set_character_velocity(CharacterId id, const Vec3& velocity) {
    const auto it = m_impl->characters.find(id);
    if (it == m_impl->characters.end() || !std::isfinite(velocity.x) || !std::isfinite(velocity.y) ||
        !std::isfinite(velocity.z))
        return;
    // (Scaled down first: a huge but finite speed has no finite length.)
    const float big = std::max({std::fabs(velocity.x), std::fabs(velocity.y), std::fabs(velocity.z)});
    JPH::Vec3 v = to_jolt(big > k_max_character_speed ? velocity * (k_max_character_speed / big) : velocity);
    const float speed = v.Length();
    if (speed > k_max_character_speed) v *= k_max_character_speed / speed;
    Impl::Character& c = it->second;
    c.body->SetLinearVelocity(v);
    c.velocity = from_jolt(v);
    // The speed up it is given: on the ground, what it moved up by (a step,
    // a slope) is not a jump, so no more than the ground's own.
    float vertical = v.GetY();
    if (c.body->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) {
        c.body->UpdateGroundVelocity();
        vertical = std::min(vertical, c.body->GetGroundVelocity().GetY());
    }
    c.vertical = vertical;
}

bool PhysicsWorld::take_hold(CharacterId id, const Vec3& wall_normal) {
    const auto it = m_impl->characters.find(id);
    if (it == m_impl->characters.end() || !it->second.carrier.IsInvalid()) return false;
    Impl::Character& c = it->second;
    JPH::Vec3 flat(wall_normal.x, 0.0f, wall_normal.z);
    if (!std::isfinite(flat.GetX()) || !std::isfinite(flat.GetZ()) || flat.LengthSq() < 1e-4f) return false;
    flat = flat.Normalized();
    const float steep = std::cos(JPH::DegreesToRadians(c.desc.max_slope_degrees));
    const JPH::RVec3 chest = c.body->GetPosition() + JPH::Vec3::sAxisY() * (0.5f * c.desc.height);
    const std::optional<Impl::Hit> wall = m_impl->look(chest, -flat, c.desc.radius + 0.35f);
    if (!wall || wall->dynamic || std::fabs(wall->normal.y) >= steep) return false;
    JPH::Vec3 met(wall->normal.x, 0.0f, wall->normal.z);
    if (met.LengthSq() < 1e-4f || met.Normalized().Dot(flat) < 0.5f) return false; // (another face)
    c.climbing = true;
    c.mantling = false;
    c.wall = wall->body;
    c.wall_normal = from_jolt(met.Normalized());
    return true;
}

bool PhysicsWorld::carry_character(CharacterId id, BodyId carrier) {
    const auto it = m_impl->characters.find(id);
    if (it == m_impl->characters.end() || !contains(carrier) || m_impl->is_character_body(from_id(carrier))) return false;
    const JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), from_id(carrier));
    if (!lock.Succeeded()) return false;
    Impl::Character& c = it->second;
    c.carrier = from_id(carrier);
    c.carry_at = from_jolt_r(lock.GetBody().GetWorldTransform().Inversed() * c.body->GetPosition());
    c.climbing = false;
    c.mantling = false;
    return true;
}

void PhysicsWorld::release_character(CharacterId id) {
    const auto it = m_impl->characters.find(id);
    if (it == m_impl->characters.end() || it->second.carrier.IsInvalid()) return;
    it->second.carrier = JPH::BodyID();
    m_impl->refresh(*it->second.body); // falls on as it was carried (its speed kept)
}

void PhysicsWorld::hold_character(CharacterId id, bool held) {
    const auto it = m_impl->characters.find(id);
    if (it != m_impl->characters.end()) it->second.held = held;
}

double PhysicsWorld::step_fraction() const { return std::clamp(m_impl->carried / k_step_seconds, 0.0, 1.0); }

std::optional<CharacterState> PhysicsWorld::character_state(CharacterId id) const {
    const auto it = m_impl->characters.find(id);
    if (it == m_impl->characters.end()) return std::nullopt;
    const JPH::CharacterVirtual& c = *it->second.body;
    CharacterState s;
    s.position = from_jolt_r(c.GetPosition());
    s.previous = it->second.previous;
    s.held = it->second.held;
    s.velocity = it->second.velocity;
    s.on_ground = c.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    s.on_steep = c.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnSteepGround;
    if (c.IsSupported()) s.ground_normal = from_jolt(c.GetGroundNormal());
    if (c.GetGroundState() != JPH::CharacterVirtual::EGroundState::InAir) s.ground = to_id(c.GetGroundBodyID());
    const Impl::Character& ch = it->second;
    s.climbing = ch.climbing || ch.mantling;
    s.mantling = ch.mantling;
    if (ch.climbing || ch.mantling) {
        s.wall = to_id(ch.wall);
        s.wall_normal = ch.wall_normal;
    }
    if (!ch.carrier.IsInvalid()) s.carrier = to_id(ch.carrier);
    s.swimming = ch.swimming;
    s.water_depth = ch.water_depth;
    return s;
}

BodyId PhysicsWorld::character_body(CharacterId id) const {
    const auto it = m_impl->characters.find(id);
    return it == m_impl->characters.end() ? k_no_body : to_id(it->second.body->GetInnerBodyID());
}

usize PhysicsWorld::characters() const { return m_impl->characters.size(); }

void PhysicsWorld::set_water(std::span<const world::Water> water, bool wake) {
    if (wake) m_impl->wake_in_water(); // where it was (gone or lowered: what floated falls)
    m_impl->set_water(water);
    if (wake) m_impl->wake_in_water(); // and where it is
}

void PhysicsWorld::wake_in_water() { m_impl->wake_in_water(); }

std::span<const world::Water> PhysicsWorld::water() const { return m_impl->water; }

void PhysicsWorld::set_swell(const world::Swell& swell, double seconds) {
    m_impl->swell = swell;
    // Its clock where the steps are: the time not yet stepped is still to come.
    m_impl->swell_seconds = (std::isfinite(seconds) ? seconds : 0.0) - m_impl->carried;
    // Risen by a quarter metre since the sleepers were last woken (from
    // none: at once): woken where it now reaches, so they float or bob.
    const double reach = m_impl->swell_reach();
    if (reach > m_impl->woken_reach + (m_impl->woken_reach > 0.0 ? 0.25 : 0.0)) {
        m_impl->wake_in_water();
        m_impl->woken_reach = reach;
    } else if (reach < m_impl->woken_reach) {
        m_impl->woken_reach = reach; // (fallen: what it left falls, awake as it rides)
    }
}

const world::Swell& PhysicsWorld::swell() const { return m_impl->swell; }

double PhysicsWorld::swell_seconds() const { return m_impl->swell_seconds; }

bool PhysicsWorld::water_reaches(const DVec3& point, double level) const {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) || !std::isfinite(level)) {
        return false;
    }
    if (!(point.y < level)) return true; // over it: nothing between
    return !m_impl->roofed(to_jolt(point), level, 0.0f);
}

const std::vector<WaterEntry>& PhysicsWorld::water_entries() const { return m_impl->entries; }

std::optional<world::WaterHere> PhysicsWorld::water_at(double x, double z) const {
    // (Its own scratch, kept: the stepping thread's is not this one's.)
    return m_impl->water_over(x, z, -std::numeric_limits<double>::infinity(), m_impl->water_asked);
}

bool PhysicsWorld::is_character_body(BodyId id) const {
    return id != k_no_body && m_impl->is_character_body(from_id(id));
}

std::optional<RayHit> PhysicsWorld::raycast(const DVec3& origin, const Vec3& direction, double max_distance) const {
    return raycast(origin, direction, max_distance, RayFilter{});
}

std::optional<RayHit> PhysicsWorld::raycast(const DVec3& origin, const Vec3& direction, double max_distance,
                                            const RayFilter& filter) const {
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!finite(origin) || !finite(direction) || length < 1e-9f || !std::isfinite(max_distance) ||
        max_distance <= 0.0) {
        return std::nullopt;
    }
    const float reach = static_cast<float>(max_distance) / length;
    const JPH::RRayCast ray(to_jolt(origin), to_jolt(direction) * reach);
    JPH::RayCastResult result;
    struct Filter final : JPH::BodyFilter {
        const RayFilter* f = nullptr;
        bool ShouldCollideLocked(const JPH::Body& body) const override {
            if (f->fixed_only && !body.IsStatic()) return false;
            if (f->body != k_no_body && to_id(body.GetID()) != f->body) return false;
            return !f->accept || f->accept(to_id(body.GetID()), body.GetUserData());
        }
    } body_filter;
    body_filter.f = &filter;
    // One part of a compound: its shape, held while the ray goes (the
    // filter is asked about each part with the id leading to it).
    JPH::RefConst<JPH::Shape> root;
    if (filter.body != k_no_body && filter.part) {
        if (!contains(filter.body)) return std::nullopt;
        const JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), from_id(filter.body));
        if (lock.Succeeded() && lock.GetBody().GetShape()->GetType() == JPH::EShapeType::Compound) {
            root = lock.GetBody().GetShape();
        }
    }
    if (root != nullptr) {
        struct PartFilter final : JPH::ShapeFilter {
            using JPH::ShapeFilter::ShouldCollide;
            const JPH::CompoundShape* compound = nullptr;
            u32 part = 0;
            bool ShouldCollide(const JPH::Shape* shape, const JPH::SubShapeID& id) const override {
                if (shape == compound) return true;
                JPH::SubShapeID rest;
                return compound->GetSubShapeIndexFromID(id, rest) == part;
            }
        } part_filter;
        part_filter.compound = static_cast<const JPH::CompoundShape*>(root.GetPtr());
        part_filter.part = *filter.part;
        JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> closest;
        m_impl->system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings{}, closest, {}, {}, body_filter,
                                                     part_filter);
        if (!closest.HadHit()) return std::nullopt;
        result = closest.mHit;
    } else if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, body_filter)) {
        return std::nullopt;
    }
    RayHit hit;
    hit.body = to_id(result.mBodyID);
    hit.distance = static_cast<double>(result.mFraction) * max_distance;
    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    hit.point = from_jolt_r(point);
    const JPH::BodyLockInterface& locks = m_impl->system.GetBodyLockInterface();
    const JPH::BodyLockRead lock(locks, result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        hit.user = body.GetUserData();
        hit.normal = from_jolt(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
        hit.part = part_of(body, result.mSubShapeID2);
    }
    return hit;
}

PhysicsStats PhysicsWorld::stats() const {
    PhysicsStats s;
    s.bodies = m_impl->bodies;
    s.characters = static_cast<u32>(m_impl->characters.size());
    // Characters' bodies never sleep: left out, so a world at rest reads 0.
    const u32 active = m_impl->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    s.active = active - std::min(active, s.characters);
    s.steps = m_impl->steps;
    s.dropped = m_impl->dropped;
    s.last_step_ms = m_impl->last_step_ms;
    return s;
}

} // namespace giant::sim
