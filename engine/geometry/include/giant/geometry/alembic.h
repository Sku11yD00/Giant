// Hair curves from Alembic files (.abc, the Ogawa kind every tool has
// written since 2013), read by a small reader of our own: grooms made in
// Houdini, Maya or Blender, such as those exported for Unreal.
//
// Read: every shown curves object's first sample (positions and how many
// points each curve has), placed by the transforms above it. Curves the
// file marks as guides (`groom_guide`, the Unreal groom convention) are
// left out and counted: the engine picks its own guides. Cubic curves are
// read through their control points. Nothing else in a file is read (no
// meshes, no animation, no widths or colours yet).
//
// The points stay in the file's space, units and up axis: a groom exported
// for Unreal is in centimetres, left-handed with +Z up. to_engine_space
// turns one.
//
// A file is never trusted: every offset and count is checked against the
// file's size, and the work is bounded (k_max_alembic_bytes, objects and
// properties visited, k_max_import_curves and k_max_import_curve_points).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace giant::geometry {

inline constexpr u64 k_max_alembic_bytes = 2'000'000'000; // a larger file is refused

struct AlembicCurves {
    // Each two or more points, root first as the file orders them. Curves
    // with a point that is no number, or of no length, are left out.
    std::vector<std::vector<Vec3>> curves;
    u64 guides = 0;                    // curves left out as guides
    std::vector<std::string> warnings; // things that were skipped
    std::string error;                 // empty when the file was read

    bool ok() const { return error.empty(); }
};

// Reads an Alembic file's hair curves. A file that is not Ogawa Alembic, or
// is damaged, gives an error; one without curves is read and has none.
AlembicCurves read_alembic_curves_file(const std::filesystem::path& path);
AlembicCurves read_alembic_curves(std::span<const u8> data);

// The axes a file's points are in.
enum class FileAxes : u8 {
    y_up,       // the engine's own (glTF's): nothing turns
    z_up,       // right-handed with +Z up (Blender, Houdini, Maya set so): (x, z, -y)
    z_up_left,  // left-handed with +Z up (Unreal's, as its grooms are exported): (x, z, y)
};

// Curves in a file's space to the engine's (metres, +Y up, right-handed):
// each point turned from `axes`, times `scale` (0.01 for centimetres).
void to_engine_space(std::vector<std::vector<Vec3>>& curves, float scale, FileAxes axes);

} // namespace giant::geometry
