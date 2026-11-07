#include "giant/world/scene.h"
#include "giant/world/far_ground.h"
#include "giant/world/scatter.h"

#include "giant/core/assert.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <limits>
#include <set>
#include <unordered_map>

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
    case Shape::mesh: return "mesh";
    case Shape::kit: return "kit";
    case Shape::decal: return "decal";
    }
    return "cube";
}

std::optional<Shape> shape_from_string(std::string_view name) {
    for (const Shape s : {Shape::cube, Shape::sphere, Shape::plane, Shape::mesh, Shape::kit, Shape::decal}) {
        if (name == to_string(s)) {
            return s;
        }
    }
    return std::nullopt;
}

const char* to_string(BodyKind body) {
    switch (body) {
    case BodyKind::fixed: return "fixed";
    case BodyKind::dynamic: return "dynamic";
    case BodyKind::none: return "none";
    }
    return "fixed";
}

std::optional<BodyKind> body_from_string(std::string_view name) {
    for (const BodyKind b : {BodyKind::fixed, BodyKind::dynamic, BodyKind::none}) {
        if (name == to_string(b)) {
            return b;
        }
    }
    return std::nullopt;
}

const char* to_string(DecalShape shape) {
    switch (shape) {
    case DecalShape::disc: return "disc";
    case DecalShape::rectangle: return "rectangle";
    case DecalShape::ring: return "ring";
    case DecalShape::splat: return "splat";
    case DecalShape::cracks: return "cracks";
    }
    return "disc";
}

std::optional<DecalShape> decal_shape_from_string(std::string_view name) {
    for (const DecalShape s : {DecalShape::disc, DecalShape::rectangle, DecalShape::ring, DecalShape::splat,
                               DecalShape::cracks}) {
        if (name == to_string(s)) {
            return s;
        }
    }
    return std::nullopt;
}

const Kit* Scene::find_kit(std::string_view name) const {
    for (const Kit& k : kits) {
        if (k.name == name) {
            return &k;
        }
    }
    return nullptr;
}

Kit* Scene::find_kit(std::string_view name) {
    return const_cast<Kit*>(static_cast<const Scene*>(this)->find_kit(name));
}

Vec3 sun_direction(const Lighting& lighting) {
    const float elevation = degrees_to_radians(lighting.sun_elevation_degrees);
    const float azimuth = degrees_to_radians(lighting.sun_azimuth_degrees);
    const float flat = std::cos(elevation);
    return {flat * std::sin(azimuth), std::sin(elevation), -flat * std::cos(azimuth)};
}

// --- Cells ----------------------------------------------------------------------

usize CellCoordHash::operator()(const CellCoord& c) const {
    const u64 key = (static_cast<u64>(static_cast<u32>(c.x)) << 32) | static_cast<u32>(c.z);
    return static_cast<usize>(key * 0x9E3779B97F4A7C15ull ^ (key >> 29));
}

CellCoord cell_of(const DVec3& position) {
    const auto axis = [](double v) {
        const double cell = std::floor(v / k_cell_size);
        if (!(cell >= -2147483648.0)) {
            return std::numeric_limits<i32>::min(); // also NaN
        }
        return cell >= 2147483647.0 ? std::numeric_limits<i32>::max() : static_cast<i32>(cell);
    };
    return {axis(position.x), axis(position.z)};
}

// --- Scene ----------------------------------------------------------------------

Scene::Identity::Identity() {
    static std::atomic<u64> next{1};
    value = next.fetch_add(1, std::memory_order_relaxed);
}

Scene::Identity& Scene::Identity::operator=(const Identity&) {
    value = Identity().value;
    return *this;
}

Scene::Scene(const Scene& other)
    : lighting(other.lighting), kits(other.kits), terrain(other.terrain), scatter(other.scatter),
      weather(other.weather), m_entities(other.m_entities), m_index(other.m_index),
      m_next_id(other.m_next_id) {}

Scene::Scene(Scene&& other) noexcept
    : lighting(other.lighting), kits(std::move(other.kits)), terrain(std::move(other.terrain)),
      scatter(std::move(other.scatter)), weather(other.weather), m_entities(std::move(other.m_entities)),
      m_index(std::move(other.m_index)), m_next_id(other.m_next_id) {
    other.clear();
}

Scene& Scene::operator=(const Scene& other) {
    if (this != &other) {
        *this = Scene(other);
    }
    return *this;
}

Scene& Scene::operator=(Scene&& other) noexcept {
    if (this != &other) {
        lighting = other.lighting;
        kits = std::move(other.kits);
        terrain = std::move(other.terrain);
        scatter = std::move(other.scatter);
        weather = other.weather;
        m_entities = std::move(other.m_entities);
        m_index = std::move(other.m_index);
        m_next_id = other.m_next_id;
        m_changes = {{}, true};
        m_identity = Identity();
        other.clear();
    }
    return *this;
}

void Scene::changed(EntityId id) {
    if (m_changes.all) {
        return;
    }
    // Nobody may be taking them (tests, a scene being built): past a
    // point, "everything" says it as well in less memory.
    if (m_changes.ids.size() >= 4096 + m_entities.size()) {
        m_changes.ids.clear();
        m_changes.ids.shrink_to_fit();
        m_changes.all = true;
        return;
    }
    m_changes.ids.push_back(id);
}

SceneChanges Scene::take_changes() {
    SceneChanges out = std::move(m_changes);
    m_changes = {};
    return out;
}

EntityId Scene::add(Entity entity, std::optional<usize> at) {
    if (entity.id == 0) {
        if (m_next_id > k_max_entity_id) {
            return 0; // every id has been used; callers report it
        }
        entity.id = m_next_id;
    }
    GIANT_ASSERT(entity.id <= k_max_entity_id, "entity ids are at most 2^53 - 1");
    GIANT_ASSERT(!m_index.contains(entity.id), "entity id already in use");
    m_next_id = std::max(m_next_id, entity.id + 1);
    const usize index = std::min(at.value_or(m_entities.size()), m_entities.size());
    const EntityId id = entity.id;
    m_entities.insert(m_entities.begin() + static_cast<std::ptrdiff_t>(index), std::move(entity));
    reindex(index);
    changed(id);
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
    changed(id);
    return removed;
}

std::vector<Scene::Removed> Scene::remove_many(std::span<const EntityId> ids) {
    std::unordered_map<EntityId, bool> doomed;
    for (const EntityId id : ids) {
        if (m_index.contains(id)) {
            doomed[id] = true;
        }
    }
    std::vector<Removed> removed;
    if (doomed.empty()) {
        return removed;
    }
    removed.reserve(doomed.size());
    // Compacted in place (keeping the capacity, so adding after a removal
    // does not grow the list again); only what moved is reindexed.
    usize first = m_entities.size();
    usize kept = 0;
    for (usize i = 0; i < m_entities.size(); ++i) {
        if (doomed.contains(m_entities[i].id)) {
            m_index.erase(m_entities[i].id);
            changed(m_entities[i].id);
            removed.push_back({std::move(m_entities[i]), i});
            first = std::min(first, i);
        } else {
            if (kept != i) {
                m_entities[kept] = std::move(m_entities[i]);
            }
            ++kept;
        }
    }
    m_entities.erase(m_entities.begin() + static_cast<std::ptrdiff_t>(kept), m_entities.end());
    reindex(first);
    return removed;
}

