// Terrain commands: making, shaping and reading the ground (world::Terrain).
// Every change is undoable; the tiles' meshes follow on worker threads
// (TerrainMeshes), so get_terrain says when they are all drawn.
#include "editor_commands.h"
#include "terrain_meshes.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
using world::CellCoord;
using world::Terrain;

// Tiles one command may make or remove (64 x 64 cells: 4 km a side), and
// heights it may make (4096 tiles of 64 x 64; 1024 of 128 x 128): a
// command runs between frames, so this bounds its hitch.
constexpr usize k_max_tiles_per_command = 4096;
constexpr usize k_max_samples_per_command = 4096 * 64 * 64;
// Heights the whole terrain may hold (256 MB; the meshes take about as
// much again) until terrain streams with the world.
constexpr usize k_max_terrain_samples = 4 * k_max_samples_per_command;
constexpr usize k_max_points = 4096;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "terrain command could not be registered");
}

// Tiles as they were (or will be): none means no tile.
using TileStates = std::vector<std::pair<CellCoord, std::optional<Terrain::Heights>>>;

TileStates snapshot(const Terrain& t, const std::vector<CellCoord>& cells) {
    TileStates out;
    out.reserve(cells.size());
    for (const CellCoord c : cells) {
        const Terrain::Heights* h = t.find(c);
        out.emplace_back(c, h != nullptr ? std::optional<Terrain::Heights>(*h) : std::nullopt);
    }
    return out;
}

void apply(Terrain& t, const TileStates& states) {
    for (const auto& [cell, heights] : states) {
        if (heights) {
            t.set(cell, *heights);
        } else {
            t.remove(cell);
        }
    }
}

// Records `before` -> `after` (with the sample spacing each side had).
void record(EditorState& state, CommandContext& context, std::shared_ptr<const TileStates> before,
            std::shared_ptr<const TileStates> after, u32 samples_before, u32 samples_after) {
    context.record(scene_change(
        state,
        [&state, before, samples_before] {
            apply(state.scene.terrain, *before);
            state.scene.terrain.set_samples(samples_before);
        },
        [&state, after, samples_after] {
            // Spacing first: it can change only while there are no tiles,
            // which is how the change was made.
            state.scene.terrain.set_samples(samples_after);
            apply(state.scene.terrain, *after);
        }));
}

// The cells an area of ground touches: [from, to] in metres, x and z.
struct Area {
    CellCoord first;
    CellCoord last;
    usize count() const {
        return static_cast<usize>(i64{last.x} - first.x + 1) * static_cast<usize>(i64{last.z} - first.z + 1);
    }
};

std::optional<Area> area_of(const Json& from, const Json& to, std::string& error) {
    const double x0 = std::min(from[0].get<double>(), to[0].get<double>());
    const double x1 = std::max(from[0].get<double>(), to[0].get<double>());
    const double z0 = std::min(from[1].get<double>(), to[1].get<double>());
    const double z1 = std::max(from[1].get<double>(), to[1].get<double>());
    Area a{world::cell_of({x0, 0.0, z0}), world::cell_of({x1, 0.0, z1})};
    // A `to` on a cell's edge does not reach into the next cell.
    if (x1 > x0 && x1 == a.last.x * world::k_cell_size) --a.last.x;
    if (z1 > z0 && z1 == a.last.z * world::k_cell_size) --a.last.z;
    if (a.count() > k_max_tiles_per_command) {
        error = "the area covers " + std::to_string(a.count()) + " cells of 64 m; at most " +
                std::to_string(k_max_tiles_per_command) + " a command (4 km by 4 km): split it";
        return std::nullopt;
    }
    return a;
}

std::vector<CellCoord> cells_in(const Area& a) {
    std::vector<CellCoord> out;
    out.reserve(a.count());
    for (i32 z = a.first.z;; ++z) {
        for (i32 x = a.first.x;; ++x) {
            out.push_back({x, z});
            if (x == a.last.x) break;
        }
        if (z == a.last.z) break;
    }
    return out;
}

Json cell_json(CellCoord c) { return Json::array({c.x, c.z}); }

Json point_schema(const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", -1e8}, {"maximum", 1e8}}},
            {"minItems", 2},
            {"maxItems", 2},
            {"description", description}};
}

Json number(double minimum, double maximum, const char* description) {
    return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}};
}

