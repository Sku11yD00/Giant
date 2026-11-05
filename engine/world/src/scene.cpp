#include "giant/world/scene.h"

#include "giant/core/assert.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <set>

namespace giant::world {

namespace {

using Json = nlohmann::ordered_json;

constexpr const char* k_format = "giant-scene";
constexpr int k_version = 1;

} // namespace

const char* to_string(Shape shape) {
    switch (shape) {
    case Shape::cube: return "cube";
    case Shape::sphere: return "sphere";
    case Shape::plane: return "plane";
    }
    return "cube";
}

std::optional<Shape> shape_from_string(std::string_view name) {
    for (const Shape s : {Shape::cube, Shape::sphere, Shape::plane}) {
        if (name == to_string(s)) {
            return s;
        }
    }
    return std::nullopt;
}

Vec3 sun_direction(const Lighting& lighting) {
    const float elevation = degrees_to_radians(lighting.sun_elevation_degrees);
    const float azimuth = degrees_to_radians(lighting.sun_azimuth_degrees);
    const float flat = std::cos(elevation);
    return {flat * std::sin(azimuth), std::sin(elevation), -flat * std::cos(azimuth)};
}

// --- Scene ----------------------------------------------------------------------

EntityId Scene::add(Entity entity, std::optional<usize> at) {
    if (entity.id == 0) {
        entity.id = m_next_id;
    }
    GIANT_ASSERT(!m_index.contains(entity.id), "entity id already in use");
    m_next_id = std::max(m_next_id, entity.id + 1);
    const usize index = std::min(at.value_or(m_entities.size()), m_entities.size());
    const EntityId id = entity.id;
    m_entities.insert(m_entities.begin() + static_cast<std::ptrdiff_t>(index), std::move(entity));
    reindex(index);
    return id;
}

std::optional<Scene::Removed> Scene::remove(EntityId id) {
    const auto it = m_index.find(id);
    if (it == m_index.end()) {
        return std::nullopt;
    }
    const usize index = it->second;
    Removed removed{std::move(m_entities[index]), index};
    m_entities.erase(m_entities.begin() + static_cast<std::ptrdiff_t>(index));
    m_index.erase(it);
    reindex(index);
    return removed;
}

Entity* Scene::find(EntityId id) {
    const auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : &m_entities[it->second];
}

const Entity* Scene::find(EntityId id) const { return const_cast<Scene*>(this)->find(id); }

void Scene::clear() {
    m_entities.clear();
    m_index.clear();
    // Ids are never reused within a session, so an undo history that
    // mentions an old id cannot reach a new entity by mistake.
}

void Scene::reindex(usize from) {
    for (usize i = from; i < m_entities.size(); ++i) {
        m_index[m_entities[i].id] = i;
    }
}

// --- Saving -----------------------------------------------------------------------

namespace {

Json vec3_json(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }
Json dvec3_json(const DVec3& v) { return Json::array({v.x, v.y, v.z}); }

} // namespace

std::string save_scene(const Scene& scene) {
    const Lighting& l = scene.lighting;
    Json out;
    out["format"] = k_format;
    out["version"] = k_version;
    out["next_id"] = scene.next_id();
    out["lighting"] = {
        {"sun_elevation_degrees", l.sun_elevation_degrees},
        {"sun_azimuth_degrees", l.sun_azimuth_degrees},
        {"sun_color", vec3_json(l.sun_color)},
        {"sun_intensity", l.sun_intensity},
        {"sky_color", vec3_json(l.sky_color)},
        {"ground_color", vec3_json(l.ground_color)},
        {"ambient_intensity", l.ambient_intensity},
        {"exposure", l.exposure},
    };
    Json entities = Json::array();
    for (const Entity& e : scene.entities()) {
        const Quat& q = e.transform.rotation;
        entities.push_back({
            {"id", e.id},
            {"name", e.name},
            {"shape", to_string(e.shape)},
            {"visible", e.visible},
            {"position", dvec3_json(e.transform.position)},
            {"rotation", Json::array({q.x, q.y, q.z, q.w})},
            {"scale", vec3_json(e.transform.scale)},
            {"material",
             {
                 {"base_color", vec3_json(e.material.base_color)},
                 {"roughness", e.material.roughness},
                 {"metallic", e.material.metallic},
                 {"emissive", vec3_json(e.material.emissive)},
             }},
        });
    }
    out["entities"] = std::move(entities);
    return out.dump(2) + "\n";
}

// --- Loading ----------------------------------------------------------------------

namespace {

// Reads one JSON object strictly: every key must be known, every value the
// right type and finite. The first problem is kept, with its path.
class Reader {
public:
    Reader(const Json& object, std::string path, std::string& error)
        : m_object(object), m_path(std::move(path)), m_error(error) {
        if (!object.is_object()) {
            fail("", "must be an object");
        }
    }

