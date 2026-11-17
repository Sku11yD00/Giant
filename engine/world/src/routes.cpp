#include "giant/world/routes.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace giant::world {

std::string_view route_kind_name(RouteKind kind) { return kind == RouteKind::road ? "road" : "path"; }

std::optional<RouteKind> route_kind_from_name(std::string_view name) {
    if (name == "path") return RouteKind::path;
    if (name == "road") return RouteKind::road;
    return std::nullopt;
}

std::optional<std::string> check_route_node(const RouteNode& node) {
    if (node.id == 0) return "id must not be 0";
    const DVec3& p = node.position;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::fabs(p.x) > 1e8 ||
        std::fabs(p.y) > 1e8 || std::fabs(p.z) > 1e8) {
        return "position must be finite and within 100,000 km";
    }
    return std::nullopt;
}

std::optional<std::string> check_route_link(const RouteLink& link) {
    if (link.a == 0 || link.a >= link.b) return "a link joins two nodes, the lower id first";
    if (!std::isfinite(link.width) || link.width < 0.5f || link.width > 100.0f) {
        return "width must be from 0.5 to 100 metres";
    }
    if (link.kind != RouteKind::path && link.kind != RouteKind::road) return "kind must be path or road";
    return std::nullopt;
}

std::optional<std::string> check_routes(const Routes& routes) {
    if (routes.nodes.size() > k_max_route_nodes) return "at most " + std::to_string(k_max_route_nodes) + " route nodes";
    if (routes.links.size() > k_max_route_links) return "at most " + std::to_string(k_max_route_links) + " route links";
    RouteNodeId last = 0;
    for (const RouteNode& n : routes.nodes) {
        if (n.id <= last) return "route nodes must be in the order of their ids, each once (node " + std::to_string(n.id) + ")";
        last = n.id;
    }
    if (routes.next_id == 0 || last >= routes.next_id) return "the routes' next_id must be past every node's id";
    const RouteLink* before = nullptr;
    for (const RouteLink& l : routes.links) {
        const std::string name = "link " + std::to_string(l.a) + "-" + std::to_string(l.b);
        if (before != nullptr && !route_link_before(*before, l)) {
            return "route links must be in the order of their nodes, each pair once (" + name + ")";
        }
        before = &l;
        if (find_route_node(routes.nodes, l.a) == nullptr || find_route_node(routes.nodes, l.b) == nullptr) {
            return name + " joins a node that is not there";
        }
    }
    return std::nullopt;
}

const RouteNode* find_route_node(std::span<const RouteNode> nodes, RouteNodeId id) {
    const auto it = std::lower_bound(nodes.begin(), nodes.end(), id, [](const RouteNode& n, RouteNodeId v) { return n.id < v; });
    return it == nodes.end() || it->id != id ? nullptr : &*it;
}

const RouteLink* find_route_link(std::span<const RouteLink> links, RouteNodeId a, RouteNodeId b) {
    if (a > b) std::swap(a, b);
    RouteLink key;
    key.a = a;
    key.b = b;
    const auto it = std::lower_bound(links.begin(), links.end(), key, route_link_before);
    return it == links.end() || it->a != a || it->b != b ? nullptr : &*it;
}

} // namespace giant::world
