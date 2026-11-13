#include "giant/world/effects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace giant::world {

namespace {

std::string range_text(float minimum, float maximum) {
    char text[64];
    std::snprintf(text, sizeof(text), "from %g to %g", static_cast<double>(minimum), static_cast<double>(maximum));
    return text;
}

// Empty when `v` is finite and within [minimum, maximum].
std::string number(const char* what, float v, float minimum, float maximum) {
    if (!std::isfinite(v) || v < minimum || v > maximum) return std::string(what) + " must be " + range_text(minimum, maximum);
    return {};
}

std::string vec(const char* what, const Vec3& v, float minimum, float maximum) {
    for (const float c : {v.x, v.y, v.z}) {
        if (!std::isfinite(c) || c < minimum || c > maximum) {
            return std::string(what) + " values must be " + range_text(minimum, maximum);
        }
    }
    return {};
}

std::optional<std::string> first(std::initializer_list<std::string> problems) {
    for (const std::string& p : problems) {
        if (!p.empty()) return p;
    }
    return std::nullopt;
}

ParticleLook look(const char* name) {
    ParticleLook l;
    l.name = name;
    return l;
}

std::vector<ParticleLook> make_looks() {
    std::vector<ParticleLook> out;
    {
        ParticleLook l = look("smoke"); // grey, rising and spreading, carried off by the wind
        l.life_min = 6.0f;
        l.life_max = 10.0f;
        l.size_start = 0.6f;
        l.size_end = 4.0f;
        l.color_start = {0.32f, 0.31f, 0.30f};
        l.color_end = {0.45f, 0.45f, 0.45f};
        l.opacity_start = 0.55f;
        l.opacity_end = 0.0f;
        l.gravity = 0.0f;
        l.buoyancy = 0.6f;
        l.drag = 0.8f;
        l.wobble = 0.6f;
        l.soft = 1.0f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("fire"); // flames: bright, short-lived, shrinking as they rise
        l.life_min = 0.5f;
        l.life_max = 1.0f;
        l.size_start = 0.5f;
        l.size_end = 0.15f;
        l.color_start = {0.1f, 0.06f, 0.03f};
        l.color_end = {0.05f, 0.05f, 0.05f};
        l.opacity_start = 0.9f;
        l.opacity_end = 0.0f;
        l.emissive = {8.0f, 3.0f, 0.6f};
        l.emissive_end = 0.1f;
        l.gravity = 0.0f;
        l.buoyancy = 3.0f;
        l.drag = 2.0f;
        l.wind = 0.5f;
        l.wobble = 0.1f;
        l.lit = false;
        l.soft = 0.3f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("sparks"); // hot metal: thin glowing streaks that fall and die out
        l.life_min = 0.4f;
        l.life_max = 1.0f;
        l.size_start = 0.03f;
        l.size_end = 0.01f;
        l.color_start = {1.0f, 0.6f, 0.2f};
        l.color_end = {0.3f, 0.1f, 0.05f};
        l.opacity_start = 1.0f;
        l.opacity_end = 0.0f; // dies out with its glow (unlit: no glow left would be a black streak)
        l.emissive = {20.0f, 8.0f, 2.0f};
        l.emissive_end = 0.1f;
        l.drag = 0.5f;
        l.wind = 0.2f;
        l.form = ParticleForm::streak;
        l.lit = false;
        l.soft = 0.05f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("dust_kick"); // dry earth kicked up: tan, spreading, settling slowly
        l.life_min = 1.5f;
        l.life_max = 3.0f;
        l.size_start = 0.3f;
        l.size_end = 1.5f;
        l.color_start = {0.45f, 0.40f, 0.33f};
        l.color_end = {0.50f, 0.46f, 0.40f};
        l.opacity_start = 0.5f;
        l.opacity_end = 0.0f;
        l.gravity = 0.05f;
        l.drag = 2.0f;
        l.wobble = 0.2f;
        l.soft = 0.5f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("spray"); // water thrown up: pale drops that fall back
        l.life_min = 0.6f;
        l.life_max = 1.2f;
        l.size_start = 0.05f;
        l.size_end = 0.25f;
        l.color_start = {0.8f, 0.85f, 0.9f};
        l.color_end = {0.85f, 0.88f, 0.9f};
        l.opacity_start = 0.7f;
        l.opacity_end = 0.0f;
        l.drag = 1.0f;
        l.wind = 0.6f;
        l.soft = 0.2f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("mist"); // fine water hanging in the air (a waterfall's foot)
        l.life_min = 4.0f;
        l.life_max = 8.0f;
        l.size_start = 1.0f;
        l.size_end = 5.0f;
        l.color_start = {0.85f, 0.87f, 0.9f};
        l.color_end = {0.85f, 0.87f, 0.9f};
        l.opacity_start = 0.25f;
        l.opacity_end = 0.0f;
        l.gravity = 0.0f;
        l.buoyancy = 0.1f;
        l.drag = 1.0f;
        l.wobble = 0.8f;
        l.soft = 1.5f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("embers"); // glowing flecks drifting up from a fire
        l.life_min = 2.0f;
        l.life_max = 4.0f;
        l.size_start = 0.04f;
        l.size_end = 0.02f;
        l.color_start = {0.1f, 0.05f, 0.02f};
        l.color_end = {0.05f, 0.05f, 0.05f};
        l.opacity_start = 1.0f;
        l.opacity_end = 0.0f;
        l.emissive = {6.0f, 1.8f, 0.3f};
        l.emissive_end = 0.0f;
        l.gravity = 0.0f;
        l.buoyancy = 0.8f;
        l.drag = 1.5f;
        l.wobble = 0.4f;
        l.lit = false;
        l.soft = 0.05f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("debris_chips"); // bits of stone or wood knocked off: thrown and falling
        l.life_min = 1.0f;
        l.life_max = 2.0f;
        l.size_start = 0.06f;
        l.size_end = 0.06f;
        l.color_start = {0.35f, 0.32f, 0.28f};
        l.color_end = {0.35f, 0.32f, 0.28f};
        l.opacity_start = 1.0f;
        l.opacity_end = 0.0f; // those lying on the ground fade, not vanish
        l.drag = 0.2f;
        l.wind = 0.1f;
        l.form = ParticleForm::streak;
        l.soft = 0.02f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("splinters"); // wood torn off by a shot: pale brown slivers thrown and falling
        l.life_min = 1.0f;
        l.life_max = 2.0f;
        l.size_start = 0.05f;
        l.size_end = 0.05f;
        l.color_start = {0.45f, 0.33f, 0.2f};
        l.color_end = {0.45f, 0.33f, 0.2f};
        l.opacity_start = 1.0f;
        l.opacity_end = 0.0f;
        l.drag = 0.3f;
        l.wind = 0.1f;
        l.form = ParticleForm::streak;
        l.soft = 0.02f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("leaves"); // shaken off a falling tree: fluttering down, carried by the wind
        l.life_min = 4.0f;
        l.life_max = 7.0f;
        l.size_start = 0.07f;
        l.size_end = 0.07f;
        l.color_start = {0.16f, 0.24f, 0.07f};
        l.color_end = {0.3f, 0.25f, 0.1f};
        l.opacity_start = 1.0f;
        l.opacity_end = 0.0f;
        l.gravity = 0.15f;
        l.drag = 2.0f;
        l.wobble = 0.5f;
        l.soft = 0.02f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("rain_splash"); // a raindrop's crown off the ground: tiny drops hopping up
        l.life_min = 0.15f;
        l.life_max = 0.3f;
        l.size_start = 0.015f;
        l.size_end = 0.01f;
        l.color_start = {0.75f, 0.8f, 0.85f};
        l.color_end = {0.75f, 0.8f, 0.85f};
        l.opacity_start = 0.6f;
        l.opacity_end = 0.0f;
        l.form = ParticleForm::streak;
        l.wind = 0.2f;
        l.soft = 0.01f;
        out.push_back(l);
    }
    {
        ParticleLook l = look("grit"); // dust and grit blown along the ground in a storm
        l.life_min = 1.5f;
        l.life_max = 3.0f;
        l.size_start = 0.15f;
        l.size_end = 0.8f;
        l.color_start = {0.5f, 0.45f, 0.36f};
        l.color_end = {0.55f, 0.5f, 0.42f};
        l.opacity_start = 0.35f;
        l.opacity_end = 0.0f;
        l.gravity = 0.05f;
        l.drag = 3.0f;
        l.wobble = 0.3f;
        l.soft = 0.3f;
        out.push_back(l);
    }
    return out;
}

Emitter emitter(const char* look_name, float rate, u32 burst, float cone, float speed_min, float speed_max, bool ground) {
    Emitter e;
    e.look = look_name;
    e.rate = rate;
    e.burst = burst;
    e.cone_degrees = cone;
    e.speed_min = speed_min;
    e.speed_max = speed_max;
    e.ground = ground;
    return e;
}

std::vector<Effect> make_effects() {
    std::vector<Effect> out;
    {
        Effect f{"smoke", {}, 10.0f, {}};
        Emitter e = emitter("smoke", 6.0f, 0, 15.0f, 0.5f, 1.0f, false);
        e.shape = EmitterShape::disc;
        e.size = {0.3f, 0.0f, 0.0f};
        f.emitters.push_back(e);
        out.push_back(f);
    }
    {
        Effect f{"fire", {}, 10.0f, {}};
        Emitter flames = emitter("fire", 40.0f, 0, 20.0f, 0.3f, 0.8f, false);
        flames.shape = EmitterShape::disc;
        flames.size = {0.3f, 0.0f, 0.0f};
        Emitter embers = emitter("embers", 3.0f, 0, 25.0f, 0.5f, 1.5f, false);
        embers.shape = EmitterShape::disc;
        embers.size = {0.3f, 0.0f, 0.0f};
        Emitter smoke = emitter("smoke", 3.0f, 0, 15.0f, 0.5f, 1.0f, false);
        smoke.offset = {0.0f, 1.0f, 0.0f};
        smoke.size_scale = 0.6f;
        f.emitters = {flames, embers, smoke};
        f.light.on = true; // its glow, flickering (the defaults: firelight)
        out.push_back(f);
    }
    out.push_back({"sparks", {emitter("sparks", 0.0f, 40, 60.0f, 3.0f, 8.0f, true)}, 0.0f, {}});
    {
        Emitter e = emitter("dust_kick", 0.0f, 12, 70.0f, 0.5f, 2.0f, true);
        e.shape = EmitterShape::sphere;
        e.size = {0.2f, 0.0f, 0.0f};
        out.push_back({"dust_kick", {e}, 0.0f, {}});
    }
    out.push_back({"spray", {emitter("spray", 0.0f, 60, 25.0f, 3.0f, 6.0f, true)}, 0.0f, {}});
    {
        Emitter e = emitter("mist", 8.0f, 0, 180.0f, 0.1f, 0.4f, false);
        e.shape = EmitterShape::box;
        e.size = {2.0f, 0.5f, 2.0f};
        out.push_back({"mist", {e}, 10.0f, {}});
    }
    {
        Emitter e = emitter("embers", 4.0f, 0, 25.0f, 0.5f, 1.5f, false);
        e.shape = EmitterShape::disc;
        e.size = {0.4f, 0.0f, 0.0f};
        out.push_back({"embers", {e}, 10.0f, {}});
    }
    out.push_back({"debris_chips", {emitter("debris_chips", 0.0f, 24, 60.0f, 2.0f, 6.0f, true)}, 0.0f, {}});
    out.push_back({"splinters", {emitter("splinters", 0.0f, 16, 50.0f, 2.0f, 6.0f, true)}, 0.0f, {}});
    {
        Emitter e = emitter("leaves", 0.0f, 60, 180.0f, 0.5f, 2.0f, true);
        e.shape = EmitterShape::sphere;
        e.size = {3.0f, 0.0f, 0.0f};
        out.push_back({"leaves", {e}, 0.0f, {}});
    }
    out.push_back({"rain_splash", {emitter("rain_splash", 0.0f, 3, 50.0f, 0.8f, 1.6f, true)}, 0.0f, {}});
    {
        Emitter e = emitter("grit", 20.0f, 0, 60.0f, 0.2f, 1.0f, false);
        e.shape = EmitterShape::disc;
        e.size = {2.0f, 0.0f, 0.0f};
        out.push_back({"grit", {e}, 5.0f, {}});
    }
    return out;
}

template <typename T>
const T* find_named(std::span<const T> scene, std::span<const T> built_in, std::string_view name) {
    for (const T& t : scene) {
        if (t.name == name) return &t;
    }
    for (const T& t : built_in) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

} // namespace

const char* to_string(ParticleForm form) { return form == ParticleForm::streak ? "streak" : "puff"; }

std::optional<ParticleForm> particle_form_from_string(std::string_view name) {
    if (name == "puff") return ParticleForm::puff;
    if (name == "streak") return ParticleForm::streak;
    return std::nullopt;
}

const char* to_string(EmitterShape shape) {
    switch (shape) {
    case EmitterShape::point: return "point";
    case EmitterShape::sphere: return "sphere";
    case EmitterShape::box: return "box";
    case EmitterShape::disc: return "disc";
    }
    return "point";
}

std::optional<EmitterShape> emitter_shape_from_string(std::string_view name) {
    for (const EmitterShape s : {EmitterShape::point, EmitterShape::sphere, EmitterShape::box, EmitterShape::disc}) {
        if (name == to_string(s)) return s;
    }
    return std::nullopt;
}

bool effect_name_ok(std::string_view name) { return !name.empty() && name.size() <= k_max_effect_name_bytes; }

std::optional<std::string> check_particle_look(const ParticleLook& l) {
    if (!effect_name_ok(l.name)) return "name must be 1 to " + std::to_string(k_max_effect_name_bytes) + " bytes";
    if (auto bad = first({number("life_min", l.life_min, 0.0f, k_max_look_life),
                          number("life_max", l.life_max, 0.0f, k_max_look_life)})) {
        return bad;
    }
    if (!(l.life_min > 0.0f) || l.life_max < l.life_min) return "life_min must be over 0 and at most life_max";
    return first({number("size_start", l.size_start, 0.0f, k_max_look_size),
                  number("size_end", l.size_end, 0.0f, k_max_look_size),
                  vec("color_start", l.color_start, 0.0f, 1.0f),
                  vec("color_end", l.color_end, 0.0f, 1.0f),
                  number("opacity_start", l.opacity_start, 0.0f, 1.0f),
                  number("opacity_end", l.opacity_end, 0.0f, 1.0f),
                  vec("emissive", l.emissive, 0.0f, k_max_look_light),
                  number("emissive_end", l.emissive_end, 0.0f, 1.0f),
                  number("gravity", l.gravity, -k_max_look_pull, k_max_look_pull),
                  number("buoyancy", l.buoyancy, -k_max_look_buoyancy, k_max_look_buoyancy),
                  number("drag", l.drag, 0.0f, k_max_look_drag),
                  number("wind", l.wind, -k_max_look_pull, k_max_look_pull),
                  number("wobble", l.wobble, 0.0f, k_max_look_size),
                  number("soft", l.soft, 0.0f, k_max_look_size),
                  (l.form == ParticleForm::puff || l.form == ParticleForm::streak) ? std::string{}
                                                                                     : std::string("form must be puff or streak")});
}

std::optional<std::string> check_emitter(const Emitter& e) {
    if (!effect_name_ok(e.look)) return "look must be 1 to " + std::to_string(k_max_effect_name_bytes) + " bytes";
    if (e.burst > k_max_emitter_burst) return "burst must be from 0 to " + std::to_string(k_max_emitter_burst);
    if (auto bad = first({number("rate", e.rate, 0.0f, k_max_emitter_rate),
                          vec("size", e.size, 0.0f, k_max_emitter_reach),
                          vec("offset", e.offset, -k_max_emitter_reach, k_max_emitter_reach),
                          vec("direction", e.direction, -1.0e6f, 1.0e6f),
                          number("cone_degrees", e.cone_degrees, 0.0f, 180.0f),
                          number("speed_min", e.speed_min, 0.0f, k_max_emitter_speed),
                          number("speed_max", e.speed_max, 0.0f, k_max_emitter_speed),
                          number("size_scale", e.size_scale, 0.0f, 1000.0f)})) {
        return bad;
    }
    if (e.shape != EmitterShape::point && e.shape != EmitterShape::sphere && e.shape != EmitterShape::box &&
        e.shape != EmitterShape::disc) {
        return "shape must be point, sphere, box or disc";
    }
    if (length_squared(e.direction) < 1e-12f) return "direction must not be zero";
    if (e.speed_max < e.speed_min) return "speed_min must be at most speed_max";
    if (!(e.size_scale > 0.0f)) return "size_scale must be over 0";
    return std::nullopt;
}

std::optional<std::string> check_effect_light(const EffectLight& l) {
    return first({vec("color", l.color, 0.0f, 1.0f),
                  number("intensity", l.intensity, 0.0f, k_max_effect_light_intensity),
                  number("range", l.range, 0.1f, k_max_effect_light_range),
                  vec("offset", l.offset, -k_max_emitter_reach, k_max_emitter_reach),
                  number("flicker", l.flicker, 0.0f, 1.0f),
                  number("source_radius", l.source_radius, 0.0f, 10.0f),
                  number("shadow_near", l.shadow_near, 0.01f, 10.0f)});
}

float effect_flicker(float flicker, double seconds, u64 seed) {
    if (!(flicker > 0.0f)) return 1.0f;
    if (!std::isfinite(seconds)) seconds = 0.0;
    // Phases from the seed (splitmix64), frequencies apart so it never repeats soon.
    u64 z = seed + 0x9e3779b97f4a7c15ull;
    const auto next = [&] {
        z += 0x9e3779b97f4a7c15ull;
        u64 x = z;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        x ^= x >> 31;
        return static_cast<double>(x >> 11) * (6.283185307179586 / 9007199254740992.0);
    };
    const double p1 = next(), p2 = next(), p3 = next();
    constexpr double k_tau = 6.283185307179586;
    // (Each sine's turns kept small before the sine: a long clock keeps its precision.)
    const auto wave = [&](double hz, double phase) { return std::sin(k_tau * std::fmod(seconds * hz, 1.0) + phase); };
    const double w = 0.5 * wave(1.7, p1) + 0.3 * wave(4.3, p2) + 0.2 * wave(9.1, p3); // -1 to 1
    const float waver = static_cast<float>(0.5 + 0.5 * w);
    return std::clamp(1.0f - std::min(flicker, 1.0f) * waver, 0.0f, 1.0f);
}

std::optional<std::string> check_effect(const Effect& f) {
    if (!effect_name_ok(f.name)) return "name must be 1 to " + std::to_string(k_max_effect_name_bytes) + " bytes";
    if (f.emitters.empty() || f.emitters.size() > k_max_emitters) {
        return "emitters must be 1 to " + std::to_string(k_max_emitters);
    }
    if (auto bad = first({number("duration", f.duration, 0.0f, k_max_effect_seconds)})) return bad;
    if (auto bad = check_effect_light(f.light)) return "light." + *bad;
    for (usize i = 0; i < f.emitters.size(); ++i) {
        if (auto bad = check_emitter(f.emitters[i])) return "emitters[" + std::to_string(i) + "]." + *bad;
    }
    return std::nullopt;
}

std::optional<std::string> check_entity_effect(const EntityEffect& e) {
    if (e.effect.size() > k_max_effect_name_bytes) {
        return "effect must be at most " + std::to_string(k_max_effect_name_bytes) + " bytes";
    }
    if (e.on && e.effect.empty()) return "effect must name an effect when on";
    if (auto bad = first({number("rate", e.rate, 0.0f, k_max_effect_rate_scale)})) return bad;
    return std::nullopt;
}

std::span<const ParticleLook> built_in_particle_looks() {
    static const std::vector<ParticleLook> looks = make_looks();
    return looks;
}

std::span<const Effect> built_in_effects() {
    static const std::vector<Effect> effects = make_effects();
    return effects;
}

const ParticleLook* find_particle_look(std::span<const ParticleLook> scene, std::string_view name) {
    return find_named(scene, built_in_particle_looks(), name);
}

const Effect* find_effect(std::span<const Effect> scene, std::string_view name) {
    return find_named(scene, built_in_effects(), name);
}

} // namespace giant::world