    // Call after reading all keys: reports any key nobody asked for.
    void finish() {
        if (!ok() || !m_object.is_object()) {
            return;
        }
        for (const auto& [key, value] : m_object.items()) {
            if (!m_seen.contains(key)) {
                fail(key, "is not a known key");
                return;
            }
        }
    }

    bool ok() const { return m_error.empty(); }

    const Json* get(const char* key, bool required) {
        m_seen.insert(key);
        if (!ok() || !m_object.is_object()) {
            return nullptr;
        }
        const auto it = m_object.find(key);
        if (it == m_object.end()) {
            if (required) {
                fail(key, "is missing");
            }
            return nullptr;
        }
        return &*it;
    }

    void number(const char* key, float& out, bool required = false) {
        if (const Json* v = get(key, required)) {
            if (!v->is_number() || !std::isfinite(v->get<double>())) {
                fail(key, "must be a finite number");
                return;
            }
            out = v->get<float>();
        }
    }

    template <typename V>
    void vec3(const char* key, V& out, bool required = false) {
        if (const Json* v = get(key, required)) {
            if (!v->is_array() || v->size() != 3) {
                fail(key, "must be an array of 3 numbers");
                return;
            }
            double xyz[3];
            for (usize i = 0; i < 3; ++i) {
                if (!(*v)[i].is_number() || !std::isfinite((*v)[i].get<double>())) {
                    fail(key, "must be an array of 3 finite numbers");
                    return;
                }
                xyz[i] = (*v)[i].get<double>();
            }
            using T = decltype(out.x);
            out = {static_cast<T>(xyz[0]), static_cast<T>(xyz[1]), static_cast<T>(xyz[2])};
        }
    }

    void quat(const char* key, Quat& out) {
        if (const Json* v = get(key, false)) {
            if (!v->is_array() || v->size() != 4) {
                fail(key, "must be an array of 4 numbers (x, y, z, w)");
                return;
            }
            float q[4];
            for (usize i = 0; i < 4; ++i) {
                if (!(*v)[i].is_number() || !std::isfinite((*v)[i].get<double>())) {
                    fail(key, "must be an array of 4 finite numbers");
                    return;
                }
                q[i] = (*v)[i].get<float>();
            }
            const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (length < 1e-6f) {
                fail(key, "must not be all zeros");
                return;
            }
            out = normalize(Quat{q[0], q[1], q[2], q[3]});
        }
    }

    void string(const char* key, std::string& out, bool required = false) {
        if (const Json* v = get(key, required)) {
            if (!v->is_string()) {
                fail(key, "must be a string");
                return;
            }
            out = v->get<std::string>();
        }
    }

    void boolean(const char* key, bool& out) {
        if (const Json* v = get(key, false)) {
            if (!v->is_boolean()) {
                fail(key, "must be true or false");
                return;
            }
            out = v->get<bool>();
        }
    }

    // A positive whole number.
    void id(const char* key, u64& out, bool required) {
        if (const Json* v = get(key, required)) {
            if (!v->is_number_unsigned() || v->get<u64>() == 0) {
                fail(key, "must be a whole number above 0");
                return;
            }
            out = v->get<u64>();
        }
    }

