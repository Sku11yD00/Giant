#include "giant/anim/motion.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>

namespace giant::anim {

namespace {

constexpr usize k_small = 16;
constexpr usize k_large = 64;
constexpr float k_far = 1e18f; // an empty box lies this far off (its square still a float)
constexpr float k_least_spread = 0.05f; // a group that hardly varies is not blown up past this
constexpr usize k_any_morphs = ~usize{0}; // a clip's morph channels are not ours to judge
constexpr float k_reach = 1e5f;         // no measured number is past this (metres, metres a second)

constexpr float k_two_pi = 2.0f * k_pi;

float wrap(float radians) {
    radians = std::fmod(radians, k_two_pi);
    if (radians > k_pi) radians -= k_two_pi;
    if (radians < -k_pi) radians += k_two_pi;
    return radians;
}

Vec3 place_of(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }

// A frame as sampled, in model space.
struct Sampled {
    MotionRoot root;
    Vec3 hips{};
    Vec3 left{};
    Vec3 right{};
};

// (x, z) turned about +y by `radians` (+z toward +x).
void turn(float radians, float& x, float& z) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float tx = x * c + z * s;
    z = -x * s + z * c;
    x = tx;
}

// What one round of a loop does: turns by `yaw` about `from` and lands it on `to`.
struct Round {
    float yaw = 0.0f;
    float from_x = 0.0f, from_z = 0.0f;
    float to_x = 0.0f, to_z = 0.0f;

    void on(float& x, float& z) const {
        x -= from_x;
        z -= from_z;
        turn(yaw, x, z);
        x += to_x;
        z += to_z;
    }
    void back(float& x, float& z) const {
        x -= to_x;
        z -= to_z;
        turn(-yaw, x, z);
        x += from_x;
        z += from_z;
    }
    void on(Sampled& s) const {
        on(s.root.x, s.root.z);
        s.root.yaw += yaw;
        on(s.hips.x, s.hips.z);
        on(s.left.x, s.left.z);
        on(s.right.x, s.right.z);
    }
    void back(Sampled& s) const {
        back(s.root.x, s.root.z);
        s.root.yaw -= yaw;
        back(s.hips.x, s.hips.z);
        back(s.left.x, s.left.z);
        back(s.right.x, s.right.z);
    }
};

// A clip's frames, and any frame before or after them: a loop's rounds
// moved on, a one-off clip's ends held.
struct Frames {
    std::span<const Sampled> frames;
    bool loop = false;
    Round round;

