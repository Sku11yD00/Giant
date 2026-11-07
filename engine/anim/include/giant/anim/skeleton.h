// Skeletons and animation clips as plain data, and what is worked out from
// them each frame: a clip sampled at a time into joint transforms, two
// poses blended, the joints' matrices in the model's space, and the
// skinning matrices that move a vertex from where the mesh was bound to
// where the pose has it.
//
// Spaces. A joint's `local` transform is relative to its parent joint
// (`root` for a root joint). Model space is the mesh's file space: the
// skeleton's joints and `root` placed as the file has them. A skinned
// vertex is stored as the mesh had it when it was bound (bind space), with
// up to four joints and weights; skin matrix j = model(j) * inverse_bind(j)
// moves it into model space, and a vertex's place is the weighted sum.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace giant::anim {

inline constexpr usize k_max_joints = 1024;          // joint indices fit u16
inline constexpr float k_max_clip_seconds = 3600.0f; // an hour
inline constexpr usize k_max_keys = 8'000'000;       // over all of a file's clips

struct JointTransform {
    Vec3 translation{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    bool operator==(const JointTransform&) const = default;
};

struct Skeleton {
    std::vector<std::string> names;
    std::vector<i32> parents;          // -1 for a root; always before its children
    std::vector<JointTransform> rest;  // the pose with no clip playing
    std::vector<Mat4> inverse_bind;    // model space to the joint's space, when bound
    Mat4 root{};                       // where the root joints' parent is, in model space
    usize size() const { return parents.size(); }
};

// Empty when sound: as many names, rest transforms and inverse binds as
// joints, at most k_max_joints, parents before children, finite numbers.
std::string validate(const Skeleton& skeleton);

enum class Path : u8 { translation, rotation, scale };
enum class Interpolation : u8 { linear, step, cubic };

// One joint's translation, rotation or scale over time. Values are xyz (w
// unused) or a rotation's xyzw; a cubic channel holds three per key: the
// tangent in, the value, the tangent out (glTF's CUBICSPLINE).
struct Channel {
    u32 joint = 0;
    Path path = Path::translation;
    Interpolation interpolation = Interpolation::linear;
    std::vector<float> times; // seconds, strictly increasing
    std::vector<Vec4> values;
};

struct Clip {
    std::string name;
    float duration = 0.0f; // the last key's time
    std::vector<Channel> channels;
};

// Empty when sound for `skeleton`: joints in range, times finite and
// increasing within [0, k_max_clip_seconds], one value (three when cubic)
// per key, finite values.
std::string validate(const Clip& clip, const Skeleton& skeleton);

// The clip's pose at `time` (seconds, clamped to the clip; each channel
// holds its first and last keys before and after them): the rest pose,
// with what the clip animates replaced. `local` is one per joint.
void sample(const Skeleton& skeleton, const Clip& clip, float time, std::span<JointTransform> local);

// `a` toward `b` by `t` (0 is a, 1 is b): translations and scales lerped,
// rotations along the shorter way (normalised lerp).
void blend(std::span<const JointTransform> a, std::span<const JointTransform> b, float t,
           std::span<JointTransform> out);

// Each joint's matrix in model space (parents first).
void model_matrices(const Skeleton& skeleton, std::span<const JointTransform> local, std::span<Mat4> model);
// model * inverse_bind, per joint.
void skin_matrices(const Skeleton& skeleton, std::span<const Mat4> model, std::span<Mat4> skin);

// A bind-space point skinned: the weighted sum of its joints' skin matrices
// applied to it (weights in 255ths, summing to 255). The renderer does the
// same on the GPU; this is its reference.
Vec3 skin_point(std::span<const Mat4> skin, const std::array<u16, 4>& joints, const std::array<u8, 4>& weights,
                const Vec3& bind);
// A direction (a normal) the same way, unit length.
Vec3 skin_direction(std::span<const Mat4> skin, const std::array<u16, 4>& joints,
                    const std::array<u8, 4>& weights, const Vec3& bind);

struct Bounds {
    Vec3 center{};
    float radius = 0.0f; // 0 and no joints: nothing
};

// A sphere holding every skinned vertex in this pose. `joint_radius` (per
// joint, from the import) is how far a vertex the joint moves lies from it
// when bound (0: it moves none); a joint's vertices then lie within that
// times its skin matrix's largest scale of the joint.
Bounds pose_bounds(std::span<const Mat4> model, std::span<const Mat4> skin, std::span<const float> joint_radius);

// The inverse of an affine matrix (its last row 0 0 0 1); the identity when
// it has none (a zero scale).
Mat4 affine_inverse(const Mat4& m);
// A matrix with no shear taken apart into translation, rotation and scale
// (a mirror goes into a negative x scale).
JointTransform decompose(const Mat4& m);
Mat4 to_matrix(const JointTransform& t);

} // namespace giant::anim
