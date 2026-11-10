// Terrain commands: making, shaping and reading the ground (world::Terrain).
// Every change is undoable; the tiles' meshes follow on worker threads
// (TerrainMeshes), so get_terrain says when they are all drawn.
#include "editor_commands.h"
#include "material_maps.h"
#include "physics_state.h"
#include "terrain_meshes.h"
#include "world_streaming.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "giant/core/text.h"
#include "giant/world/height_map.h"
#include "giant/world/terrain_brush.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

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

// Every terrain edit is made to the ground as authored: dents pressed in
// while simulating are lifted for it (and its snapshots), and put back
// after on the tiles it left as they were (an edited tile loses its dents).
void add(ai::CommandRegistry& registry, EditorState& state, ai::CommandDesc desc) {
    if (!desc.read_only) {
        desc.run = [&state, run = std::move(desc.run)](const Json& args, CommandContext& context) {
            const PhysicsState::GroundAsSaved as_authored(state.physics, state.scene);
            return run(args, context);
        };
    }
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "terrain command could not be registered");
}

// Tiles as they were (or will be), with their versions (Terrain::version:
// put back, a tile is again exactly what it was, so streaming sees a tile
// undone to what its file holds as unchanged): none means no tile.
struct TileState {
    CellCoord cell;
    std::optional<Terrain::Heights> heights;
    u64 version = 0;
};
using TileStates = std::vector<TileState>;

TileStates snapshot(const Terrain& t, const std::vector<CellCoord>& cells) {
    TileStates out;
    out.reserve(cells.size());
    for (const CellCoord c : cells) {
        const Terrain::Heights* h = t.find(c);
        out.push_back({c, h != nullptr ? std::optional<Terrain::Heights>(*h) : std::nullopt, t.version(c)});
    }
    return out;
}

// Puts the tiles back as `states` says. Tiles of another spacing than the
// terrain's (streaming gives an empty terrain its world's spacing outside
// the history) take theirs back when the terrain is empty, and are skipped
// otherwise (logged), never asserted on.
void put_back(Terrain& t, const TileStates& states) {
    for (const auto& [cell, heights, version] : states) {
        if (!heights) t.remove(cell);
    }
    for (const auto& [cell, heights, version] : states) {
        if (!heights) continue;
        const usize n = t.samples();
        if (heights->size() != n * n) {
            const auto side = static_cast<u32>(std::lround(std::sqrt(static_cast<double>(heights->size()))));
            if (!t.empty() || !t.set_samples(side) || heights->size() != usize{side} * side) {
                log_warning("terrain", "undo: tile [" + std::to_string(cell.x) + ", " + std::to_string(cell.z) +
                                           "] is " + std::to_string(side) + " samples a side and the terrain " +
                                           std::to_string(n) + ": not put back");
                continue;
            }
        }
        t.restore(cell, *heights, version);
    }
}

// Refused when a cell the command would change has ground in the open
// world's files that is not loaded (WorldStreaming::ground_on_disk).
std::optional<std::string> ground_not_loaded(const EditorState& state, const std::vector<CellCoord>& cells) {
    if (state.world == nullptr || !state.world->active(state.scene_generation)) {
        return std::nullopt;
    }
    for (const CellCoord c : cells) {
        if (state.world->ground_on_disk(c)) {
            return "cell [" + std::to_string(c.x) + ", " + std::to_string(c.z) +
                   "] has ground saved in the open world that is not loaded yet: bring the camera near it "
                   "(get_world shows loading), or leave it out of the area";
        }
    }
    return std::nullopt;
}