void Scene::restore_many(std::vector<Removed> removed) {
    if (removed.empty()) {
        return;
    }
    std::sort(removed.begin(), removed.end(),
              [](const Removed& a, const Removed& b) { return a.index < b.index; });
    // Indices are places in the list as it was before removal, so merging in
    // ascending order lands each one exactly where it was.
    std::vector<Entity> merged;
    merged.reserve(m_entities.size() + removed.size());
    usize next = 0;
    for (Removed& r : removed) {
        GIANT_ASSERT(!m_index.contains(r.entity.id), "entity id already in use");
        while (merged.size() < r.index && next < m_entities.size()) {
            merged.push_back(std::move(m_entities[next++]));
        }
        m_next_id = std::max(m_next_id, r.entity.id + 1);
        changed(r.entity.id);
        merged.push_back(std::move(r.entity));
    }
    while (next < m_entities.size()) {
        merged.push_back(std::move(m_entities[next++]));
    }
    m_entities = std::move(merged);
    reindex(0);
}

const Entity* Scene::find(EntityId id) const {
    const auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : &m_entities[it->second];
}

Entity* Scene::modify(EntityId id) {
    const auto it = m_index.find(id);
    if (it == m_index.end()) {
        return nullptr;
    }
    changed(id);
    return &m_entities[it->second];
}

