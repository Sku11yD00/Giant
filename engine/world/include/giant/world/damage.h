// Damage kept: what simulating broke, made part of the world (the
// keep_damage command) so it stays broken through stopping, streaming,
// saving and reopening. It is kept by cell, in the cell's file (and in a
// scene's file), beside what was authored there: a broken building's kit
// placement is still the whole kit, and its kept record says which pieces
// came down and where they lie; a scatter tree is still its layer's point,
// and its record says it lies felled. Craters are kept as the ground itself
// (the terrain's tiles: nothing here).
//
// A record holds what it is of, so it only applies while that is still
// there as it was: a kit record names its placement's id, kit and
// transform (moved or changed since, the building stands whole again); a
// tree record its foot's x and z and its size (another tree placed there
// stands).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/cell.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace giant::world {

// Kit placements and trees a cell's damage holds at most (a city block's
// houses, a cell's forest: the editor's caps are lower).
constexpr usize k_max_kept_kits = 4096;
constexpr usize k_max_kept_trees = 1u << 16;

// A kit piece come down: where it lies (its world transform).
struct KeptPiece {
    u32 piece = 0; // its index in the kit
    DVec3 position{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    bool operator==(const KeptPiece&) const = default;
};

// A breakable kit placement with pieces down (in piece order, each once).
// It belongs to the cell its placement's position is in.
struct KeptKit {
    u64 entity = 0;       // the placement's id (world::EntityId)
    std::string kit;      // its kit, as it was
    DVec3 position{};     // where it was placed (its transform, as it was)
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    std::vector<KeptPiece> pieces;
    bool operator==(const KeptKit&) const = default;
};

// A scatter tree felled: its foot as placed (x and z, metres; matched to
// the centimetre) and size, and where its foot lies now. It belongs to
// the cell its foot is in.
struct KeptTree {
    double x = 0.0;
    double z = 0.0;
    float height = 10.0f;      // metres, foot to top (the scale applied)
    float trunk_radius = 0.25f;
    DVec3 position{};          // its foot's transform, lying
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    bool operator==(const KeptTree&) const = default;
};

// One cell's damage: kit placements by id, trees by x then z.
struct CellDamage {
    std::vector<KeptKit> kits;
    std::vector<KeptTree> trees;
    bool empty() const { return kits.empty() && trees.empty(); }
    bool operator==(const CellDamage&) const = default;
};

using Damage = std::unordered_map<CellCoord, CellDamage, CellCoordHash>;

// The same records in their saved order (kits by id, trees by x then z;
// pieces by index), so equal damage saves the same text.
void sort_damage(CellDamage& damage);

} // namespace giant::world