    Sampled at(i64 i) const {
        const i64 n = static_cast<i64>(frames.size());
        if (!loop) return frames[static_cast<usize>(std::clamp<i64>(i, 0, n - 1))];
        i64 rounds = i / n;
        i64 j = i % n;
        if (j < 0) {
            j += n;
            --rounds;
        }
        Sampled s = frames[static_cast<usize>(j)];
        for (; rounds > 0; --rounds) round.on(s);
        for (; rounds < 0; ++rounds) round.back(s);
        return s;
    }
};

// Reads the pose's root and feet from its model matrices. `rest_forward`
// is the model's forward in the hips' own space; `yaw_before` is what the
// yaw runs on from (and stays, with the hips' forward straight up or down).
Sampled read(std::span<const Mat4> model, const MotionJoints& joints, const Vec3& rest_forward, float yaw_before) {
    Sampled s;
    s.hips = place_of(model[joints.hips]);
    s.left = place_of(model[joints.left_foot]);
    s.right = place_of(model[joints.right_foot]);
    const Vec4 f = model[joints.hips] * Vec4{rest_forward.x, rest_forward.y, rest_forward.z, 0.0f};
    s.root.x = s.hips.x;
    s.root.z = s.hips.z;
    s.root.yaw = yaw_before;
    if (f.x * f.x + f.z * f.z > 1e-8f) s.root.yaw += wrap(std::atan2(f.x, f.z) - yaw_before);
    return s;
}

// The model's forward as the hips carry it: in the hips' space at rest.
Vec3 forward_in_hips(const Skeleton& skeleton, u32 hips, const Vec3& forward, std::vector<Mat4>& model) {
    model_matrices(skeleton, skeleton.rest, model);
    const Vec4 f = affine_inverse(model[hips]) * Vec4{forward.x, forward.y, forward.z, 0.0f};
    return {f.x, f.y, f.z};
}

bool joints_in(const MotionJoints& joints, usize count) {
    return joints.hips < count && joints.left_foot < count && joints.right_foot < count;
}

std::string settings_problem(const MotionSettings& s) {
    if (!(s.frame_rate >= 10.0f && s.frame_rate <= 120.0f)) return "the frame rate must be 10 to 120";
    float before = 0.0f;
    for (const float h : s.horizons) {
        if (!(h > before && h <= 2.0f)) return "the looks ahead must rise from above 0 to at most 2 seconds";
        before = h;
    }
    if (!(s.smoothing_seconds >= 0.0f && s.smoothing_seconds <= 1.0f)) return "the smoothing must be 0 to 1 second";
    const float level = std::hypot(s.forward.x, s.forward.z);
    if (!std::isfinite(s.forward.y) || !(level > 1e-4f && level <= 1e6f)) {
        return "forward must be a direction with a level part";
    }
    for (const float w : {s.foot_place_weight, s.foot_velocity_weight, s.hips_velocity_weight, s.ahead_place_weight,
                          s.ahead_facing_weight}) {
        if (!(w >= 0.0f && w <= 1000.0f)) return "a weight must be 0 to 1000";
    }
    return {};
}

// A name as rigs are compared: past any prefix, letters and digits only, lower case.
std::string plain(const std::string& name) {
    const usize cut = name.find_last_of(":|");
    std::string out;
    for (usize i = cut == std::string::npos ? 0 : cut + 1; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isalnum(c) != 0) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

struct Group {
    usize first;
    usize count;
    float MotionSettings::* weight;
};
constexpr Group k_groups[] = {
    {k_motion_foot_places, 6, &MotionSettings::foot_place_weight},
    {k_motion_foot_velocities, 6, &MotionSettings::foot_velocity_weight},
    {k_motion_hips_velocity, 3, &MotionSettings::hips_velocity_weight},
    {k_motion_ahead_places, 6, &MotionSettings::ahead_place_weight},
    {k_motion_ahead_facings, 6, &MotionSettings::ahead_facing_weight},
};

// How far `query` is from a box, squared.
float box_cost(const float* least, const float* most, const MotionFeatures& query, usize from) {
    float cost = 0.0f;
    for (usize i = from; i < k_motion_features; ++i) {
        const float q = query[i];
        const float d = q < least[i] ? least[i] - q : (q > most[i] ? q - most[i] : 0.0f);
        cost += d * d;
    }
    return cost;
}

} // namespace

MotionJoints find_motion_joints(const Skeleton& skeleton) {
    MotionJoints joints;
    const auto is_any = [](const std::string& name, std::initializer_list<const char*> of) {
        return std::any_of(of.begin(), of.end(), [&](const char* n) { return name == n; });
    };
    for (usize i = 0; i < skeleton.names.size() && i < skeleton.size(); ++i) {
        const std::string name = plain(skeleton.names[i]);
        const u32 j = static_cast<u32>(i);
        if (joints.hips == k_no_joint && is_any(name, {"hips", "pelvis", "hip"})) joints.hips = j;
        if (joints.left_foot == k_no_joint && is_any(name, {"leftfoot", "footl", "lfoot", "footleft"})) {
            joints.left_foot = j;
        }
        if (joints.right_foot == k_no_joint && is_any(name, {"rightfoot", "footr", "rfoot", "footright"})) {
            joints.right_foot = j;
        }
    }
    return joints;
}

bool clip_loops(const Skeleton& skeleton, const Clip& clip, const MotionJoints& joints, float tolerance) {
    const usize n = skeleton.size();
    if (!joints_in(joints, n) || !validate(skeleton).empty() || !validate(clip, skeleton, k_any_morphs).empty()) {
        return false;
    }
    if (!(clip.duration > 0.0f)) return true;
    std::vector<JointTransform> local(n);
    std::vector<Mat4> start(n);
    std::vector<Mat4> end(n);
    const Vec3 forward = forward_in_hips(skeleton, joints.hips, MotionSettings{}.forward, start);
    sample(skeleton, clip, 0.0f, local);
    model_matrices(skeleton, local, start);
    sample(skeleton, clip, clip.duration, local);
    model_matrices(skeleton, local, end);
    const Sampled a = read(start, joints, forward, 0.0f);
    const Sampled b = read(end, joints, forward, 0.0f);
    for (usize j = 0; j < n; ++j) {
        Vec3 pa = place_of(start[j]);
        Vec3 pb = place_of(end[j]);
        pa.x -= a.root.x;
        pa.z -= a.root.z;
        pb.x -= b.root.x;
        pb.z -= b.root.z;
        turn(-a.root.yaw, pa.x, pa.z);
        turn(-b.root.yaw, pb.x, pb.z);
        if (!(length(pa - pb) <= tolerance)) return false;
    }
    return true;
}

const MotionRange& MotionDatabase::range_of(u32 f) const {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), f,
                               [](u32 frame, const MotionRange& r) { return frame < r.first; });
    return *(it == ranges.begin() ? it : it - 1);
}