void Scene::clear() {
    m_entities.clear();
    m_index.clear();
    m_changes = {{}, true};
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

namespace {

// What entities and kit pieces share: shape, files, transform, material.
void write_look(Json& out, Shape shape, const std::string& mesh, const std::string& kit,
                const Transform& t, const Material& m, const DecalSettings& d) {
    const Quat& q = t.rotation;
    out["shape"] = to_string(shape);
    out["position"] = dvec3_json(t.position);
    out["rotation"] = Json::array({q.x, q.y, q.z, q.w});
    out["scale"] = vec3_json(t.scale);
    out["material"] = {
        {"base_color", vec3_json(m.base_color)},
        {"roughness", m.roughness},
        {"metallic", m.metallic},
        {"emissive", vec3_json(m.emissive)},
    };
    if (!mesh.empty()) {
        out["mesh"] = mesh;
    }
    if (!kit.empty()) {
        out["kit"] = kit;
    }
    // Kept when the shape changes (as mesh and kit are), so written whenever set.
    const DecalSettings none;
    if (shape == Shape::decal || d.shape != none.shape || d.opacity != none.opacity ||
        d.softness != none.softness || d.seed != none.seed) {
        out["decal"] = {{"shape", to_string(d.shape)}, {"opacity", d.opacity}, {"softness", d.softness},
                        {"seed", d.seed}};
    }
}

Json weather_json(const Weather& w) {
    return {
        {"cloud_cover", w.cloud_cover},
        {"cloud_density", w.cloud_density},
        {"cloud_base", w.cloud_base},
        {"cloud_thickness", w.cloud_thickness},
        {"cloud_scale", w.cloud_scale},
        {"wind_direction_degrees", w.wind_direction_degrees},
        {"wind_speed", w.wind_speed},
        {"fog_density", w.fog_density},
        {"fog_height", w.fog_height},
        {"haze", w.haze},
        {"precipitation", w.precipitation},
        {"temperature", w.temperature},
        {"wetness", w.wetness},
        {"snow_cover", w.snow_cover},
    };
}

Json lighting_json(const Lighting& l) {
    return {
        {"sun_elevation_degrees", l.sun_elevation_degrees},
        {"sun_azimuth_degrees", l.sun_azimuth_degrees},
        {"sun_color", vec3_json(l.sun_color)},
        {"sun_intensity", l.sun_intensity},
        {"sky_color", vec3_json(l.sky_color)},
        {"ground_color", vec3_json(l.ground_color)},
        {"ambient_intensity", l.ambient_intensity},
        {"exposure", l.exposure},
    };
}

Json kits_json(std::span<const Kit> kits) {
    Json out = Json::array();
    for (const Kit& k : kits) {
        Json pieces = Json::array();
        for (const KitPiece& p : k.pieces) {
            Json piece = {{"name", p.name}};
            write_look(piece, p.shape, p.mesh, {}, p.transform, p.material, p.decal);
            pieces.push_back(std::move(piece));
        }
        Json kit = {{"name", k.name}, {"pieces", std::move(pieces)}};
        if (k.breakable) kit["breakable"] = true;
        if (k.strength != 0.0f) kit["strength"] = k.strength;
        out.push_back(std::move(kit));
    }
    return out;
}

Json scatter_json(std::span<const ScatterLayer> layers) {
    Json out = Json::array();
    const ScatterLayer none;
    for (const ScatterLayer& l : layers) {
        Json layer = {{"name", l.name},         {"spacing", l.spacing},   {"density", l.density},
                      {"jitter", l.jitter},     {"seed", l.seed},         {"visible", l.visible}};
        if (l.clump_size != none.clump_size || l.clumping != none.clumping) {
            layer["clump_size"] = l.clump_size;
            layer["clumping"] = l.clumping;
        }
        if (l.height_min != none.height_min) layer["height_min"] = l.height_min;
        if (l.height_max != none.height_max) layer["height_max"] = l.height_max;
        if (l.slope_min != none.slope_min) layer["slope_min"] = l.slope_min;
        if (l.slope_max != none.slope_max) layer["slope_max"] = l.slope_max;
        if (l.area) {
            layer["area"] = {{"from", Json::array({l.area->from_x, l.area->from_z})},
                             {"to", Json::array({l.area->to_x, l.area->to_z})}};
        }
        Json items = Json::array();
        for (const ScatterItem& i : l.items) {
            Json item = {{"shape", to_string(i.shape)}};
            if (!i.mesh.empty()) item["mesh"] = i.mesh;
            if (!i.kit.empty()) item["kit"] = i.kit;
            item["material"] = {{"base_color", vec3_json(i.material.base_color)},
                                {"roughness", i.material.roughness},
                                {"metallic", i.material.metallic},
                                {"emissive", vec3_json(i.material.emissive)}};
            item["weight"] = i.weight;
            item["scale"] = Json::array({i.scale_min, i.scale_max});
            item["align"] = i.align;
            item["sink"] = i.sink;
            if (i.tree) {
                item["tree"] = {{"trunk_radius", i.tree->trunk_radius},
                                {"height", i.tree->height},
                                {"strength", i.tree->strength}};
            }
            items.push_back(std::move(item));
        }
        layer["items"] = std::move(items);
        out.push_back(std::move(layer));
    }
    return out;
}

// One row of a terrain tile as text: its heights, shortest exact form,
// one space apart (a row per line keeps a tile readable and diffable).
std::string heights_row(std::span<const float> row) {
    std::string out;
    out.reserve(row.size() * 8);
    char text[32];
    for (usize i = 0; i < row.size(); ++i) {
        const auto [end, ec] = std::to_chars(text, text + sizeof(text), row[i]);
        GIANT_ASSERT(ec == std::errc{}, "a height did not fit its text");
        if (i > 0) out += ' ';
        out.append(text, end);
    }
    return out;
}

// A tile's heights as rows of text, one per row of samples.
Json heights_json(const Terrain::Heights& h, usize n) {
    Json rows = Json::array();
    for (usize j = 0; j < n; ++j) {
        rows.push_back(heights_row(std::span<const float>(h).subspan(j * n, n)));
    }
    return rows;
}

Json terrain_material_json(const TerrainMaterial& m) {
    return {{"base_color", vec3_json(m.base_color)}, {"roughness", m.roughness}};
}

Json terrain_json(const Terrain& t) {
    std::vector<CellCoord> cells;
    cells.reserve(t.size());
    for (const auto& [cell, heights] : t.tiles()) cells.push_back(cell);
    std::sort(cells.begin(), cells.end(),
              [](const CellCoord& a, const CellCoord& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
    const usize n = t.samples();
    Json tiles = Json::array();
    for (const CellCoord c : cells) {
        tiles.push_back({{"cell", Json::array({c.x, c.z})}, {"heights", heights_json(*t.find(c), n)}});
    }
    return {{"samples", t.samples()}, {"material", terrain_material_json(t.material)}, {"tiles", std::move(tiles)}};
}

Json entity_json(const Entity& e) {
    Json entity = {{"id", e.id}, {"name", e.name}, {"visible", e.visible}};
    write_look(entity, e.shape, e.mesh, e.kit, e.transform, e.material, e.decal);
    // Only when not the default, so older scenes read and write the same.
    if (e.body != BodyKind::fixed) entity["body"] = to_string(e.body);
    if (e.mass != 0.0f) entity["mass"] = e.mass;
    return entity;
}

} // namespace

std::string save_scene(const Scene& scene) {
    Json out;
    out["format"] = k_format;
    out["version"] = k_version;
    out["next_id"] = scene.next_id();
    out["lighting"] = lighting_json(scene.lighting);
    if (!scene.kits.empty()) {
        out["kits"] = kits_json(scene.kits);
    }
    if (!scene.scatter.empty()) {
        out["scatter"] = scatter_json(scene.scatter);
    }
    if (scene.weather) {
        out["weather"] = weather_json(*scene.weather);
    }
    if (const Terrain& t = scene.terrain;
        !t.empty() || t.samples() != k_default_terrain_samples || !(t.material == TerrainMaterial{})) {
        out["terrain"] = terrain_json(t);
    }
    Json entities = Json::array();
    for (const Entity& e : scene.entities()) {
        entities.push_back(entity_json(e));
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

    // The same limits the editor commands enforce, so a loaded scene is one
    // the editor could have made (and can save again).
    void number(const char* key, float& out, double minimum, double maximum) {
        if (const Json* v = get(key, false)) {
            if (!v->is_number() || !std::isfinite(v->get<double>())) {
                fail(key, "must be a finite number");
                return;
            }
            const double d = v->get<double>();
            // Compared as stored (float), so a saved limit like 0.0001
            // (float 9.99999975e-05) loads again.
            if (!within(d, minimum, maximum)) {
                fail(key, "must be from " + limit(minimum) + " to " + limit(maximum));
                return;
            }
            out = static_cast<float>(d);
        }
    }

    template <typename V>
    void vec3(const char* key, V& out, double minimum, double maximum) {
        if (const Json* v = get(key, false)) {
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
                if (!within(xyz[i], minimum, maximum)) {
                    fail(key, "values must be from " + limit(minimum) + " to " + limit(maximum));
                    return;
                }
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

    void string(const char* key, std::string& out, bool required = false, usize max_bytes = 4096) {
        if (const Json* v = get(key, required)) {
            if (!v->is_string()) {
                fail(key, "must be a string");
                return;
            }
            if (v->get_ref<const std::string&>().size() > max_bytes) {
                fail(key, "must be at most " + std::to_string(max_bytes) + " bytes");
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
    void id(const char* key, u64& out, bool required, u64 maximum = k_max_entity_id) {
        if (const Json* v = get(key, required)) {
            if (!v->is_number_unsigned() || v->get<u64>() == 0 ||
                v->get<u64>() > maximum) {
                fail(key, "must be a whole number from 1 to " +
                              std::string(maximum == k_max_entity_id ? "2^53 - 1" : "2^53"));
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

    static bool within(double v, double minimum, double maximum) {
        const float f = static_cast<float>(v);
        return f >= static_cast<float>(minimum) && f <= static_cast<float>(maximum);
    }

    static std::string limit(double v) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", v);
        return text;
    }

private:
    const Json& m_object;
    std::string m_path;
    std::string& m_error;
    std::set<std::string, std::less<>> m_seen;
};

void read_lighting(const Json& json, Lighting& l, std::string& error) {
    Reader r(json, "lighting", error);
    r.number("sun_elevation_degrees", l.sun_elevation_degrees, -90, 90);
    r.number("sun_azimuth_degrees", l.sun_azimuth_degrees, -360, 360);
    r.vec3("sun_color", l.sun_color, 0, 1);
    r.number("sun_intensity", l.sun_intensity, 0, 100);
    r.vec3("sky_color", l.sky_color, 0, 1);
    r.vec3("ground_color", l.ground_color, 0, 1);
    r.number("ambient_intensity", l.ambient_intensity, 0, 100);
    r.number("exposure", l.exposure, 0.01, 100);
    r.finish();
}

void read_weather(const Json& json, std::optional<Weather>& out, std::string& error) {
    Reader r(json, "weather", error);
    Weather w;
    r.number("cloud_cover", w.cloud_cover, 0, 1);
    r.number("cloud_density", w.cloud_density, 0, 1);
    r.number("cloud_base", w.cloud_base, 100, 10000);
    r.number("cloud_thickness", w.cloud_thickness, 100, 10000);
    r.number("cloud_scale", w.cloud_scale, 0.25, 4);
    r.number("wind_direction_degrees", w.wind_direction_degrees, -360, 360);
    r.number("wind_speed", w.wind_speed, 0, 200);
    r.number("fog_density", w.fog_density, 0, 1);
    r.number("fog_height", w.fog_height, 1, 10000);
    r.number("haze", w.haze, 0, 10);
    r.number("precipitation", w.precipitation, 0, 1);
    r.number("temperature", w.temperature, -50, 50);
    r.number("wetness", w.wetness, 0, 1);
    r.number("snow_cover", w.snow_cover, 0, 1);
    r.finish();
    if (error.empty()) out = w;
}

// Shape, files, transform, material and decal of an entity or kit piece.
void read_look(Reader& r, const std::string& path, Shape& shape, std::string& mesh, std::string* kit,
               Transform& t, Material& material, DecalSettings& decal, std::string& error) {
    std::string name = to_string(shape);
    r.string("shape", name);
    if (r.ok()) {
        if (const auto parsed = shape_from_string(name); parsed && (kit != nullptr || *parsed != Shape::kit)) {
            shape = *parsed;
        } else {
            r.fail("shape", kit != nullptr ? "must be cube, sphere, plane, mesh, kit or decal"
                                           : "must be cube, sphere, plane, mesh or decal (kits do not nest)");
        }
    }
    r.string("mesh", mesh, false, k_max_mesh_path_bytes);
    if (r.ok() && shape == Shape::mesh && mesh.empty()) {
        r.fail("mesh", "is needed when the shape is mesh");
    }
    if (kit != nullptr) {
        const bool given = r.get("kit", false) != nullptr;
        r.string("kit", *kit, false, k_max_name_bytes);
        if (r.ok() && given && kit->empty()) {
            r.fail("kit", "must not be empty");
        }
        if (r.ok() && shape == Shape::kit && kit->empty()) {
            r.fail("kit", "is needed when the shape is kit");
        }
    }
    r.vec3("position", t.position, -1e8, 1e8);
    r.quat("rotation", t.rotation);
    r.vec3("scale", t.scale, 0.0001, 1e6);
    if (const Json* m = r.get("material", false); m != nullptr && r.ok()) {
        Reader mr(*m, path + ".material", error);
        mr.vec3("base_color", material.base_color, 0, 1);
        mr.number("roughness", material.roughness, 0, 1);
        mr.number("metallic", material.metallic, 0, 1);
        mr.vec3("emissive", material.emissive, 0, 1000);
        mr.finish();
    }
    if (const Json* d = r.get("decal", false); d != nullptr && r.ok()) {
        Reader dr(*d, path + ".decal", error);
        std::string mask = to_string(decal.shape);
        dr.string("shape", mask);
        if (dr.ok()) {
            if (const auto parsed = decal_shape_from_string(mask)) {
                decal.shape = *parsed;
            } else {
                dr.fail("shape", "must be disc, rectangle, ring, splat or cracks");
            }
        }
        dr.number("opacity", decal.opacity, 0, 1);
        dr.number("softness", decal.softness, 0, 1);
        u64 seed = u64{decal.seed} + 1; // id() wants 1 or more
        if (const Json* v = dr.get("seed", false); v != nullptr && dr.ok()) {
            if (!v->is_number_unsigned() || v->get<u64>() > 0xFFFF'FFFFull) {
                dr.fail("seed", "must be a whole number from 0 to 4294967295");
            } else {
                seed = v->get<u64>() + 1;
            }
        }
        decal.seed = static_cast<u32>(seed - 1);
        dr.finish();
    }
}

void read_entity(const Json& json, const std::string& path, Entity& e, std::string& error) {
    Reader r(json, path, error);
    r.id("id", e.id, true);
    r.string("name", e.name, false, k_max_name_bytes);
    r.boolean("visible", e.visible);
    read_look(r, path, e.shape, e.mesh, &e.kit, e.transform, e.material, e.decal, error);
    if (const Json* v = r.get("body", false); v != nullptr && r.ok()) {
        const std::optional<BodyKind> body = v->is_string() ? body_from_string(v->get<std::string>()) : std::nullopt;
        if (body) {
            e.body = *body;
        } else {
            r.fail("body", "must be fixed, dynamic or none");
        }
    }
    r.number("mass", e.mass, 0, k_max_body_mass);
    r.finish();
}

void read_kit(const Json& json, const std::string& path, Kit& k, std::string& error) {
    Reader r(json, path, error);
    r.string("name", k.name, true, k_max_name_bytes);
    if (r.ok() && k.name.empty()) {
        r.fail("name", "must not be empty");
    }
    if (const Json* pieces = r.get("pieces", true); pieces != nullptr && r.ok()) {
        if (!pieces->is_array() || pieces->empty() || pieces->size() > k_max_kit_pieces) {
            r.fail("pieces", "must be an array of 1 to " + std::to_string(k_max_kit_pieces) + " pieces");
        } else {
            for (usize i = 0; i < pieces->size() && error.empty(); ++i) {
                const std::string piece_path = path + ".pieces[" + std::to_string(i) + "]";
                KitPiece p;
                Reader pr((*pieces)[i], piece_path, error);
                pr.string("name", p.name, false, k_max_name_bytes);
                read_look(pr, piece_path, p.shape, p.mesh, nullptr, p.transform, p.material, p.decal, error);
                pr.finish();
                k.pieces.push_back(std::move(p));
            }
        }
    }
    r.boolean("breakable", k.breakable);
    r.number("strength", k.strength, 0, k_max_kit_strength);
    r.finish();
}

// A [x, z] pair of whole numbers in i32.
std::optional<CellCoord> read_cell(const Json& v) {
    if (!v.is_array() || v.size() != 2 || !v[0].is_number_integer() || !v[1].is_number_integer()) {
        return std::nullopt;
    }
    const i64 x = v[0].get<i64>();
    const i64 z = v[1].get<i64>();
    const i64 lo = std::numeric_limits<i32>::min();
    const i64 hi = std::numeric_limits<i32>::max();
    if (x < lo || x > hi || z < lo || z > hi) {
        return std::nullopt;
    }
    return CellCoord{static_cast<i32>(x), static_cast<i32>(z)};
}

// "samples" and "material" of a terrain (or a world's terrain settings).
void read_terrain_settings(Reader& r, const std::string& path, u32& samples, TerrainMaterial& material,
                           std::string& error) {
    if (const Json* v = r.get("samples", false); v != nullptr && r.ok()) {
        if (!v->is_number_unsigned() || !valid_terrain_samples(static_cast<u32>(std::min<u64>(v->get<u64>(), 1u << 20)))) {
            r.fail("samples", "must be 8, 16, 32, 64 or 128");
        } else {
            samples = v->get<u32>();
        }
    }
    if (const Json* m = r.get("material", false); m != nullptr && r.ok()) {
        Reader mr(*m, path + ".material", error);
        mr.vec3("base_color", material.base_color, 0, 1);
        mr.number("roughness", material.roughness, 0, 1);
        mr.finish();
    }
}

// A tile's heights under `key`: n rows of n numbers as text.
void read_heights(Reader& tr, const char* key, usize n, Terrain::Heights& heights) {
    const Json* rows = tr.get(key, true);
    if (rows == nullptr || !tr.ok()) {
        return;
    }
    if (!rows->is_array() || rows->size() != n) {
        tr.fail(key, "must be " + std::to_string(n) + " rows (samples)");
        return;
    }
    heights.reserve(n * n);
    for (usize j = 0; tr.ok() && j < n; ++j) {
        const Json& row = (*rows)[j];
        const std::string where = std::string(key) + "[" + std::to_string(j) + "]";
        if (!row.is_string()) {
            tr.fail(where, "must be a string of numbers");
            break;
        }
        const std::string& text = row.get_ref<const std::string&>();
        const char* at = text.data();
        const char* end = text.data() + text.size();
        usize count = 0;
        while (tr.ok()) {
            while (at < end && *at == ' ') ++at;
            if (at == end) break;
            float h = 0.0f;
            const auto [next, ec] = std::from_chars(at, end, h);
            if (ec != std::errc{} || (next < end && *next != ' ') || !std::isfinite(h) ||
                std::fabs(h) > k_max_terrain_height) {
                tr.fail(where, "must hold numbers from -10000 to 10000, one space apart");
                break;
            }
            heights.push_back(h);
            ++count;
            at = next;
        }
        if (tr.ok() && count != n) {
            tr.fail(where, "must hold " + std::to_string(n) + " heights (samples)");
        }
    }
}

// A terrain: its sample spacing, material and tiles (heights as rows of text).
void read_terrain(const Json& json, Terrain& out, std::string& error) {
    Reader r(json, "terrain", error);
    Terrain t;
    u32 samples = t.samples();
    read_terrain_settings(r, "terrain", samples, t.material, error);
    t.set_samples(samples);
    const Json* tiles = r.get("tiles", false);
    if (tiles != nullptr && r.ok() && !tiles->is_array()) {
        r.fail("tiles", "must be an array");
    }
    const usize n = t.samples();
    for (usize k = 0; tiles != nullptr && r.ok() && error.empty() && k < tiles->size(); ++k) {
        const std::string path = "terrain.tiles[" + std::to_string(k) + "]";
        Reader tr((*tiles)[k], path, error);
        std::optional<CellCoord> cell;
        if (const Json* c = tr.get("cell", true); c != nullptr && tr.ok()) {
            cell = read_cell(*c);
            if (!cell) {
                tr.fail("cell", "must be [x, z], whole numbers");
            } else if (t.find(*cell) != nullptr) {
                tr.fail("cell", "is listed twice");
            }
        }
        Terrain::Heights heights;
        read_heights(tr, "heights", n, heights);
        tr.finish();
        if (tr.ok() && error.empty()) {
            t.set(*cell, std::move(heights));
        }
    }
    r.finish();
    if (error.empty()) {
        out = std::move(t);
    }
}

// The JSON in `text`, or why it is not JSON.
std::optional<LoadError> parse(std::string_view text, Json& json) {
    try {
        json = Json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        return LoadError{std::string("not valid JSON: ") + e.what()};
    }
    return std::nullopt;
}

// "format" and "version" (1) of a giant text file.
void read_header(Reader& root, const char* format_name, const char* what) {
    std::string format;
    root.string("format", format, true);
    if (root.ok() && format != format_name) {
        root.fail("format", std::string("must be \"") + format_name + "\"");
    }
    if (const Json* version = root.get("version", true); version != nullptr && root.ok()) {
        if (!version->is_number_integer() || version->get<i64>() != k_version) {
            root.fail("version", std::string("must be 1 (this editor reads version 1 ") + what + ")");
        }
    }
}

void read_kits(Reader& root, std::vector<Kit>& out, std::string& error) {
    const Json* kits = root.get("kits", false);
    if (kits == nullptr || !root.ok()) {
        return;
    }
    if (!kits->is_array()) {
        root.fail("kits", "must be an array");
        return;
    }
    for (usize i = 0; i < kits->size() && error.empty(); ++i) {
        const std::string path = "kits[" + std::to_string(i) + "]";
        Kit kit;
        read_kit((*kits)[i], path, kit, error);
        if (error.empty() &&
            std::any_of(out.begin(), out.end(), [&](const Kit& k) { return k.name == kit.name; })) {
            error = path + ".name \"" + kit.name + "\" is used twice";
        }
        if (error.empty()) {
            out.push_back(std::move(kit));
        }
    }
}

// A pair of numbers [a, b], each within +-limit.
bool read_pair(const Json& v, double limit, double& a, double& b) {
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) return false;
    a = v[0].get<double>();
    b = v[1].get<double>();
    return std::isfinite(a) && std::isfinite(b) && std::fabs(a) <= limit && std::fabs(b) <= limit;
}

void read_scatter_layer(const Json& json, const std::string& path, ScatterLayer& l, std::string& error) {
    Reader r(json, path, error);
    r.string("name", l.name, true, k_max_name_bytes);
    r.number("spacing", l.spacing, k_min_scatter_spacing, k_max_scatter_spacing);
    r.number("density", l.density, 0, 1);
    r.number("jitter", l.jitter, 0, 1);
    r.number("clump_size", l.clump_size, 0, 100000);
    r.number("clumping", l.clumping, 0, 1);
    r.number("height_min", l.height_min, -k_max_terrain_height, k_max_terrain_height);
    r.number("height_max", l.height_max, -k_max_terrain_height, k_max_terrain_height);
    r.number("slope_min", l.slope_min, 0, 90);
    r.number("slope_max", l.slope_max, 0, 90);
    r.boolean("visible", l.visible);
    if (const Json* v = r.get("seed", false); v != nullptr && r.ok()) {
        if (!v->is_number_unsigned() || v->get<u64>() > 0xFFFF'FFFFull) {
            r.fail("seed", "must be a whole number from 0 to 4294967295");
        } else {
            l.seed = static_cast<u32>(v->get<u64>());
        }
    }
    if (const Json* a = r.get("area", false); a != nullptr && r.ok()) {
        Reader ar(*a, path + ".area", error);
        ScatterArea area;
        const Json* from = ar.get("from", true);
        const Json* to = ar.get("to", true);
        if (ar.ok() && (!read_pair(*from, 1e8, area.from_x, area.from_z) || !read_pair(*to, 1e8, area.to_x, area.to_z))) {
            ar.fail("", "from and to must be [x, z], within 1e8 metres");
        }
        ar.finish();
        l.area = area;
    }
    if (const Json* items = r.get("items", true); items != nullptr && r.ok()) {
        if (!items->is_array() || items->size() > k_max_scatter_items) {
            r.fail("items", "must be an array of 1 to " + std::to_string(k_max_scatter_items) + " items");
        }
        for (usize k = 0; r.ok() && error.empty() && k < items->size(); ++k) {
            const std::string at = path + ".items[" + std::to_string(k) + "]";
            Reader ir((*items)[k], at, error);
            ScatterItem item;
            std::string shape = to_string(item.shape);
            ir.string("shape", shape);
            if (ir.ok()) {
                const auto parsed = shape_from_string(shape);
                if (parsed && *parsed != Shape::decal) {
                    item.shape = *parsed;
                } else {
                    ir.fail("shape", "must be cube, sphere, plane, mesh or kit");
                }
            }
            ir.string("mesh", item.mesh, false, k_max_mesh_path_bytes);
            ir.string("kit", item.kit, false, k_max_name_bytes);
            if (const Json* m = ir.get("material", false); m != nullptr && ir.ok()) {
                Reader mr(*m, at + ".material", error);
                mr.vec3("base_color", item.material.base_color, 0, 1);
                mr.number("roughness", item.material.roughness, 0, 1);
                mr.number("metallic", item.material.metallic, 0, 1);
                mr.vec3("emissive", item.material.emissive, 0, 1000);
                mr.finish();
            }
            ir.number("weight", item.weight, 1e-6, 1e6);
            if (const Json* sc = ir.get("scale", false); sc != nullptr && ir.ok()) {
                double lo = 0.0, hi = 0.0;
                if (!read_pair(*sc, 1e4, lo, hi)) {
                    ir.fail("scale", "must be [min, max]");
                } else {
                    item.scale_min = static_cast<float>(lo);
                    item.scale_max = static_cast<float>(hi);
                }
            }
            ir.number("align", item.align, 0, 1);
            ir.number("sink", item.sink, -1000, 1000);
            if (const Json* tr = ir.get("tree", false); tr != nullptr && ir.ok() && !tr->is_null()) {
                Reader tree_reader(*tr, at + ".tree", error);
                ScatterTree tree;
                tree_reader.number("trunk_radius", tree.trunk_radius, 0.01, 100);
                tree_reader.number("height", tree.height, 0.1, 1000);
                tree_reader.number("strength", tree.strength, 0, 1e9);
                tree_reader.finish();
                item.tree = tree;
            }
            ir.finish();
            l.items.push_back(std::move(item));
        }
    }
    r.finish();
    if (error.empty()) {
        if (const auto bad = check_scatter_layer(l)) error = path + ": " + *bad;
    }
}

// After read_kits: a kit item names one of `kits`.
void read_scatter(Reader& root, const std::vector<Kit>& kits, std::vector<ScatterLayer>& out, std::string& error) {
    const Json* layers = root.get("scatter", false);
    if (layers == nullptr || !root.ok()) return;
    if (!layers->is_array() || layers->size() > k_max_scatter_layers) {
        root.fail("scatter", "must be an array of at most " + std::to_string(k_max_scatter_layers) + " layers");
        return;
    }
    for (usize i = 0; i < layers->size() && error.empty(); ++i) {
        const std::string path = "scatter[" + std::to_string(i) + "]";
        ScatterLayer layer;
        read_scatter_layer((*layers)[i], path, layer, error);
        if (error.empty() &&
            std::any_of(out.begin(), out.end(), [&](const ScatterLayer& l) { return l.name == layer.name; })) {
            error = path + ".name \"" + layer.name + "\" is used twice";
        }
        for (usize k = 0; error.empty() && k < layer.items.size(); ++k) {
            const ScatterItem& item = layer.items[k];
            if (item.shape == Shape::kit &&
                std::none_of(kits.begin(), kits.end(), [&](const Kit& kit) { return kit.name == item.kit; })) {
                error = path + ".items[" + std::to_string(k) + "].kit \"" + item.kit + "\" is not one of the kits";
            }
        }
        if (error.empty()) out.push_back(std::move(layer));
    }
}

// Calls `take` with each entity of the "entities" array, read strictly;
// `take` returns an error to stop.
template <typename Take>
void read_entities(Reader& root, std::string& error, Take take) {
    const Json* entities = root.get("entities", false);
    if (entities == nullptr || !root.ok()) {
        return;
    }
    if (!entities->is_array()) {
        root.fail("entities", "must be an array");
        return;
    }
    for (usize i = 0; i < entities->size() && error.empty(); ++i) {
        const std::string path = "entities[" + std::to_string(i) + "]";
        Entity entity;
        read_entity((*entities)[i], path, entity, error);
        if (error.empty()) {
            error = take(path, std::move(entity));
        }
    }
}

} // namespace

std::optional<LoadError> load_scene(std::string_view text, Scene& out) {
    Json json;
    if (auto bad = parse(text, json)) {
        return bad;
    }
    std::string error;
    Reader root(json, "", error);
    read_header(root, k_format, "scenes");
    u64 next_id = 1;
    root.id("next_id", next_id, false, k_max_entity_id + 1); // one past the last id

    Scene scene;
    if (const Json* lighting = root.get("lighting", false); lighting != nullptr && root.ok()) {
        read_lighting(*lighting, scene.lighting, error);
    }
    if (const Json* weather = root.get("weather", false); weather != nullptr && root.ok()) {
        read_weather(*weather, scene.weather, error);
    }
    read_kits(root, scene.kits, error);
    read_scatter(root, scene.kits, scene.scatter, error);
    if (const Json* terrain = root.get("terrain", false); terrain != nullptr && root.ok()) {
        read_terrain(*terrain, scene.terrain, error);
    }
    read_entities(root, error, [&](const std::string& path, Entity entity) -> std::string {
        if (scene.find(entity.id) != nullptr) {
            return path + ".id " + std::to_string(entity.id) + " is used twice";
        }
        if (entity.shape == Shape::kit && scene.find_kit(entity.kit) == nullptr) {
            return path + ".kit \"" + entity.kit + "\" is not one of the scene's kits";
        }
        scene.add(std::move(entity));
        return {};
    });
    root.finish();
    if (!error.empty()) {
        return LoadError{std::move(error)};
    }
    // A hand-edited next_id that is too low must not hand out used ids.
    scene.reserve_ids_below(next_id);
    out = std::move(scene);
    return std::nullopt;
}

// --- Worlds -----------------------------------------------------------------------

namespace {

constexpr const char* k_world_format = "giant-world";
constexpr const char* k_cell_format = "giant-cell";

Json cell_json(CellCoord c) { return Json::array({c.x, c.z}); }


} // namespace

std::string cell_file_name(CellCoord cell) {
    return std::to_string(cell.x) + "_" + std::to_string(cell.z) + ".cell.json";
}

std::string save_world_index(const WorldIndex& world) {
    Json out;
    out["format"] = k_world_format;
    out["version"] = k_version;
    out["next_id"] = world.next_id;
    out["lighting"] = lighting_json(world.lighting);
    if (!world.kits.empty()) {
        out["kits"] = kits_json(world.kits);
    }
    if (!world.scatter.empty()) {
        out["scatter"] = scatter_json(world.scatter);
    }
    if (world.weather) {
        out["weather"] = weather_json(*world.weather);
    }
    std::vector<std::string> meshes = world.meshes;
    std::sort(meshes.begin(), meshes.end());
    meshes.erase(std::unique(meshes.begin(), meshes.end()), meshes.end());
    out["meshes"] = meshes;
    std::vector<CellCoord> cells = world.cells;
    const auto by_zx = [](CellCoord a, CellCoord b) { return a.z != b.z ? a.z < b.z : a.x < b.x; };
    std::sort(cells.begin(), cells.end(), by_zx);
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    Json list = Json::array();
    for (const CellCoord c : cells) {
        list.push_back(cell_json(c));
    }
    out["cells"] = std::move(list);
    std::vector<CellCoord> ground = world.terrain_cells;
    std::sort(ground.begin(), ground.end(), by_zx);
    ground.erase(std::unique(ground.begin(), ground.end()), ground.end());
    const auto sorted_list = [&](std::vector<CellCoord> owed) {
        std::sort(owed.begin(), owed.end(), by_zx);
        owed.erase(std::unique(owed.begin(), owed.end()), owed.end());
        Json array = Json::array();
        for (const CellCoord c : owed) array.push_back(cell_json(c));
        return array;
    };
    if (!ground.empty() || world.terrain_samples != k_default_terrain_samples ||
        !(world.terrain_material == TerrainMaterial{}) || !world.far_owed.empty() || !world.far_owed_2.empty()) {
        Json tiles = Json::array();
        for (const CellCoord c : ground) {
            tiles.push_back(cell_json(c));
        }
        out["terrain"] = {{"samples", world.terrain_samples},
                          {"material", terrain_material_json(world.terrain_material)},
                          {"cells", std::move(tiles)}};
        if (!world.far_owed.empty()) out["terrain"]["far_owed"] = sorted_list(world.far_owed);
        if (!world.far_owed_2.empty()) out["terrain"]["far_owed_2"] = sorted_list(world.far_owed_2);
    }
    return out.dump(2) + "\n";
}

std::optional<LoadError> load_world_index(std::string_view text, WorldIndex& out) {
    Json json;
    if (auto bad = parse(text, json)) {
        return bad;
    }
    std::string error;
    Reader root(json, "", error);
    read_header(root, k_world_format, "worlds");
    WorldIndex world;
    root.id("next_id", world.next_id, false, k_max_entity_id + 1);
    if (const Json* lighting = root.get("lighting", false); lighting != nullptr && root.ok()) {
        read_lighting(*lighting, world.lighting, error);
    }
    if (const Json* weather = root.get("weather", false); weather != nullptr && root.ok()) {
        read_weather(*weather, world.weather, error);
    }
    read_kits(root, world.kits, error);
    read_scatter(root, world.kits, world.scatter, error);
    if (const Json* meshes = root.get("meshes", false); meshes != nullptr && root.ok()) {
        if (!meshes->is_array()) {
            root.fail("meshes", "must be an array of paths");
        }
        for (usize i = 0; root.ok() && i < meshes->size(); ++i) {
            const Json& m = (*meshes)[i];
            if (!m.is_string() || m.get_ref<const std::string&>().empty() ||
                m.get_ref<const std::string&>().size() > k_max_mesh_path_bytes) {
                root.fail("meshes[" + std::to_string(i) + "]", "must be a path of 1 to " +
                                                                  std::to_string(k_max_mesh_path_bytes) + " bytes");
            } else {
                world.meshes.push_back(m.get<std::string>());
            }
        }
    }
    if (const Json* cells = root.get("cells", true); cells != nullptr && root.ok()) {
        if (!cells->is_array()) {
            root.fail("cells", "must be an array of [x, z] cells");
        }
        std::unordered_map<CellCoord, bool, CellCoordHash> seen;
        for (usize i = 0; root.ok() && i < cells->size(); ++i) {
            const std::string key = "cells[" + std::to_string(i) + "]";
            const auto c = read_cell((*cells)[i]);
            if (!c) {
                root.fail(key, "must be [x, z], two whole numbers");
            } else if (!seen.emplace(*c, true).second) {
                root.fail(key, "is listed twice");
            } else {
                world.cells.push_back(*c);
            }
        }
    }
    if (const Json* terrain = root.get("terrain", false); terrain != nullptr && root.ok()) {
        Reader tr(*terrain, "terrain", error);
        read_terrain_settings(tr, "terrain", world.terrain_samples, world.terrain_material, error);
        std::unordered_map<CellCoord, bool, CellCoordHash> listed;
        for (const CellCoord c : world.cells) listed.emplace(c, true);
        if (const Json* cells = tr.get("cells", false); cells != nullptr && tr.ok()) {
            if (!cells->is_array()) {
                tr.fail("cells", "must be an array of [x, z] cells");
            }
            std::unordered_map<CellCoord, bool, CellCoordHash> seen;
            for (usize i = 0; tr.ok() && i < cells->size(); ++i) {
                const std::string key = "cells[" + std::to_string(i) + "]";
                const auto c = read_cell((*cells)[i]);
                if (!c) {
                    tr.fail(key, "must be [x, z], two whole numbers");
                } else if (!seen.emplace(*c, true).second) {
                    tr.fail(key, "is listed twice");
                } else if (!listed.contains(*c)) {
                    tr.fail(key, "is not in the world's cells (a cell with ground has a file)");
                } else {
                    world.terrain_cells.push_back(*c);
                }
            }
        }
        for (const auto& [key, list] : {std::pair<const char*, std::vector<CellCoord>*>{"far_owed", &world.far_owed},
                                        {"far_owed_2", &world.far_owed_2}}) {
            const Json* owed = tr.get(key, false);
            if (owed == nullptr || !tr.ok()) continue;
            if (!owed->is_array()) {
                tr.fail(key, "must be an array of [x, z] cells");
                continue;
            }
            for (usize i = 0; tr.ok() && i < owed->size(); ++i) {
                const auto c = read_cell((*owed)[i]);
                if (!c) {
                    tr.fail(std::string(key) + "[" + std::to_string(i) + "]", "must be [x, z], two whole numbers");
                } else {
                    list->push_back(*c);
                }
            }
        }
        tr.finish();
    }
    root.finish();
    if (!error.empty()) {
        return LoadError{std::move(error)};
    }
    out = std::move(world);
    return std::nullopt;
}

std::string save_cell(CellCoord cell, std::span<const Entity> entities, const Terrain::Heights* heights) {
    std::vector<const Entity*> sorted;
    sorted.reserve(entities.size());
    for (const Entity& e : entities) {
        sorted.push_back(&e);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Entity* a, const Entity* b) { return a->id < b->id; });
    Json out;
    out["format"] = k_cell_format;
    out["version"] = k_version;
    out["cell"] = cell_json(cell);
    Json list = Json::array();
    for (const Entity* e : sorted) {
        list.push_back(entity_json(*e));
    }
    out["entities"] = std::move(list);
    if (heights != nullptr) {
        const auto n = static_cast<usize>(std::lround(std::sqrt(static_cast<double>(heights->size()))));
        GIANT_ASSERT(n * n == heights->size() && valid_terrain_samples(static_cast<u32>(n)),
                     "a terrain tile is samples x samples heights");
        out["heights"] = heights_json(*heights, n);
    }
    return out.dump(2) + "\n";
}

std::optional<LoadError> load_cell(std::string_view text, CellCoord cell, std::vector<Entity>& out) {
    std::optional<Terrain::Heights> heights;
    return load_cell(text, cell, 0, out, heights);
}

std::optional<LoadError> load_cell(std::string_view text, CellCoord cell, u32 terrain_samples,
                                   std::vector<Entity>& out, std::optional<Terrain::Heights>& heights_out) {
    GIANT_ASSERT(terrain_samples == 0 || valid_terrain_samples(terrain_samples), "terrain samples must be valid");
    Json json;
    if (auto bad = parse(text, json)) {
        return bad;
    }
    std::string error;
    Reader root(json, "", error);
    read_header(root, k_cell_format, "cells");
    if (const Json* c = root.get("cell", true); c != nullptr && root.ok()) {
        const auto read = read_cell(*c);
        if (!read || !(*read == cell)) {
            root.fail("cell", "must be [" + std::to_string(cell.x) + ", " + std::to_string(cell.z) +
                                  "], the cell its file name says");
        }
    }
    std::vector<Entity> entities;
    std::unordered_map<EntityId, bool> ids;
    read_entities(root, error, [&](const std::string& path, Entity entity) -> std::string {
        if (!ids.emplace(entity.id, true).second) {
            return path + ".id " + std::to_string(entity.id) + " is used twice";
        }
        if (!(cell_of(entity.transform.position) == cell)) {
            return path + ".position is not in cell [" + std::to_string(cell.x) + ", " +
                   std::to_string(cell.z) + "] (cells are " + std::to_string(static_cast<int>(k_cell_size)) +
                   " m on x and z)";
        }
        entities.push_back(std::move(entity));
        return {};
    });
    std::optional<Terrain::Heights> heights;
    if (root.ok() && error.empty() && root.get("heights", false) != nullptr) {
        if (terrain_samples == 0) {
            root.fail("heights", "is a terrain tile, and this cell is read without terrain");
        } else {
            heights.emplace();
            read_heights(root, "heights", terrain_samples, *heights);
        }
    }
    root.finish();
    if (!error.empty()) {
        return LoadError{std::move(error)};
    }
    out = std::move(entities);
    heights_out = std::move(heights);
    return std::nullopt;
}

// --- Far ground chunks -----------------------------------------------------------

std::string far_chunk_file_name(CellCoord chunk) {
    return std::to_string(chunk.x) + "_" + std::to_string(chunk.z) + ".ground.json";
}

std::string save_far_chunk(u32 level, CellCoord chunk, const Terrain::Heights& heights) {
    const usize n = k_far_samples;
    GIANT_ASSERT(heights.size() == n * n, "a far ground chunk is k_far_samples squared heights");
    Json out;
    out["format"] = "giant-ground";
    out["version"] = k_version;
    out["level"] = level;
    out["chunk"] = cell_json(chunk);
    Json rows = Json::array();
    char text[32];
    for (usize j = 0; j < n; ++j) {
        std::string row;
        row.reserve(n * 8);
        for (usize i = 0; i < n; ++i) {
            if (i > 0) row += ' ';
            const float h = heights[j * n + i];
            if (std::isnan(h)) {
                row += '-';
                continue;
            }
            const auto [end, ec] = std::to_chars(text, text + sizeof(text), h);
            GIANT_ASSERT(ec == std::errc{}, "a height did not fit its text");
            row.append(text, end);
        }
        rows.push_back(std::move(row));
    }
    out["heights"] = std::move(rows);
    return out.dump(2) + "\n";
}

std::optional<LoadError> load_far_chunk(std::string_view text, u32 level, CellCoord chunk, Terrain::Heights& out) {
    Json json;
    if (auto bad = parse(text, json)) {
        return bad;
    }
    std::string error;
    Reader root(json, "", error);
    read_header(root, "giant-ground", "far ground chunks");
    if (const Json* l = root.get("level", true); l != nullptr && root.ok()) {
        if (!l->is_number_unsigned() || l->get<u64>() != level) {
            root.fail("level", "must be " + std::to_string(level) + ", the level its folder says");
        }
    }
    if (const Json* c = root.get("chunk", true); c != nullptr && root.ok()) {
        const auto read = read_cell(*c);
        if (!read || !(*read == chunk)) {
            root.fail("chunk", "must be [" + std::to_string(chunk.x) + ", " + std::to_string(chunk.z) +
                                   "], the chunk its file name says");
        }
    }
    const usize n = k_far_samples;
    Terrain::Heights heights;
    if (const Json* rows = root.get("heights", true); rows != nullptr && root.ok()) {
        if (!rows->is_array() || rows->size() != n) {
            root.fail("heights", "must be " + std::to_string(n) + " rows");
        }
        heights.reserve(n * n);
        for (usize j = 0; root.ok() && j < n; ++j) {
            const Json& row = (*rows)[j];
            const std::string where = "heights[" + std::to_string(j) + "]";
            if (!row.is_string()) {
                root.fail(where, "must be a string of numbers");
                break;
            }
            const std::string& t = row.get_ref<const std::string&>();
            const char* at = t.data();
            const char* end = t.data() + t.size();
            usize count = 0;
            while (root.ok()) {
                while (at < end && *at == ' ') ++at;
                if (at == end) break;
                float h = std::numeric_limits<float>::quiet_NaN();
                const char* next = at + 1;
                if (*at != '-' || (next < end && *next != ' ')) {
                    const auto [after, ec] = std::from_chars(at, end, h);
                    next = after;
                    if (ec != std::errc{} || (next < end && *next != ' ') || !std::isfinite(h) ||
                        std::fabs(h) > k_max_terrain_height) {
                        root.fail(where, "must hold numbers from -10000 to 10000 or -, one space apart");
                        break;
                    }
                }
                heights.push_back(h);
                ++count;
                at = next;
            }
            if (root.ok() && count != n) {
                root.fail(where, "must hold " + std::to_string(n) + " heights");
            }
        }
    }
    root.finish();
    if (!error.empty()) {
        return LoadError{std::move(error)};
    }
    out = std::move(heights);
    return std::nullopt;
}

} // namespace giant::world