Json hills_schema() {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"base", number(-5000, 5000, "Average height, metres. Default 0.")},
              {"amplitude", number(0, 5000, "The most the hills rise above or dip below base, metres. Default 20.")},
              {"size", number(1, 100000, "Width of the biggest hills, metres. Default 400.")},
              {"octaves", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8},
                           {"description", "Layers of detail, each half as big and high. Default 4."}}},
              {"seed", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0},
                        {"description", "Varies the hills. Default 0."}}}}},
            {"description", "Rolling hills from fractal noise (the same settings give the same ground "
                            "anywhere). Left out: flat ground at `height`."}};
}

Json material_json(const world::TerrainMaterial& m) {
    return {{"base_color", Json::array({m.base_color.x, m.base_color.y, m.base_color.z})},
            {"roughness", m.roughness}};
}

// Lowest and highest height of the tiles in `cells` that exist.
Json height_range(const Terrain& t, const std::vector<CellCoord>& cells) {
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (const CellCoord c : cells) {
        if (const Terrain::Heights* h = t.find(c)) {
            const auto [a, b] = std::minmax_element(h->begin(), h->end());
            lo = std::min(lo, *a);
            hi = std::max(hi, *b);
        }
    }
    if (lo > hi) return nullptr;
    return {{"lowest", lo}, {"highest", hi}};
}

} // namespace