float MotionDatabase::time_of(u32 f) const {
    const MotionRange& r = range_of(f);
    return static_cast<float>(f - r.first) * r.frame_seconds;
}

u32 MotionDatabase::next(u32 f) const {
    const MotionRange& r = range_of(f);
    if (f + 1 < r.first + r.count) return f + 1;
    return r.loop ? r.first : f;
}

float MotionDatabase::raw(u32 f, usize feature) const {
    const float s = scale[feature];
    return s > 0.0f ? frame(f)[feature] / s + offset[feature] : offset[feature];
}

std::string build_motion_database(const Skeleton& skeleton, std::span<const Clip> clips,
                                  std::span<const MotionClip> chosen, const MotionJoints& joints,
                                  const MotionSettings& settings, MotionDatabase& out) {
    out = {};
    const usize joint_count = skeleton.size();
    if (std::string problem = validate(skeleton); !problem.empty()) return problem;
    if (!joints.found()) return "the hips and both feet must be named";
    if (!joints_in(joints, joint_count)) return "a joint is not in the skeleton";
    if (std::string problem = settings_problem(settings); !problem.empty()) return problem;
    if (chosen.empty()) return "no clips";
    if (chosen.size() > k_max_motion_clips) return "too many clips";

    const float rate = settings.frame_rate;
    usize total = 0;
    std::vector<MotionRange> ranges;
    ranges.reserve(chosen.size());
    for (const MotionClip& c : chosen) {
        if (c.clip >= clips.size()) return "a clip is not in the file";
        if (std::string problem = validate(clips[c.clip], skeleton, k_any_morphs); !problem.empty()) return problem;
        const float duration = clips[c.clip].duration;
        if (!(duration >= 0.0f && duration <= k_max_clip_seconds)) return "a clip's length is out of range";
        MotionRange r;
        r.clip = c.clip;
        r.loop = c.loop;
        r.first = static_cast<u32>(total);
        r.frame_seconds = 1.0f / rate;
        if (c.loop) {
            // A whole number of frames to the round, as near the rate as that allows.
            // (One shorter than half a frame is a pose: one frame, at the rate.)
            r.count = std::max<u32>(1, static_cast<u32>(std::lround(duration * rate)));
            if (duration * rate >= 0.5f) r.frame_seconds = duration / static_cast<float>(r.count);
        } else {
            r.count = static_cast<u32>(std::floor(duration * rate)) + 1;
        }
        total += r.count;
        if (total > k_max_motion_frames) return "too many frames";
        ranges.push_back(r);
    }

    std::vector<JointTransform> local(joint_count);
    std::vector<Mat4> model(joint_count);
    const float level = std::hypot(settings.forward.x, settings.forward.z);
    const Vec3 forward = forward_in_hips(skeleton, joints.hips,
                                         {settings.forward.x / level, 0.0f, settings.forward.z / level}, model);

    std::vector<float> raw(total * k_motion_features);
    std::vector<MotionRoot> roots(total);
    std::vector<Sampled> sampled;
    std::vector<MotionRoot> smoothed;
    bool any_startable = false;
    for (MotionRange& r : ranges) {
        const Clip& clip = clips[r.clip];
        const float dt = r.frame_seconds;
        const usize n = r.count;
        sampled.resize(n);
        float yaw = 0.0f;
        for (usize i = 0; i < n; ++i) {
            sample(skeleton, clip, std::min(static_cast<float>(i) * dt, clip.duration), local);
            model_matrices(skeleton, local, model);
            sampled[i] = read(model, joints, forward, yaw);
            yaw = sampled[i].root.yaw;
        }
        Frames frames{sampled, r.loop, {}};
        if (r.loop && clip.duration * rate >= 0.5f) { // (a pose goes nowhere)
            sample(skeleton, clip, clip.duration, local);
            model_matrices(skeleton, local, model);
            const Sampled end = read(model, joints, forward, yaw);
            frames.round = {end.root.yaw - sampled[0].root.yaw, sampled[0].root.x, sampled[0].root.z, end.root.x,
                            end.root.z};
        }

        // The root: the hips averaged over the frames about each.
        const i64 window = static_cast<i64>(std::lround(settings.smoothing_seconds / dt));
        smoothed.resize(n);
        for (usize i = 0; i < n; ++i) {
            const i64 at = static_cast<i64>(i);
            const i64 w = r.loop ? window : std::min({window, at, static_cast<i64>(n) - 1 - at});
            double x = 0.0, z = 0.0, turned = 0.0;
            for (i64 j = at - w; j <= at + w; ++j) {
                const MotionRoot root = frames.at(j).root;
                x += root.x;
                z += root.z;
                turned += root.yaw;
            }
            const double count = static_cast<double>(2 * w + 1);
            smoothed[i] = {static_cast<float>(x / count), static_cast<float>(z / count),
                           static_cast<float>(turned / count)};
        }
        for (usize i = 0; i < n; ++i) sampled[i].root = smoothed[i];

        std::array<i64, k_motion_horizons> ahead{};
        for (usize h = 0; h < k_motion_horizons; ++h) {
            ahead[h] = std::max<i64>(1, std::lround(settings.horizons[h] / dt));
        }
        const u32 longest = static_cast<u32>(ahead[k_motion_horizons - 1]);
        r.startable = r.loop ? r.count : (r.count > longest ? r.count - longest : 0);
        any_startable = any_startable || r.startable > 0;

        for (usize i = 0; i < n; ++i) {
            const i64 at = static_cast<i64>(i);
            const Sampled now = sampled[i];
            float* f = raw.data() + (r.first + i) * k_motion_features;
            roots[r.first + i] = now.root;
            const auto relative = [&](Vec3 v, float* to) {
                turn(-now.root.yaw, v.x, v.z);
                to[0] = v.x;
                to[1] = v.y;
                to[2] = v.z;
            };
            relative({now.left.x - now.root.x, now.left.y, now.left.z - now.root.z}, f + k_motion_foot_places);
            relative({now.right.x - now.root.x, now.right.y, now.right.z - now.root.z}, f + k_motion_foot_places + 3);

            // Velocities over the frames either side (one side at a one-off clip's ends).
            const i64 before = r.loop ? at - 1 : std::max<i64>(at - 1, 0);
            const i64 after = r.loop ? at + 1 : std::min<i64>(at + 1, static_cast<i64>(n) - 1);
            const Sampled a = frames.at(before);
            const Sampled b = frames.at(after);
            const float per = after > before ? 1.0f / (static_cast<float>(after - before) * dt) : 0.0f;
            relative((b.left - a.left) * per, f + k_motion_foot_velocities);
            relative((b.right - a.right) * per, f + k_motion_foot_velocities + 3);
            relative((b.hips - a.hips) * per, f + k_motion_hips_velocity);

            for (usize h = 0; h < k_motion_horizons; ++h) {
                const MotionRoot then = frames.at(at + ahead[h]).root;
                float x = then.x - now.root.x;
                float z = then.z - now.root.z;
                turn(-now.root.yaw, x, z);
                f[k_motion_ahead_places + 2 * h] = x;
                f[k_motion_ahead_places + 2 * h + 1] = z;
                const float turned = then.yaw - now.root.yaw;
                f[k_motion_ahead_facings + 2 * h] = std::sin(turned);
                f[k_motion_ahead_facings + 2 * h + 1] = std::cos(turned);
            }
        }
    }
    if (!any_startable) return "no frame to start from: every clip is shorter than the longest look ahead";
    // (Within reach, every normalised number and every cost stays a float.)
    if (!std::all_of(raw.begin(), raw.end(), [](float v) { return std::fabs(v) <= k_reach; })) {
        return "a clip's pose is out of reach: its feet or its way ahead lie over 100 km off";
    }

    // Each number about its mean; each group by how much it spreads, times its weight.
    MotionFeatures offset{};
    MotionFeatures scale{};
    std::array<double, k_motion_features> mean{};
    std::array<double, k_motion_features> spread{};
    for (usize f = 0; f < total; ++f) {
        for (usize i = 0; i < k_motion_features; ++i) mean[i] += static_cast<double>(raw[f * k_motion_features + i]);
    }
    for (double& m : mean) m /= static_cast<double>(total);
    for (usize f = 0; f < total; ++f) {
        for (usize i = 0; i < k_motion_features; ++i) {
            const double d = static_cast<double>(raw[f * k_motion_features + i]) - mean[i];
            spread[i] += d * d;
        }
    }
    for (const Group& g : k_groups) {
        double sum = 0.0;
        for (usize i = g.first; i < g.first + g.count; ++i) sum += spread[i] / static_cast<double>(total);
        const float deviation = static_cast<float>(std::sqrt(sum / static_cast<double>(g.count)));
        for (usize i = g.first; i < g.first + g.count; ++i) {
            offset[i] = static_cast<float>(mean[i]);
            scale[i] = settings.*g.weight / std::max(deviation, k_least_spread);
        }
    }
    for (usize f = 0; f < total; ++f) {
        for (usize i = 0; i < k_motion_features; ++i) {
            float& v = raw[f * k_motion_features + i];
            v = (v - offset[i]) * scale[i];
        }
    }

    const auto boxes = [&](usize size, std::vector<float>& least, std::vector<float>& most) {
        const usize count = (total + size - 1) / size;
        least.assign(count * k_motion_features, k_far);
        most.assign(count * k_motion_features, -k_far);
        for (const MotionRange& r : out.ranges) {
            for (usize f = r.first; f < usize{r.first} + r.startable; ++f) {
                float* lo = least.data() + (f / size) * k_motion_features;
                float* hi = most.data() + (f / size) * k_motion_features;
                for (usize i = 0; i < k_motion_features; ++i) {
                    const float v = raw[f * k_motion_features + i];
                    lo[i] = std::min(lo[i], v);
                    hi[i] = std::max(hi[i], v);
                }
            }
        }
        for (usize i = 0; i < most.size(); ++i) {
            if (least[i] > most[i]) most[i] = k_far; // empty: far from everything
        }
    };
    out.settings = settings;
    out.joints = joints;
    out.ranges = std::move(ranges);
    out.roots = std::move(roots);
    out.offset = offset;
    out.scale = scale;
    boxes(k_small, out.small_least, out.small_most);
    boxes(k_large, out.large_least, out.large_most);
    out.features = std::move(raw);
    return {};
}

