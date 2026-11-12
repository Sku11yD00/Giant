// Lakes, rivers and the sea (world::Water in scene.h): checking a body
// and asking where water is. A point is in a lake when it is inside its
// shore, in a river when it is within half the river's width of its path
// (level, width and flow taken along it), and in the sea anywhere: what
// is under the water there is the caller's (water over the ground is the
// level above the ground, none where the ground rises over it). Hidden
// bodies are not there. Where bodies overlap the highest surface wins.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace giant::world {

// What is wrong with a body (limits as the editor's commands and the
// scene's loading keep them), or nothing.
std::optional<std::string> check_water(const Water& water);

std::string_view water_kind_name(WaterKind kind);
std::optional<WaterKind> water_kind_from_name(std::string_view name);

// The water at a point (x, z metres).
struct WaterHere {
    const Water* water = nullptr;
    float level = 0.0f;  // the surface's height
    float flow_x = 0.0f; // its flow (metres a second; rivers only)
    float flow_z = 0.0f;
    float width = 0.0f;  // rivers: the width there
};
std::optional<WaterHere> water_at(std::span<const Water> waters, double x, double z);

// The ground a body covers, x and z (a sea: everywhere, false).
bool water_bounds(const Water& water, double& min_x, double& min_z, double& max_x, double& max_z);

} // namespace giant::world
