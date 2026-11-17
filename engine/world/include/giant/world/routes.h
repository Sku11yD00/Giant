// The world's roads and paths (world::Routes in scene.h; living-world plan
// LW2): checking them and finding their nodes and links. The limits are
// the ones the editor's commands and the scene's loading keep.
#pragma once

#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace giant::world {

std::string_view route_kind_name(RouteKind kind);
std::optional<RouteKind> route_kind_from_name(std::string_view name);

// What is wrong with one (its own fields alone), or nothing.
std::optional<std::string> check_route_node(const RouteNode& node);
std::optional<std::string> check_route_link(const RouteLink& link);
// What is wrong among them all, or nothing: more than the scene keeps,
// nodes not by rising id or not under `next_id`, links not by rising
// (a, b), a pair twice, an end that is no node. (Each one's own fields
// are check_route_node's and check_route_link's.)
std::optional<std::string> check_routes(const Routes& routes);

// (Found by halving: both lists are kept in order.)
const RouteNode* find_route_node(std::span<const RouteNode> nodes, RouteNodeId id);
const RouteLink* find_route_link(std::span<const RouteLink> links, RouteNodeId a, RouteNodeId b); // either way round

// The order links are kept in.
inline bool route_link_before(const RouteLink& l, const RouteLink& r) { return l.a != r.a ? l.a < r.a : l.b < r.b; }

} // namespace giant::world