MotionFeatures motion_query(const MotionDatabase& db, u32 now, const MotionAhead& ahead) {
    MotionFeatures q{};
    if (now < db.frames()) {
        const auto pose = db.frame(now);
        std::copy(pose.begin(), pose.begin() + k_motion_ahead_places, q.begin());
    }
    for (usize h = 0; h < k_motion_horizons; ++h) {
        for (usize i = 0; i < 2; ++i) {
            const usize p = k_motion_ahead_places + 2 * h + i;
            const usize f = k_motion_ahead_facings + 2 * h + i;
            // (Kept to what a search can measure: no number, none; a place within a kilometre.)
            const float wanted = ahead.place[h][i];
            const float faced = ahead.facing[h][i];
            const float place = std::isfinite(wanted) ? std::clamp(wanted, -1000.0f, 1000.0f) : 0.0f;
            const float facing = std::isfinite(faced) ? std::clamp(faced, -1.0f, 1.0f) : 0.0f;
            q[p] = (place - db.offset[p]) * db.scale[p];
            q[f] = (facing - db.offset[f]) * db.scale[f];
        }
    }
    return q;
}

float motion_cost(const MotionDatabase& db, u32 f, const MotionFeatures& query, bool pose) {
    const auto frame = db.frame(f);
    float cost = 0.0f;
    for (usize i = pose ? 0 : k_motion_ahead_places; i < k_motion_features; ++i) {
        const float d = frame[i] - query[i];
        cost += d * d;
    }
    return cost;
}