void register_terrain_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"create_terrain",
         "Makes ground: a height field tile for every 64 m cell the area [from, to] (x, z "
         "metres) touches, replacing tiles already there. Flat at `height`, or rolling `hills`. "
         "Tiles join their neighbours seamlessly; they draw at every distance, cast shadows "
         "and light the scene (GI) once their meshes are built (get_terrain: building). "
         "`samples` (per tile side: 8, 16, 32, 64 or 128, that is 8 m to 0.5 m apart; default "
         "64, 1 m) can only change while there is no terrain. At most 4096 tiles a command. "
         "Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"from", "to"}},
              {"properties",
               {{"from", point_schema("[x, z], metres: one corner of the area.")},
                {"to", point_schema("[x, z], metres: the opposite corner.")},
                {"height", number(-10000, 10000, "Flat ground's height, metres (without hills). Default 0.")},
                {"hills", hills_schema()},
                {"samples", {{"type", "integer"}, {"enum", {8, 16, 32, 64, 128}},
                             {"description", "Height samples per tile side."}}}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Terrain& t = state.scene.terrain;
             std::string error;
             const std::optional<Area> area = area_of(args["from"], args["to"], error);
             if (!area) return CommandResult::failure(error);
             const u32 samples_before = t.samples();
             u32 samples = args.value("samples", samples_before);
             if (samples != samples_before && !t.empty()) {
                 return CommandResult::failure("samples is " + std::to_string(samples_before) +
                                               " and can change only while there is no terrain "
                                               "(remove_terrain with all first)");
             }
             std::optional<world::HillsSettings> hills;
             if (args.contains("hills")) {
                 const Json& h = args["hills"];
                 world::HillsSettings s;
                 s.base = h.value("base", s.base);
                 s.amplitude = h.value("amplitude", s.amplitude);
                 s.size = h.value("size", s.size);
                 s.octaves = h.value("octaves", s.octaves);
                 s.seed = h.value("seed", s.seed);
                 hills = s;
             }
             const float flat = args.value("height", 0.0f);
             const std::vector<CellCoord> cells = cells_in(*area);
             const usize n = samples;
             usize added = 0;
             for (const CellCoord c : cells) added += t.find(c) == nullptr ? usize{1} : usize{0};
             if (cells.size() * n * n > k_max_samples_per_command) {
                 return CommandResult::failure(
                     "the area needs " + std::to_string(cells.size() * n * n) + " heights; at most " +
                     std::to_string(k_max_samples_per_command) + " a command (" +
                     std::to_string(k_max_samples_per_command / (n * n)) + " tiles of " + std::to_string(n) +
                     " samples): split it");
             }
             if ((t.size() + added) * n * n > k_max_terrain_samples) {
                 return CommandResult::failure("the terrain would hold " + std::to_string(t.size() + added) +
                                               " tiles; at most " + std::to_string(k_max_terrain_samples / (n * n)) +
                                               " of " + std::to_string(n) + " samples until terrain streams");
             }
             auto before = std::make_shared<const TileStates>(snapshot(t, cells));
             t.set_samples(samples);
             const double spacing = t.spacing();
             // Made in parallel (on the terrain's workers and this thread), set in order.
             std::vector<Terrain::Heights> made(cells.size());
             const auto make = [&](usize begin, usize end) {
                 for (usize k = begin; k < end; ++k) {
                     const CellCoord c = cells[k];
                     Terrain::Heights& heights = made[k];
                     heights.assign(n * n, flat);
                     if (!hills) continue;
                     for (usize j = 0; j < n; ++j) {
                         for (usize i = 0; i < n; ++i) {
                             const double x = c.x * world::k_cell_size + static_cast<double>(i) * spacing;
                             const double z = c.z * world::k_cell_size + static_cast<double>(j) * spacing;
                             heights[j * n + i] = world::hills_height(*hills, x, z);
                         }
                     }
                 }
             };
             if (state.terrain_meshes != nullptr && hills) {
                 state.terrain_meshes->jobs().parallel_for(cells.size(), 16, make);
             } else {
                 make(0, cells.size());
             }
             for (usize k = 0; k < cells.size(); ++k) t.set(cells[k], std::move(made[k]));
             auto after = std::make_shared<const TileStates>(snapshot(t, cells));
             record(state, context, std::move(before), std::move(after), samples_before, t.samples());
             Json out = {{"tiles", cells.size()},
                         {"first_cell", cell_json(area->first)},
                         {"last_cell", cell_json(area->last)},
                         {"samples", t.samples()},
                         {"spacing", t.spacing()},
                         {"terrain_tiles", t.size()}};
             out["heights"] = height_range(t, cells);
             return CommandResult::success(std::move(out));
         }});

    add(registry,
        {"remove_terrain",
         "Removes the terrain tiles of every 64 m cell the area [from, to] (x, z metres) "
         "touches, or every tile with all. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"from", point_schema("[x, z], metres: one corner of the area.")},
                {"to", point_schema("[x, z], metres: the opposite corner.")},
                {"all", {{"type", "boolean"}, {"description", "Every tile (from and to are not needed)."}}}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Terrain& t = state.scene.terrain;
             std::vector<CellCoord> cells;
             if (args.value("all", false)) {
                 for (const auto& [c, h] : t.tiles()) cells.push_back(c);
             } else if (args.contains("from") && args.contains("to")) {
                 std::string error;
                 const std::optional<Area> area = area_of(args["from"], args["to"], error);
                 if (!area) return CommandResult::failure(error);
                 for (const CellCoord c : cells_in(*area)) {
                     if (t.find(c) != nullptr) cells.push_back(c);
                 }
             } else {
                 return CommandResult::failure("give from and to, or all");
             }
             auto before = std::make_shared<const TileStates>(snapshot(t, cells));
             for (const CellCoord c : cells) t.remove(c);
             auto after = std::make_shared<const TileStates>(snapshot(t, cells));
             record(state, context, std::move(before), std::move(after), t.samples(), t.samples());
             return CommandResult::success({{"removed", cells.size()}, {"terrain_tiles", t.size()}});
         }});

    add(registry,
        {"set_terrain_heights",
         "Sets one tile's heights (adding the tile if the cell has none): samples x samples "
         "numbers (get_terrain gives samples), row by row from the cell's corner (lowest x and "
         "z) along +x, rows along +z; sample (i, j) is at the corner plus (i, j) times the "
         "spacing. Metres, -10000 to 10000. For hand-made or imported ground. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"cell", "heights"}},
              {"properties",
               {{"cell", {{"type", "array"},
                          {"items", {{"type", "integer"}, {"minimum", -2147483648.0}, {"maximum", 2147483647.0}}},
                          {"minItems", 2},
                          {"maxItems", 2},
                          {"description", "[x, z]: the cell (64 m; cell [0, 0] spans 0 to 64 m on x and z)."}}},
                {"heights", {{"type", "array"},
                             {"items", {{"type", "number"}, {"minimum", -10000}, {"maximum", 10000}}},
                             {"minItems", 64},
                             {"maxItems", 16384}}}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Terrain& t = state.scene.terrain;
             const CellCoord cell{args["cell"][0].get<i32>(), args["cell"][1].get<i32>()};
             const Json& values = args["heights"];
             const usize n = t.samples();
             if (values.size() != n * n) {
                 return CommandResult::failure("heights must hold " + std::to_string(n * n) + " numbers (" +
                                               std::to_string(n) + " samples a side), not " +
                                               std::to_string(values.size()));
             }
             if (t.find(cell) == nullptr && (t.size() + 1) * n * n > k_max_terrain_samples) {
                 return CommandResult::failure("the terrain holds the most tiles it may (" + std::to_string(t.size()) +
                                               ") until terrain streams");
             }
             Terrain::Heights heights(n * n);
             for (usize i = 0; i < n * n; ++i) heights[i] = values[i].get<float>();
             const std::vector<CellCoord> cells{cell};
             auto before = std::make_shared<const TileStates>(snapshot(t, cells));
             t.set(cell, std::move(heights));
             auto after = std::make_shared<const TileStates>(snapshot(t, cells));
             record(state, context, std::move(before), std::move(after), t.samples(), t.samples());
             Json out = {{"cell", cell_json(cell)}, {"terrain_tiles", t.size()}};
             out["heights"] = height_range(t, cells);
             return CommandResult::success(std::move(out));
         }});

    add(registry,
        {"set_terrain_material",
         "Sets how the ground looks (all of it, until painted layers arrive): base_color "
         "(linear RGB) and roughness. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"base_color", {{"type", "array"},
                                {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                                {"minItems", 3},
                                {"maxItems", 3}}},
                {"roughness", number(0, 1, "0 mirror to 1 matte.")}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             world::TerrainMaterial& m = state.scene.terrain.material;
             const world::TerrainMaterial before = m;
             if (args.contains("base_color")) {
                 const Json& c = args["base_color"];
                 m.base_color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
             }
             m.roughness = args.value("roughness", m.roughness);
             const world::TerrainMaterial after = m;
             context.record(scene_change(state, [&state, before] { state.scene.terrain.material = before; },
                                         [&state, after] { state.scene.terrain.material = after; }));
             return CommandResult::success(material_json(after));
         }});

    add(registry,
        {"get_terrain",
         "Describes the terrain: tiles, samples per tile side and their spacing, the cells it "
         "covers, the material, its meshes (tiles drawn, building: still being made). With "
         "points ([x, z] metres), the ground's height at each (null where there is no tile): "
         "use it to stand things on the ground. With cell, that tile's heights.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"points", {{"type", "array"},
                            {"items", point_schema("[x, z], metres.")},
                            {"maxItems", k_max_points}}},
                {"cell", {{"type", "array"},
                          {"items", {{"type", "integer"}, {"minimum", -2147483648.0}, {"maximum", 2147483647.0}}},
                          {"minItems", 2},
                          {"maxItems", 2}}}}}},
         true,
         [&state](const Json& args, CommandContext&) {
             const Terrain& t = state.scene.terrain;
             Json out = {{"tiles", t.size()}, {"samples", t.samples()}, {"spacing", t.spacing()},
                         {"material", material_json(t.material)}};
             if (!t.empty()) {
                 CellCoord lo = t.tiles().begin()->first;
                 CellCoord hi = lo;
                 for (const auto& [c, h] : t.tiles()) {
                     lo = {std::min(lo.x, c.x), std::min(lo.z, c.z)};
                     hi = {std::max(hi.x, c.x), std::max(hi.z, c.z)};
                 }
                 out["first_cell"] = cell_json(lo);
                 out["last_cell"] = cell_json(hi);
             }
             if (state.terrain_meshes != nullptr) {
                 const TerrainMeshStats s = state.terrain_meshes->stats();
                 // Changes the meshes have not seen yet (they look once a frame) count too.
                 out["meshes"] = {{"tiles", s.tiles},
                                  {"building", s.building + t.untaken_changes()},
                                  {"built", s.built},
                                  {"triangles", s.triangles}};
             }
             if (args.contains("points")) {
                 Json heights = Json::array();
                 for (const Json& p : args["points"]) {
                     const std::optional<double> h = t.height_at(p[0].get<double>(), p[1].get<double>());
                     heights.push_back(h ? Json(*h) : Json(nullptr));
                 }
                 out["heights"] = std::move(heights);
             }
             if (args.contains("cell")) {
                 const CellCoord c{args["cell"][0].get<i32>(), args["cell"][1].get<i32>()};
                 const Terrain::Heights* h = t.find(c);
                 out["cell_heights"] = h != nullptr ? Json(*h) : Json(nullptr);
             }
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