    void fail(std::string_view key, std::string_view problem) {
        if (!ok()) {
            return;
        }
        m_error = m_path;
        if (!key.empty()) {
            m_error += m_path.empty() ? "" : ".";
            m_error += key;
        }
        m_error += m_error.empty() ? "" : " ";
        m_error += problem;
    }

    const std::string& path() const { return m_path; }

private:
    const Json& m_object;
    std::string m_path;
    std::string& m_error;
    std::set<std::string, std::less<>> m_seen;
};

void read_lighting(const Json& json, Lighting& l, std::string& error) {
    Reader r(json, "lighting", error);
    r.number("sun_elevation_degrees", l.sun_elevation_degrees);
    r.number("sun_azimuth_degrees", l.sun_azimuth_degrees);
    r.vec3("sun_color", l.sun_color);
    r.number("sun_intensity", l.sun_intensity);
    r.vec3("sky_color", l.sky_color);
    r.vec3("ground_color", l.ground_color);
    r.number("ambient_intensity", l.ambient_intensity);
    r.number("exposure", l.exposure);
    r.finish();
}

void read_entity(const Json& json, const std::string& path, Entity& e, std::string& error) {
    Reader r(json, path, error);
    r.id("id", e.id, true);
    r.string("name", e.name);
    std::string shape = to_string(e.shape);
    r.string("shape", shape);
    if (r.ok()) {
        if (const auto parsed = shape_from_string(shape)) {
            e.shape = *parsed;
        } else {
            r.fail("shape", "must be cube, sphere or plane");
        }
    }
    r.boolean("visible", e.visible);
    r.vec3("position", e.transform.position);
    r.quat("rotation", e.transform.rotation);
    r.vec3("scale", e.transform.scale);
    if (const Json* material = r.get("material", false); material != nullptr && r.ok()) {
        Reader m(*material, path + ".material", error);
        m.vec3("base_color", e.material.base_color);
        m.number("roughness", e.material.roughness);
        m.number("metallic", e.material.metallic);
        m.vec3("emissive", e.material.emissive);
        m.finish();
    }
    r.finish();
}

} // namespace

std::optional<LoadError> load_scene(std::string_view text, Scene& out) {
    Json json;
    try {
        json = Json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        return LoadError{std::string("not valid JSON: ") + e.what()};
    }
    std::string error;
    Reader root(json, "", error);
    std::string format;
    root.string("format", format, true);
    if (root.ok() && format != k_format) {
        root.fail("format", "must be \"giant-scene\"");
    }
    if (const Json* version = root.get("version", true); version != nullptr && root.ok()) {
        if (!version->is_number_integer() || version->get<i64>() != k_version) {
            root.fail("version", "must be 1 (this editor reads version 1 scenes)");
        }
    }
    u64 next_id = 1;
    root.id("next_id", next_id, false);

    Scene scene;
    if (const Json* lighting = root.get("lighting", false); lighting != nullptr && root.ok()) {
        read_lighting(*lighting, scene.lighting, error);
    }
    if (const Json* entities = root.get("entities", false); entities != nullptr && root.ok()) {
        if (!entities->is_array()) {
            root.fail("entities", "must be an array");
        } else {
            for (usize i = 0; i < entities->size() && error.empty(); ++i) {
                const std::string path = "entities[" + std::to_string(i) + "]";
                Entity entity;
                read_entity((*entities)[i], path, entity, error);
                if (error.empty() && scene.find(entity.id) != nullptr) {
                    error = path + ".id " + std::to_string(entity.id) + " is used twice";
                }
                if (error.empty()) {
                    scene.add(std::move(entity));
                }
            }
        }
    }
    root.finish();
    if (!error.empty()) {
        return LoadError{std::move(error)};
    }
    // A hand-edited next_id that is too low must not hand out used ids.
    scene.reserve_ids_below(next_id);
    out = std::move(scene);
    return std::nullopt;
}

} // namespace giant::world