// Records `before` -> `after` (with the sample spacing each side had).
void record(EditorState& state, CommandContext& context, std::shared_ptr<const TileStates> before,
            std::shared_ptr<const TileStates> after, u32 samples_before, u32 samples_after) {
    context.record(scene_change(
        state,
        [&state, before, samples_before] {
            put_back(state.scene.terrain, *before);
            if (state.scene.terrain.empty()) state.scene.terrain.set_samples(samples_before);
        },
        [&state, after, samples_after] {
            // Spacing first: it can change only while there are no tiles,
            // which is how the change was made.
            state.scene.terrain.set_samples(samples_after);
            put_back(state.scene.terrain, *after);
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

Json maps_json(const world::MaterialMaps& maps) {
    Json out = Json::object();
    if (!maps.base_color.empty()) out["base_color"] = maps.base_color;
    if (!maps.normal.empty()) out["normal"] = maps.normal;
    if (!maps.roughness_metal.empty()) out["roughness_metal"] = maps.roughness_metal;
    if (maps.normal_green_down) out["normal_green_down"] = true;
    return out;
}

Json layer_json(const world::TerrainLayer& l) {
    return {{"base_color", Json::array({l.base_color.x, l.base_color.y, l.base_color.z})},
            {"roughness", l.roughness},
            {"maps", maps_json(l.maps)},
            {"tiling", l.tiling},
            {"min_slope", l.min_slope},
            {"max_slope", l.max_slope},
            {"slope_blend", l.slope_blend},
            {"min_height", l.min_height},
            {"max_height", l.max_height},
            {"height_blend", l.height_blend}};
}

Json material_json(const world::TerrainMaterial& m) {
    Json layers = Json::array();
    for (const world::TerrainLayer& l : m.layers) layers.push_back(layer_json(l));
    return {{"base_color", Json::array({m.base_color.x, m.base_color.y, m.base_color.z})},
            {"roughness", m.roughness},
            {"layers", std::move(layers)}};
}

Json layer_schema() {
    // The maps a layer uses (an occlusion map has no place on the ground).
    Json maps = maps_schema();
    maps["properties"].erase("occlusion");
    maps["description"] = "Image files mapped by world position every `tiling` metres (from above, and "
                          "from the sides where steep). Paths from the editor's working folder; \"\" "
                          "removes one. roughness_metal's green (roughness) multiplies roughness; its "
                          "metal is ignored (the ground is not metal).";
    const Json colour{{"type", "array"},
                      {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                      {"minItems", 3},
                      {"maxItems", 3},
                      {"description", "Linear RGB, multiplying the colour image (set [1, 1, 1] to show it as painted)."}};
    const double h = world::k_max_terrain_layer_height;
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"base_color", colour},
              {"roughness", number(0, 1, "0 mirror to 1 matte; multiplies the roughness image.")},
              {"maps", std::move(maps)},
              {"tiling", number(0.05, 1000, // world::k_min_terrain_tiling, k_max_terrain_tiling (as typed)
                                "Metres one repeat of the images covers. Default 4.")},
              {"min_slope", number(0, 90, "Lies where the ground is at least this steep, degrees from level. Default 0.")},
              {"max_slope", number(0, 90, "And at most this steep. Default 90.")},
              {"slope_blend", number(0, 90, "Degrees over which it fades in at the slope band's edges. Default 5.")},
              {"min_height", number(-h, h, "Lies where the ground is at least this high, metres above 0. Default -10000.")},
              {"max_height", number(-h, h, "And at most this high. Default 10000.")},
              {"height_blend", number(0, h, "Metres over which it fades in at the height band's edges. Default 10.")}}}};
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

// A sculpt command's limits: the cells its stroke reaches, its dabs, and
// the samples its dabs reach in all (each dab's square), so one call is a
// bounded hitch (the brush tool keeps its radius to 128 samples, so a
// frame's dab is well under a millisecond's work at 64 samples a tile).
constexpr usize k_max_stroke_cells = 4096;
constexpr usize k_max_stroke_dabs = 65536;
constexpr double k_max_stroke_samples = static_cast<double>(k_max_samples_per_command);

// One undo step of strokes (a drag, its calls merged): the tiles as they
// were before its first change, and as they were when it was undone (for
// redo), so a long drag holds one copy per tile it touched, not one a call.
struct SculptStroke {
    u64 group = 0; // its merge group
    TileStates before;
    std::unordered_set<CellCoord, world::CellCoordHash> cells;
    TileStates after;
};

world::BrushOp brush_op(const std::string& name) {
    if (name == "lower") return world::BrushOp::lower;
    if (name == "smooth") return world::BrushOp::smooth;
    if (name == "flatten") return world::BrushOp::flatten;
    return world::BrushOp::raise;
}

std::optional<std::vector<u8>> read_bytes(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "could not open " + path_to_utf8(path);
        return std::nullopt;
    }
    if (size > (usize{1} << 30)) {
        error = path_to_utf8(path) + " is over 1 GB";
        return std::nullopt;
    }
    std::vector<u8> bytes(static_cast<usize>(size));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = "could not read " + path_to_utf8(path);
        return std::nullopt;
    }
    return bytes;
}

} // namespace

