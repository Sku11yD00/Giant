#include "giant/world/terrain.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace giant::world {

bool valid_terrain_samples(u32 samples) {
    return samples >= k_min_terrain_samples && samples <= k_max_terrain_samples && (samples & (samples - 1)) == 0;
}

// --- Terrain ----------------------------------------------------------------------

Terrain::Identity::Identity() {
    static std::atomic<u64> next{1};
    value = next.fetch_add(1, std::memory_order_relaxed);
}

Terrain::Identity& Terrain::Identity::operator=(const Identity&) {
    value = Identity().value;
    return *this;
}

namespace {
u64 next_version() {
    static std::atomic<u64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

Terrain::Terrain(const Terrain& other)
    : material(other.material), m_samples(other.m_samples), m_tiles(other.m_tiles), m_versions(other.m_versions) {}

Terrain::Terrain(Terrain&& other) noexcept
    : material(other.material), m_samples(other.m_samples), m_tiles(std::move(other.m_tiles)),
      m_versions(std::move(other.m_versions)) {
    other.clear();
}

Terrain& Terrain::operator=(const Terrain& other) {
    if (this != &other) {
        *this = Terrain(other);
    }
    return *this;
}

Terrain& Terrain::operator=(Terrain&& other) noexcept {
    if (this != &other) {
        material = other.material;
        m_samples = other.m_samples;
        m_tiles = std::move(other.m_tiles);
        m_versions = std::move(other.m_versions);
        m_changes = {{}, true};
        m_identity = Identity();
        other.clear();
    }
    return *this;
}

bool Terrain::set_samples(u32 samples) {
    if (!m_tiles.empty() || !valid_terrain_samples(samples)) {
        return samples == m_samples && valid_terrain_samples(samples);
    }
    m_samples = samples;
    return true;
}

const Terrain::Heights* Terrain::find(CellCoord cell) const {
    const auto it = m_tiles.find(cell);
    return it == m_tiles.end() ? nullptr : &it->second;
}

Terrain::Heights* Terrain::modify(CellCoord cell) {
    const auto it = m_tiles.find(cell);
    if (it == m_tiles.end()) {
        return nullptr;
    }
    changed(cell);
    m_versions[cell] = next_version();
    return &it->second;
}

u64 Terrain::version(CellCoord cell) const {
    const auto it = m_versions.find(cell);
    return it == m_versions.end() ? 0 : it->second;
}

void Terrain::set(CellCoord cell, Heights heights) {
    GIANT_ASSERT(heights.size() == usize{m_samples} * m_samples, "a terrain tile has samples() squared heights");
    GIANT_ASSERT(std::all_of(heights.begin(), heights.end(),
                             [](float h) { return std::isfinite(h) && std::fabs(h) <= k_max_terrain_height; }),
                 "terrain heights are finite and within k_max_terrain_height");
    m_tiles[cell] = std::move(heights);
    m_versions[cell] = next_version();
    changed(cell);
}

void Terrain::restore(CellCoord cell, Heights heights, u64 version) {
    set(cell, std::move(heights));
    if (version != 0) m_versions[cell] = version;
}

std::optional<Terrain::Heights> Terrain::remove(CellCoord cell) {
    const auto it = m_tiles.find(cell);
    if (it == m_tiles.end()) {
        return std::nullopt;
    }
    Heights out = std::move(it->second);
    m_tiles.erase(it);
    m_versions.erase(cell);
    changed(cell);
    return out;
}

void Terrain::clear() {
    m_tiles.clear();
    m_versions.clear();
    m_changes = {{}, true};
}

void Terrain::changed(CellCoord cell) {
    if (m_changes.all) {
        return;
    }
    // Nobody may be taking them: past a point "everything" says it in less memory.
    if (m_changes.tiles.size() >= 4096 + m_tiles.size()) {
        m_changes.tiles.clear();
        m_changes.tiles.shrink_to_fit();
        m_changes.all = true;
        return;
    }
    m_changes.tiles.push_back(cell);
}

TerrainChanges Terrain::take_changes() {
    TerrainChanges out = std::move(m_changes);
    m_changes = {};
    return out;
}

namespace {

// Rounds towards minus infinity (C++ division rounds towards zero).
i64 floor_div(i64 a, i64 b) {
    const i64 q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

constexpr i64 k_i32_min = std::numeric_limits<i32>::min();
constexpr i64 k_i32_max = std::numeric_limits<i32>::max();

} // namespace

std::optional<float> Terrain::sample(i64 i, i64 j, const CellCoord* home) const {
    const i64 n = m_samples;
    const auto tile_of = [&](i64 si, i64 sj, CellCoord& c) {
        const i64 cx = floor_div(si, n);
        const i64 cz = floor_div(sj, n);
        if (cx < k_i32_min || cx > k_i32_max || cz < k_i32_min || cz > k_i32_max) return false;
        c = {static_cast<i32>(cx), static_cast<i32>(cz)};
        return true;
    };
    for (const auto& [di, dj] : {std::pair<i64, i64>{0, 0}, {1, 0}, {0, 1}, {1, 1}}) {
        const i64 si = i - di;
        const i64 sj = j - dj;
        CellCoord c;
        if (!tile_of(si, sj, c)) continue;
        const Heights* t = find(c);
        if (t == nullptr) continue;
        // A tile touching home only at a corner, with no tile between them:
        // its corner is not home's (each keeps its own height there).
        if (home != nullptr && c.x != home->x && c.z != home->z && find({c.x, home->z}) == nullptr &&
            find({home->x, c.z}) == nullptr) {
            continue;
        }
        return (*t)[static_cast<usize>((sj - i64{c.z} * n) * n + (si - i64{c.x} * n))];
    }
    return std::nullopt;
}

std::optional<double> Terrain::height_at(double x, double z) const {
    // The tile the point is in, or the one it is the far edge of.
    CellCoord home = cell_of({x, 0.0, z});
    if (find(home) == nullptr) {
        const bool on_x = x == home.x * k_cell_size && home.x > std::numeric_limits<i32>::min();
        const bool on_z = z == home.z * k_cell_size && home.z > std::numeric_limits<i32>::min();
        const CellCoord before[3] = {{home.x - (on_x ? 1 : 0), home.z},
                                     {home.x, home.z - (on_z ? 1 : 0)},
                                     {home.x - (on_x ? 1 : 0), home.z - (on_z ? 1 : 0)}};
        bool found = false;
        for (const CellCoord c : before) {
            if (!(c == home) && find(c) != nullptr) {
                home = c;
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    const double s = spacing();
    // In samples from the cell's corner: [0, samples()].
    const double u = std::clamp((x - home.x * k_cell_size) / s, 0.0, static_cast<double>(m_samples));
    const double v = std::clamp((z - home.z * k_cell_size) / s, 0.0, static_cast<double>(m_samples));
    const double fu = std::min(std::floor(u), static_cast<double>(m_samples - 1));
    const double fv = std::min(std::floor(v), static_cast<double>(m_samples - 1));
    const i64 i = i64{home.x} * m_samples + static_cast<i64>(fu);
    const i64 j = i64{home.z} * m_samples + static_cast<i64>(fv);
    const double a = u - fu; // along x, 0 to 1
    const double b = v - fv; // along z
    // Corners of a square of the home tile, as its mesh has them: always there.
    const double h00 = *sample(i, j, &home);
    const double h10 = *sample(i + 1, j, &home);
    const double h01 = *sample(i, j + 1, &home);
    const double h11 = *sample(i + 1, j + 1, &home);
    // The square's two triangles meet on its (0, 0)-(1, 1) diagonal.
    if (b >= a) {
        return h00 + a * (h11 - h01) + b * (h01 - h00);
    }
    return h00 + a * (h10 - h00) + b * (h11 - h10);
}

// --- Hills ----------------------------------------------------------------------

namespace {

// A value from -1 to 1 for a lattice point: integer hashing only, so the
// same on every machine.
double lattice(i64 x, i64 z, u32 seed) {
    u64 h = static_cast<u64>(x) * 0x9E3779B97F4A7C15ull ^ static_cast<u64>(z) * 0xC2B2AE3D27D4EB4Full ^
            (u64{seed} + 0x165667B19E3779F9ull) * 0x27D4EB2F165667C5ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return static_cast<double>(h >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

// Smooth value noise, -1 to 1, one lattice square per unit.
double value_noise(double x, double z, u32 seed) {
    const double fx = std::floor(x);
    const double fz = std::floor(z);
    const i64 ix = static_cast<i64>(fx);
    const i64 iz = static_cast<i64>(fz);
    const auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
    const double a = fade(x - fx);
    const double b = fade(z - fz);
    const double v00 = lattice(ix, iz, seed);
    const double v10 = lattice(ix + 1, iz, seed);
    const double v01 = lattice(ix, iz + 1, seed);
    const double v11 = lattice(ix + 1, iz + 1, seed);
    const double low = v00 + (v10 - v00) * a;
    const double high = v01 + (v11 - v01) * a;
    return low + (high - low) * b;
}

} // namespace

float hills_height(const HillsSettings& hills, double x, double z) {
    const u32 octaves = std::clamp<u32>(hills.octaves, 1, 8);
    const double size = std::max(static_cast<double>(hills.size), 1.0);
    double sum = 0.0;
    double weight = 0.0;
    double scale = 1.0;
    double amplitude = 1.0;
    for (u32 o = 0; o < octaves; ++o) {
        sum += amplitude * value_noise(x * scale / size, z * scale / size, hills.seed + o * 7919u);
        weight += amplitude;
        scale *= 2.0;
        amplitude *= 0.5;
    }
    const double h = static_cast<double>(hills.base) + static_cast<double>(hills.amplitude) * sum / weight;
    return static_cast<float>(std::clamp(h, -static_cast<double>(k_max_terrain_height),
                                         static_cast<double>(k_max_terrain_height)));
}

} // namespace giant::world
