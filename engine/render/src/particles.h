// Particles (effects plan FX1a): born on the CPU from FrameInput's bursts,
// each written once into a ring on the GPU, and never simulated after.
// Where a particle is at any age is a closed form of how it was born
// (particle_position), which the shaders evaluate the same way: the same
// age is always the same place, with nothing kept from frame to frame to
// drift or flicker.
//
// Motion: it takes the speed of the air it was born in (`air`, the wind
// times its style's `wind`) at `drag` per second, while gravity and
// buoyancy (`lift`) pull on it:
//   v(t) = air + (v0 - air) e^-kt + lift E1(t)
//   x(t) = x0 + air t + (v0 - air) E1(t) + lift E2(t)
// with E1 = (1 - e^-kt) / k and E2 = (t - E1) / k (t and t^2/2 with no
// drag: plain ballistics). Its height has one top or bottom at most (the
// acceleration's sign never changes), so the age it reaches the ground it
// was given is found by Newton's method within a bracket on one side of
// that, once, at birth.
//
// The ring keeps particles in birth order. Each frame's are one span that
// ends when its longest-lived one dies; spans end from the oldest, so the
// alive range is known without reading anything back (some in it may have
// died: the shader skips those by age). Full, the oldest make way.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/core/wind.h"
#include "giant/render/renderer.h"
#include "gpu_layout.h"

#include <span>
#include <vector>

namespace giant::render {

inline constexpr float k_particle_gravity = 9.81f;     // m/s^2
inline constexpr double k_particle_reach = 8192.0;     // metres: bursts farther from the camera are left out
inline constexpr double k_particle_reanchor = 4096.0;  // metres the camera may go from the anchor (then all go)
inline constexpr float k_particle_max_drag = 100.0f;   // 1/s
inline constexpr float k_particle_max_speed = 1000.0f; // m/s, at birth and of the air
inline constexpr float k_particle_max_gravity = 100.0f;  // a style's gravity and wind shares (buoyancy: that many g)
inline constexpr float k_particle_max_size = 1000.0f;    // metres: a style's sizes, wobble and softness
inline constexpr float k_particle_max_emissive = 1.0e4f; // a style's colours and emissive

// How one particle moves (relative to wherever its x0 is measured from).
struct ParticleMotion {
    Vec3 x0{};
    Vec3 v0{};
    Vec3 air{};
    float lift = -k_particle_gravity;
    float drag = 0.0f;
    float stop = 1.0e30f; // age it stops at
};

// At `age` seconds (held at `stop` after it).
Vec3 particle_position(const ParticleMotion& m, float age);
// Zero once stopped.
Vec3 particle_velocity(const ParticleMotion& m, float age);
// The first age in [0, life] its height (x0.y + the motion) is at or below
// `ground` (a height in the same frame as x0), or a number above `life`
// when it never is. Born below it: 0.
float particle_ground_stop(const ParticleMotion& m, float ground, float life);

// The style as the shaders read it; `usable` false (a bad style): it
// draws nothing.
gpu::ParticleStyle to_gpu(const ParticleStyle& style, bool usable);
// Whether a style's numbers are ones particles can be born with.
bool particle_style_ok(const ParticleStyle& style);
// Whether a burst's numbers are (its style aside).
bool particle_burst_ok(const ParticleBurst& burst);

class ParticleRing {
public:
    // What one frame's update did.
    struct Frame {
        u64 now_us = 0;     // the clock (microseconds since the first update)
        DVec3 anchor{};     // ring positions are relative to it
        u32 first = 0;      // ring index of the oldest alive
        u32 count = 0;      // alive range, from `first` (wrapping past the end)
        u32 born_first = 0; // ring index the born ones go to (born_records in order, wrapping)
        u32 born = 0;
        u32 dropped = 0;    // bursts' particles left out: past the caps, bad numbers or styles, too far
        u32 cut = 0;        // alive ones that made way (the ring full, or the camera far from the anchor)
    };

    ParticleRing();

    // Steps the clock by `delta_seconds` (at most 1; bad or negative: 0),
    // ends spans that died, and gives birth to the bursts' particles.
    const Frame& update(std::span<const ParticleBurst> bursts, std::span<const ParticleStyle> styles,
                        double delta_seconds, const DVec3& camera, const WindState& wind);
    const Frame& frame() const { return m_frame; }
    // This update's births, to go to the ring at frame().born_first.
    std::span<const gpu::Particle> born_records() const { return m_born; }
    // Takes back this update's births (they could not be sent to the GPU):
    // counted as dropped. (Those cut to make room for them stay cut.)
    void forget_births();
    void clear();

    // Alive particles, as the CPU sees them (ring order from the oldest).
    u32 alive() const { return static_cast<u32>(m_head - m_tail); }
    usize spans() const { return m_span_count; }
    // A record's motion relative to the anchor, and its age now.
    static ParticleMotion motion(const gpu::Particle& p);
    float age(const gpu::Particle& p) const;

private:
    struct Span {
        u64 end = 0;    // one past its last particle (a ring count, not wrapped)
        u64 expiry = 0; // microseconds: its last one dies
    };
    void cut_to(u64 tail);
    void push_span(const Span& span);
    void pop_span();
    const Span& front() const { return m_spans[m_span_first]; }

    Frame m_frame;
    u64 m_now_us = 0;
    DVec3 m_anchor{};
    bool m_anchored = false;
    u64 m_head = 0; // next particle's count (ring index: % k_max_particles)
    u64 m_tail = 0; // the oldest alive
    std::vector<Span> m_spans; // a ring of them (k_max_particles, made at once), oldest at m_span_first
    usize m_span_first = 0;
    usize m_span_count = 0;
    std::vector<gpu::Particle> m_born;
    std::vector<u8> m_style_ok;
};

} // namespace giant::render