MotionMatch motion_search(const MotionDatabase& db, const MotionFeatures& query, MotionMatch best, bool pose) {
    const float* features = db.features.data();
    const usize from = pose ? 0 : k_motion_ahead_places;
    for (const MotionRange& r : db.ranges) {
        usize i = r.first;
        const usize end = usize{r.first} + r.startable;
        while (i < end) {
            const usize big = i / k_large;
            const usize large_end = std::min((big + 1) * k_large, end);
            if (box_cost(db.large_least.data() + big * k_motion_features,
                         db.large_most.data() + big * k_motion_features, query, from) >= best.cost) {
                i = large_end;
                continue;
            }
            while (i < large_end) {
                const usize little = i / k_small;
                const usize small_end = std::min((little + 1) * k_small, large_end);
                if (box_cost(db.small_least.data() + little * k_motion_features,
                             db.small_most.data() + little * k_motion_features, query, from) >= best.cost) {
                    i = small_end;
                    continue;
                }
                for (; i < small_end; ++i) {
                    const float* frame = features + i * k_motion_features;
                    float cost = 0.0f;
                    for (usize k = from; k < k_motion_features && cost < best.cost; ++k) {
                        const float d = frame[k] - query[k];
                        cost += d * d;
                    }
                    if (cost < best.cost) best = {static_cast<u32>(i), cost};
                }
            }
        }
    }
    return best;
}

} // namespace giant::anim
