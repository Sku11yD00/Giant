// Wind near the ground under the weather (giant/core/wind.h has the wind
// itself: its clocks, gusts and how swaying things move in it).
#pragma once

#include "giant/core/wind.h"
#include "giant/world/scene.h"

namespace giant::world {

// The weather's wind moves the clouds; near the ground it blows at half
// that (the ground slows it), gustier the harder it blows.
Wind ground_wind(const Weather& w);

} // namespace giant::world