void register_terrain_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry, state,
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
             // In a streamed world whose files hold tiles, the spacing is theirs.
             // (0: the world has none, or none is open; a plain u32 keeps GCC's
             // maybe-uninitialized check quiet.)
             const u32 ground = state.world != nullptr && state.world->active(state.scene_generation)
                                    ? state.world->ground_samples().value_or(0u)
                                    : 0u;
             u32 samples = args.value("samples", ground != 0 ? ground : samples_before);
             if (ground != 0 && samples != ground) {
                 return CommandResult::failure("the open world's terrain tiles are " + std::to_string(ground) +
                                               " samples a side; its terrain cannot take another");
             }
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
                                               " of " + std::to_string(n) + " samples loaded at once");
             }
             if (auto refused = ground_not_loaded(state, cells)) return CommandResult::failure(*refused);
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

    add(registry, state,
        {"remove_terrain",
         "Removes the terrain tiles of every 64 m cell the area [from, to] (x, z metres) "
         "touches, or every tile with all (in a streamed world: every tile loaded). Undoable.",
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
                 const std::vector<CellCoord> touched = cells_in(*area);
                 // Ground on disk there would stream back: refused, as for the other commands.
                 if (auto refused = ground_not_loaded(state, touched)) return CommandResult::failure(*refused);
                 for (const CellCoord c : touched) {
                     if (t.find(c) != nullptr) cells.push_back(c);
                 }
             } else {
                 return CommandResult::failure("give from and to, or all");
             }
             if (auto refused = ground_not_loaded(state, cells)) return CommandResult::failure(*refused);
             auto before = std::make_shared<const TileStates>(snapshot(t, cells));
             for (const CellCoord c : cells) t.remove(c);
             auto after = std::make_shared<const TileStates>(snapshot(t, cells));
             record(state, context, std::move(before), std::move(after), t.samples(), t.samples());
             return CommandResult::success({{"removed", cells.size()}, {"terrain_tiles", t.size()}});
         }});

    add(registry, state,
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
             if (auto refused = ground_not_loaded(state, cells)) return CommandResult::failure(*refused);
             auto before = std::make_shared<const TileStates>(snapshot(t, cells));
             t.set(cell, std::move(heights));
             auto after = std::make_shared<const TileStates>(snapshot(t, cells));
             record(state, context, std::move(before), std::move(after), t.samples(), t.samples());
             Json out = {{"cell", cell_json(cell)}, {"terrain_tiles", t.size()}};
             out["heights"] = height_range(t, cells);
             return CommandResult::success(std::move(out));
         }});

    add(registry, state,
        {"sculpt_terrain",
         "Sculpts the ground with a round brush along a stroke (one point is one dab): raise or "
         "lower (strength in metres at the centre), smooth (towards the average of each sample's "
         "neighbours) or flatten (towards `height`; strength 0 to 1 is how far a dab goes). The "
         "brush fades out over the outer `falloff` part of its radius, and dabs are made at least "
         "every quarter radius along the stroke. Only existing tiles change (create_terrain makes "
         "ground; ground of an open world not loaded yet is left as it is: cells_not_loaded). At "
         "most 4096 cells, 65536 dabs and 16.7 million heights reached (dabs x square) a call. The editor's brush tool sends one call a frame while dragging, as one undo step. "
         "Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"op", "points"}},
              {"properties",
               {{"op", {{"type", "string"}, {"enum", {"raise", "lower", "smooth", "flatten"}}}},
                {"points", {{"type", "array"}, {"items", point_schema("[x, z], metres.")}, {"minItems", 1},
                            {"maxItems", k_max_points}}},
                {"radius", number(0.5, 2000, "Metres. Default 8.")},
                {"strength", number(0, 1000, "raise/lower: metres a dab at the centre (default 1); smooth/flatten: "
                                             "0 to 1 (default 0.5).")},
                {"falloff", number(0, 1, "The outer part of the radius over which it fades. Default 0.5.")},
                {"height", number(-10000, 10000, "flatten: the height to flatten to. Default 0.")},
                {"continues", {{"type", "boolean"},
                               {"description", "The first point is the last call's last dab (a drag going on): "
                                               "not dabbed again. Default false."}}}}}},
         false,
         [&state, current = std::make_shared<std::shared_ptr<SculptStroke>>()](const Json& args,
                                                                            CommandContext& context) {
             std::shared_ptr<SculptStroke>& stroke = *current; // the newest step's
             Terrain& t = state.scene.terrain;
             world::Brush brush;
             brush.op = brush_op(args["op"].get<std::string>());
             brush.radius = args.value("radius", 8.0);
             const bool metres = brush.op == world::BrushOp::raise || brush.op == world::BrushOp::lower;
             brush.strength = args.value("strength", metres ? 1.0f : 0.5f);
             if (!metres) brush.strength = std::min(brush.strength, 1.0f);
             brush.falloff = args.value("falloff", 0.5f);
             brush.height = args.value("height", 0.0f);
             std::vector<world::GroundPoint> points;
             for (const Json& p : args["points"]) points.push_back({p[0].get<double>(), p[1].get<double>()});
             std::vector<world::GroundPoint> dabs = world::brush_dabs(brush, points, k_max_stroke_dabs);
             if (dabs.size() > k_max_stroke_dabs) {
                 return CommandResult::failure("the stroke makes over " + std::to_string(k_max_stroke_dabs) +
                                               " dabs (one every quarter radius): a shorter stroke a call");
             }
             if (args.value("continues", false) && dabs.size() > 1) dabs.erase(dabs.begin());
             const double side = 2.0 * brush.radius / t.spacing() + 1.0;
             if (static_cast<double>(dabs.size()) * side * side > k_max_stroke_samples) {
                 return CommandResult::failure("the stroke's dabs reach over " +
                                               std::to_string(k_max_samples_per_command) +
                                               " heights in all: a smaller radius or fewer points a call");
             }
             // The cells the stroke reaches.
             std::unordered_set<CellCoord, world::CellCoordHash> reach;
             for (const world::GroundPoint& d : dabs) {
                 const CellCoord lo = world::cell_of({d.x - brush.radius, 0.0, d.z - brush.radius});
                 const CellCoord hi = world::cell_of({d.x + brush.radius, 0.0, d.z + brush.radius});
                 for (i64 z = lo.z; z <= hi.z; ++z) {
                     for (i64 x = lo.x; x <= hi.x; ++x) {
                         reach.insert({static_cast<i32>(x), static_cast<i32>(z)});
                         if (reach.size() > k_max_stroke_cells) {
                             return CommandResult::failure(
                                 "the stroke reaches over " + std::to_string(k_max_stroke_cells) +
                                 " cells of 64 m: a smaller radius or a shorter stroke a call");
                         }
                     }
                 }
             }
             // Ground saved in an open world but not loaded has no tile here,
             // so the brush leaves it as it is (the stroke goes on over the
             // ground that is loaded).
             usize not_loaded = 0;
             if (state.world != nullptr && state.world->active(state.scene_generation)) {
                 for (const CellCoord c : reach) not_loaded += state.world->ground_on_disk(c) ? usize{1} : usize{0};
             }
             // One copy per tile a step: a run joining a stroke's step adds
             // only tiles it had not changed yet.
             const bool joins =
                 context.merging() && stroke != nullptr && stroke->group == context.merge_group();
             if (!joins) {
                 stroke = std::make_shared<SculptStroke>();
                 stroke->group = context.merge_group();
             }
             std::vector<CellCoord> fresh;
             for (const CellCoord c : reach) {
                 if (t.find(c) != nullptr && !stroke->cells.contains(c)) fresh.push_back(c);
             }
             TileStates before = snapshot(t, fresh);
             std::vector<CellCoord> changed;
             for (const world::GroundPoint& d : dabs) world::apply_brush(t, brush, d.x, d.z, changed);
             std::sort(changed.begin(), changed.end(), [](CellCoord a, CellCoord b) {
                 return a.z != b.z ? a.z < b.z : a.x < b.x;
             });
             changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
             const usize steps_before = stroke->before.size();
             for (TileState& s : before) {
                 if (std::binary_search(changed.begin(), changed.end(), s.cell, [](CellCoord a, CellCoord b) {
                         return a.z != b.z ? a.z < b.z : a.x < b.x;
                     })) {
                     stroke->cells.insert(s.cell);
                     stroke->before.push_back(std::move(s));
                 }
             }
             if (!joins && steps_before == 0 && !stroke->before.empty()) {
                 // The step's one change: undo keeps the tiles as they are
                 // then for redo (undo runs only on the state after the step).
                 std::shared_ptr<SculptStroke> step = stroke;
                 context.record(scene_change(
                     state,
                     [&state, step] {
                         std::vector<CellCoord> cells;
                         cells.reserve(step->before.size());
                         for (const TileState& s : step->before) cells.push_back(s.cell);
                         step->after = snapshot(state.scene.terrain, cells);
                         put_back(state.scene.terrain, step->before);
                     },
                     [&state, step] { put_back(state.scene.terrain, step->after); }));
             } else if (!joins) {
                 stroke.reset(); // nothing changed: no step
             }
             Json out = {{"dabs", dabs.size()}, {"tiles_changed", changed.size()}};
             if (not_loaded > 0) out["cells_not_loaded"] = not_loaded;
             out["heights"] = height_range(t, changed);
             return CommandResult::success(std::move(out));
         }});

    add(registry, state,
        {"import_height_map",
         "Makes ground from a height map file over the area [from, to] (x, z metres; the image's "
         "first row at `from`'s z, its first column at `from`'s x): a tile for every 64 m cell the "
         "area touches (replacing tiles there; samples outside the area keep their height, or the "
         "image's edge on new tiles), black at `low` and white at `high` metres. Files: .r16 or "
         ".raw (16-bit little-endian, square: World Machine, Gaea) and .pgm (8 or 16 bit), relative "
         "to the editor's working folder. The terrain's spacing; at most 4096 tiles. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"file", "from", "to"}},
              {"properties",
               {{"file", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
                {"from", point_schema("[x, z], metres: where the image's first sample goes.")},
                {"to", point_schema("[x, z], metres: where its last goes.")},
                {"low", number(-10000, 10000, "Metres for black. Default 0.")},
                {"high", number(-10000, 10000, "Metres for white. Default 100.")}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Terrain& t = state.scene.terrain;
             const std::string name = args["file"].get<std::string>();
             const std::filesystem::path path(std::u8string(name.begin(), name.end()));
             std::string error;
             const auto bytes = read_bytes(path, error);
             if (!bytes) return CommandResult::failure(error);
             world::HeightMap map;
             if (auto bad = world::read_height_map(*bytes, path_to_utf8(path.extension()), map)) {
                 return CommandResult::failure(path_to_utf8(path) + ": " + *bad);
             }
             const std::optional<Area> area = area_of(args["from"], args["to"], error);
             if (!area) return CommandResult::failure(error);
             const double fx = args["from"][0].get<double>(), fz = args["from"][1].get<double>();
             const double tx = args["to"][0].get<double>(), tz = args["to"][1].get<double>();
             if (fx == tx || fz == tz) return CommandResult::failure("from and to must differ on x and on z");
             const float low = args.value("low", 0.0f);
             const float high = args.value("high", 100.0f);
             std::vector<CellCoord> cells = cells_in(*area);
             {
                 // A `to` on a cell's edge puts the image's last row or column
                 // on the next cell's first samples: a tile there takes them
                 // (no step at the edge); none, and no tile is made for them.
                 const CellCoord end = world::cell_of({std::max(fx, tx), 0.0, std::max(fz, tz)});
                 for (i32 z = area->first.z; z <= end.z; ++z) {
                     for (i32 x = area->first.x; x <= end.x; ++x) {
                         if ((x > area->last.x || z > area->last.z) && t.find({x, z}) != nullptr) {
                             cells.push_back({x, z});
                         }
                     }
                 }
             }
             const usize n = t.samples();
             usize added = 0;
             for (const CellCoord c : cells) added += t.find(c) == nullptr ? usize{1} : usize{0};
             if (cells.size() * n * n > k_max_samples_per_command) {
                 return CommandResult::failure("the area needs " + std::to_string(cells.size() * n * n) +
                                               " heights; at most " + std::to_string(k_max_samples_per_command) +
                                               " a command: split it");
             }
             if ((t.size() + added) * n * n > k_max_terrain_samples) {
                 return CommandResult::failure("the terrain would hold " + std::to_string(t.size() + added) +
                                               " tiles; at most " + std::to_string(k_max_terrain_samples / (n * n)));
             }
             if (auto refused = ground_not_loaded(state, cells)) return CommandResult::failure(*refused);
             auto before = std::make_shared<const TileStates>(snapshot(t, cells));
             const double spacing = t.spacing();
             const double x0 = std::min(fx, tx), x1 = std::max(fx, tx);
             const double z0 = std::min(fz, tz), z1 = std::max(fz, tz);
             for (const CellCoord c : cells) {
                 const Terrain::Heights* old = t.find(c);
                 Terrain::Heights heights(n * n);
                 for (usize j = 0; j < n; ++j) {
                     for (usize i = 0; i < n; ++i) {
                         const double x = c.x * world::k_cell_size + static_cast<double>(i) * spacing;
                         const double z = c.z * world::k_cell_size + static_cast<double>(j) * spacing;
                         const bool inside = x >= x0 && x <= x1 && z >= z0 && z <= z1;
                         if (!inside && old != nullptr) {
                             heights[j * n + i] = (*old)[j * n + i];
                             continue;
                         }
                         const float v = map.at((x - fx) / (tx - fx), (z - fz) / (tz - fz));
                         heights[j * n + i] = std::clamp(low + (high - low) * v, -world::k_max_terrain_height,
                                                         world::k_max_terrain_height);
                     }
                 }
                 t.set(c, std::move(heights));
             }
             auto after = std::make_shared<const TileStates>(snapshot(t, cells));
             record(state, context, std::move(before), std::move(after), t.samples(), t.samples());
             Json out = {{"tiles", cells.size()},
                         {"width", map.width},
                         {"height", map.height},
                         {"first_cell", cell_json(area->first)},
                         {"last_cell", cell_json(area->last)}};
             out["heights"] = height_range(t, cells);
             return CommandResult::success(std::move(out));
         }});

    add(registry, state,
        {"set_terrain_material",
         "Sets how the ground looks without layers (set_terrain_layers), and the far voxel "
         "world's colour with them: base_color (linear RGB) and roughness. Undoable.",
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

    add(registry, state,
        {"set_terrain_layers",
         "Sets the ground's materials as layers stacked like paint: the first covers everywhere, "
         "each later one lies over those before it where the ground's slope and height are within "
         "its bands (fading at their edges), e.g. grass, then rock from 35 degrees, then snow above "
         "1500 m. Each has a colour, roughness and image files mapped by world position. Replaces "
         "all layers ([] for none: the plain base_color and roughness). The far voxel world, past "
         "where meshes are drawn, keeps the terrain material's base_color: set it "
         "(set_terrain_material) near the layers' look from afar. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"layers"})},
              {"properties",
               {{"layers", {{"type", "array"},
                            {"items", layer_schema()},
                            {"maxItems", world::k_max_terrain_layers},
                            {"description", "Bottom first."}}}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<world::TerrainLayer> layers;
             for (const Json& j : args["layers"]) {
                 world::TerrainLayer l;
                 if (j.contains("base_color")) {
                     const Json& c = j["base_color"];
                     l.base_color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                 }
                 l.roughness = j.value("roughness", l.roughness);
                 l.tiling = j.value("tiling", l.tiling);
                 l.min_slope = j.value("min_slope", l.min_slope);
                 l.max_slope = j.value("max_slope", l.max_slope);
                 l.slope_blend = j.value("slope_blend", l.slope_blend);
                 l.min_height = j.value("min_height", l.min_height);
                 l.max_height = j.value("max_height", l.max_height);
                 l.height_blend = j.value("height_blend", l.height_blend);
                 if (j.contains("maps")) {
                     // Images the layers name already are taken as they are
                     // (read back and sent again, even if a file has gone
                     // since); new ones must exist.
                     Json given = j["maps"];
                     for (const char* key : {"base_color", "normal", "roughness_metal"}) {
                         if (!given.contains(key)) continue;
                         const std::string path = given[key].get<std::string>();
                         const auto named = [&](const world::TerrainLayer& o) {
                             return !path.empty() && (o.maps.base_color == path || o.maps.normal == path ||
                                                      o.maps.roughness_metal == path);
                         };
                         const std::vector<world::TerrainLayer>& now = state.scene.terrain.material.layers;
                         if (std::any_of(now.begin(), now.end(), named)) {
                             (std::string_view(key) == "base_color" ? l.maps.base_color
                              : std::string_view(key) == "normal"   ? l.maps.normal
                                                                    : l.maps.roughness_metal) = path;
                             given.erase(key);
                         }
                     }
                     if (std::string bad = apply_maps(given, l.maps); !bad.empty()) {
                         return CommandResult::failure("layers[" + std::to_string(layers.size()) + "]." + bad);
                     }
                 }
                 layers.push_back(std::move(l));
             }
             world::TerrainMaterial& m = state.scene.terrain.material;
             const world::TerrainMaterial before = m;
             m.layers = std::move(layers);
             const world::TerrainMaterial after = m;
             context.record(scene_change(state, [&state, before] { state.scene.terrain.material = before; },
                                         [&state, after] { state.scene.terrain.material = after; }));
             return CommandResult::success(material_json(after));
         }});

    add(registry, state,
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
