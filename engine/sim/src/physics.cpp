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
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
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
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
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
    }
    ~Impl() {
        // Bodies go before the system that owns them (then Jolt's types, last).
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
    JPH::PhysicsSystem system;
    double carried = 0.0; // time not yet stepped
    u64 steps = 0;
    u32 dropped = 0;
    double last_step_ms = 0.0;
    u32 bodies = 0;

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

PhysicsWorld::~PhysicsWorld() {
    m_impl.reset();
    if (m_disposal.joinable()) m_disposal.join();
}

void PhysicsWorld::clear() {
    auto fresh = std::make_unique<Impl>(m_impl->jobs.jobs(), m_impl->settings);
    fresh->steps = m_impl->steps;
    fresh->dropped = m_impl->dropped;
    std::swap(m_impl, fresh);
    m_moved.clear();
    // Letting tens of thousands of bodies go takes tens of milliseconds: on
    // a thread of its own (one at a time), not in a frame.
    if (m_disposal.joinable()) m_disposal.join();
    m_disposal = std::thread([old = std::move(fresh)]() mutable { old.reset(); });
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
        if (!b.IsInvalid() && bi.IsAdded(b)) gone.push_back(b);
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
    if (!contains(id) || !finite(position) || !finite(rotation)) return;
    m_impl->wake_around(from_id(id)); // a fixed body: what rested on it where it was
    m_impl->bodies_interface().SetPositionAndRotation(from_id(id), to_jolt(position), to_jolt(rotation),
                                                      JPH::EActivation::Activate);
    m_impl->wake_around(from_id(id)); // and what it now touches
}

void PhysicsWorld::set_velocity(BodyId id, const Vec3& linear, const Vec3& angular) {
    if (!contains(id) || !finite(linear) || !finite(angular)) return;
    m_impl->bodies_interface().SetLinearAndAngularVelocity(from_id(id), to_jolt(linear), to_jolt(angular));
}

void PhysicsWorld::move_kinematic(BodyId id, const DVec3& position, const Quat& rotation) {
    if (!contains(id) || !finite(position) || !finite(rotation)) return;
    m_impl->bodies_interface().MoveKinematic(from_id(id), to_jolt(position), to_jolt(rotation),
                                             static_cast<float>(k_step_seconds));
}

u32 PhysicsWorld::step(double seconds) {
    const auto start = std::chrono::steady_clock::now();
    m_moved.clear();
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
        const JPH::EPhysicsUpdateError error =
            m_impl->system.Update(static_cast<float>(k_step_seconds), 1, &m_impl->temp, &m_impl->jobs);
        m_impl->jobs.drain();
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
    std::sort(m_moved.begin(), m_moved.end()); // the same order every run
    m_moved.erase(std::unique(m_moved.begin(), m_moved.end()), m_moved.end());
    m_impl->last_step_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return run;
}

std::optional<RayHit> PhysicsWorld::raycast(const DVec3& origin, const Vec3& direction, double max_distance) const {
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!finite(origin) || !finite(direction) || length < 1e-9f || !std::isfinite(max_distance) ||
        max_distance <= 0.0) {
        return std::nullopt;
    }
    const float reach = static_cast<float>(max_distance) / length;
    const JPH::RRayCast ray(to_jolt(origin), to_jolt(direction) * reach);
    JPH::RayCastResult result;
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, result)) return std::nullopt;
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
    s.active = m_impl->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    s.steps = m_impl->steps;
    s.dropped = m_impl->dropped;
    s.last_step_ms = m_impl->last_step_ms;
    return s;
}

} // namespace giant::sim
