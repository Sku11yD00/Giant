// PhysicsWorld on Jolt: layers, the job system bridge and the conversions.
#include "giant/sim/physics.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

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
#include <map>
#include <string>
#include <thread>
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
           d.step_height >= 0.0f && d.step_height <= 0.5f * d.height && d.mass > 0.0f && d.strength >= 0.0f;
}

// Fastest a character is told to go, jump or fall (metres a second): far
// past running, within what Jolt keeps exact.
constexpr float k_max_character_speed = 100.0f;

bool valid_input(const CharacterInput& in) {
    return finite(in.velocity) && std::fabs(in.velocity.x) <= k_max_character_speed &&
           std::fabs(in.velocity.z) <= k_max_character_speed && std::isfinite(in.acceleration) && in.acceleration >= 0.0f &&
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
    };
    std::map<CharacterId, Character> characters; // in id order: the same steps every run

    // A character's own body: moved with it, never by the body calls.
    // (Asked for every body a character's ray meets: a set, not a scan.)
    std::unordered_set<JPH::uint32> character_bodies;
    bool is_character_body(JPH::BodyID b) const { return character_bodies.contains(b.GetIndexAndSequenceNumber()); }
    void index_character(const Character& c) { character_bodies.insert(c.body->GetInnerBodyID().GetIndexAndSequenceNumber()); }
    CharacterId next_character = 1;

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
                    c.mantling = true;
                    c.mantle_t = 0.0f;
                    c.mantle_from = from_jolt_r(feet);
                    c.mantle_to = from_jolt_r(to);
                    c.mantle_on = JPH::BodyID();
                    if (!ledge->dynamic) {
                        const JPH::BodyLockRead lock(system.GetBodyLockInterface(), ledge->body);
                        if (lock.Succeeded() && !lock.GetBody().IsStatic()) { // a moving ledge: in its frame
                            const JPH::RMat44 into = lock.GetBody().GetWorldTransform().Inversed();
                            c.mantle_on = ledge->body;
                            c.mantle_from = from_jolt_r(into * feet);
                            c.mantle_to = from_jolt_r(into * to);
                        }
                    }
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
        if (climb_step(c, dt)) return;
        JPH::CharacterVirtual& cv = *c.body;
        const JPH::Vec3 up = JPH::Vec3::sAxisY();
        const JPH::Vec3 gravity = system.GetGravity();
        cv.UpdateGroundVelocity(); // what it stands on may have changed speed
        // How it really moved: a wall, a ceiling or a slope stopped what it asked for.
        const JPH::Vec3 v = to_jolt(c.velocity);
        const JPH::Vec3 ground = cv.GetGroundVelocity();
        const JPH::Vec3 want(c.input.velocity.x, 0.0f, c.input.velocity.z);
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

void PhysicsWorld::move_kinematic(BodyId id, const DVec3& position, const Quat& rotation) {
    if (!contains(id) || !finite(position) || !finite(rotation) || m_impl->is_character_body(from_id(id))) return;
    m_impl->bodies_interface().MoveKinematic(from_id(id), to_jolt(position), to_jolt(rotation),
                                             static_cast<float>(k_step_seconds));
}

u32 PhysicsWorld::step(double seconds) {
    const auto start = std::chrono::steady_clock::now();
    m_moved.clear();
    m_hits.clear();
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
        for (auto& [id, c] : m_impl->characters) {
            c.previous = from_jolt_r(c.body->GetPosition());
            if (!c.carrier.IsInvalid()) {
                c.jump_wait = 0.0f; // (carried after the step, below)
            } else if (!c.held) {
                m_impl->step_character(c, static_cast<float>(k_step_seconds));
            } else {
                // A jump pressed while held still lasts only its fifth of a second.
                c.jump_wait = std::max(0.0f, c.jump_wait - static_cast<float>(k_step_seconds));
            }
        }
        const JPH::EPhysicsUpdateError error =
            m_impl->system.Update(static_cast<float>(k_step_seconds), 1, &m_impl->temp, &m_impl->jobs);
        m_impl->jobs.drain();
        for (auto& [id, c] : m_impl->characters) {
            if (!c.carrier.IsInvalid()) m_impl->carry(c);
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
    return s;
}

BodyId PhysicsWorld::character_body(CharacterId id) const {
    const auto it = m_impl->characters.find(id);
    return it == m_impl->characters.end() ? k_no_body : to_id(it->second.body->GetInnerBodyID());
}

usize PhysicsWorld::characters() const { return m_impl->characters.size(); }

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
            return !f->accept || f->accept(to_id(body.GetID()), body.GetUserData());
        }
    } body_filter;
    body_filter.f = &filter;
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, body_filter)) return std::nullopt;
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
