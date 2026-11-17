#include "giant/nav/route.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>

namespace giant::nav {

namespace {

constexpr double k_farthest = 2e8; // metres from the origin a point asked about may be (nodes: 1e8)

double distance(const DVec3& a, const DVec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

RouteGraph::RouteGraph(const world::Routes& routes, const RouteSettings& settings) {
    const usize n = routes.nodes.size();
    m_ids.reserve(n);
    m_positions.reserve(n);
    for (const world::RouteNode& node : routes.nodes) {
        const world::CellCoord cell = world::cell_of(node.position);
        if (m_ids.empty()) m_grid_min = m_grid_max = cell;
        m_grid_min = {std::min(m_grid_min.x, cell.x), std::min(m_grid_min.z, cell.z)};
        m_grid_max = {std::max(m_grid_max.x, cell.x), std::max(m_grid_max.z, cell.z)};
        m_grid[cell].push_back(static_cast<u32>(m_ids.size()));
        m_ids.push_back(node.id);
        m_positions.push_back(node.position);
    }
    // (Written so that a pace that is not a number takes the least too.)
    const float road = settings.road_pace >= 0.1f ? std::min(settings.road_pace, 1000.0f) : 0.1f;
    const float path = settings.path_pace >= 0.1f ? std::min(settings.path_pace, 1000.0f) : 0.1f;
    const float slope = settings.slope_cost >= 0.0f ? std::min(settings.slope_cost, 1000.0f) : 0.0f;
    m_least_pace = std::min(road, path);
    // Each link is an edge from both its ends: counted, then filed.
    m_first.assign(n + 1, 0);
    struct Ends {
        u32 a, b;
        Edge edge;
    };
    std::vector<Ends> ends;
    ends.reserve(routes.links.size());
    for (const world::RouteLink& link : routes.links) {
        const u32 a = index_of(link.a), b = index_of(link.b);
        if (a == k_no_route_node || b == k_no_route_node || a == b) continue; // (checked routes have none such)
        const double length = distance(m_positions[a], m_positions[b]);
        const double rise = std::fabs(m_positions[a].y - m_positions[b].y);
        const double pace = link.kind == world::RouteKind::road ? road : path;
        Edge edge;
        edge.length = static_cast<float>(length);
        edge.cost = static_cast<float>(length * pace + static_cast<double>(slope) * rise);
        ends.push_back({a, b, edge});
        ++m_first[a + 1];
        ++m_first[b + 1];
    }
    for (usize i = 0; i < n; ++i) m_first[i + 1] += m_first[i];
    m_edges.resize(ends.size() * 2);
    std::vector<u32> next(m_first.begin(), m_first.end() - 1); // where each node's next edge goes
    for (const Ends& e : ends) {
        Edge there = e.edge;
        there.to = e.b;
        m_edges[next[e.a]++] = there;
        Edge back = e.edge;
        back.to = e.a;
        m_edges[next[e.b]++] = back;
    }
}

u32 RouteGraph::index_of(world::RouteNodeId id) const {
    const auto it = std::lower_bound(m_ids.begin(), m_ids.end(), id);
    return it == m_ids.end() || *it != id ? k_no_route_node : static_cast<u32>(it - m_ids.begin());
}

std::optional<u32> RouteGraph::nearest(const DVec3& point, double within) const {
    if (m_ids.empty() || !(within >= 0.0)) return std::nullopt;
    // No node is past the world's edge (world::check_route_node), and squares
    // counted to there never reach the end of their numbers.
    if (!(std::fabs(point.x) <= k_farthest) || !(std::fabs(point.z) <= k_farthest) || !std::isfinite(point.y)) {
        return std::nullopt;
    }
    within = std::min(within, 4096.0);
    // Square by square in rings round the point's own, nearest first: once a
    // node is found, no ring whose every square is farther is looked at (one
    // by a road finds it in a few squares, however far `within` reaches).
    // Only squares among those that have nodes are looked at (one far from
    // every road looks at none).
    const world::CellCoord centre = world::cell_of({point.x, 0.0, point.z});
    const i64 x0 = m_grid_min.x, x1 = m_grid_max.x, z0 = m_grid_min.z, z1 = m_grid_max.z;
    const i64 cx = centre.x, cz = centre.z;
    const i64 farthest = std::max(std::max(std::abs(cx - x0), std::abs(cx - x1)), std::max(std::abs(cz - z0), std::abs(cz - z1)));
    const i64 rings = std::min(static_cast<i64>(std::ceil(within / world::k_cell_size)) + 1, farthest);
    double best = within;
    std::optional<u32> found;
    const auto look = [&](i64 x, i64 z) {
        const auto cell = m_grid.find({static_cast<i32>(x), static_cast<i32>(z)});
        if (cell == m_grid.end()) return;
        for (const u32 node : cell->second) {
            const double d = distance(m_positions[node], point);
            // (Of two as near, the one made first: the same whatever order the squares come in.)
            if (d < best || (d == best && (!found || node < *found))) {
                best = d;
                found = node;
            }
        }
    };
    for (i64 ring = 0; ring <= rings; ++ring) {
        if (found && static_cast<double>(ring - 1) * world::k_cell_size > best) break;
        if (ring == 0) {
            look(cx, cz);
            continue;
        }
        for (i64 x = std::max(cx - ring, x0); x <= std::min(cx + ring, x1); ++x) {
            if (cz - ring >= z0 && cz - ring <= z1) look(x, cz - ring);
            if (cz + ring >= z0 && cz + ring <= z1) look(x, cz + ring);
        }
        for (i64 z = std::max(cz - ring + 1, z0); z <= std::min(cz + ring - 1, z1); ++z) {
            if (cx - ring >= x0 && cx - ring <= x1) look(cx - ring, z);
            if (cx + ring >= x0 && cx + ring <= x1) look(cx + ring, z);
        }
    }
    return found;
}

void RouteSearch::start(const RouteGraph& graph, u32 from, u32 to) {
    m_graph = &graph;
    m_to = to;
    m_state = RouteState::searching;
    m_open.clear();
    m_path.clear();
    m_length = 0.0;
    m_cost = 0.0;
    m_opened = 0;
    const usize n = graph.size();
    if (from >= n || to >= n) {
        m_state = RouteState::none;
        return;
    }
    m_so_far.assign(n, std::numeric_limits<double>::infinity());
    m_came_from.assign(n, k_no_route_node);
    m_done.assign(n, 0);
    m_so_far[from] = 0.0;
    // A node is filed once for each link that reaches it at less, at most:
    // room for all now, so no slice allocates.
    m_open.reserve(graph.links() * 2 + 1);
    m_open.push_back({rest(from), from});
}

double RouteSearch::rest(u32 node) const {
    return distance(m_graph->position(node), m_graph->position(m_to)) * static_cast<double>(m_graph->least_pace());
}

RouteState RouteSearch::advance(u32 budget) {
    const auto later = [](const Open& a, const Open& b) { return a.guess > b.guess; };
    for (u64 spent = 0; m_state == RouteState::searching && spent < std::max<u32>(budget, 1);) {
        if (m_open.empty()) {
            m_state = RouteState::none;
            break;
        }
        std::pop_heap(m_open.begin(), m_open.end(), later);
        const u32 node = m_open.back().node;
        m_open.pop_back();
        ++spent;
        if (m_done[node] != 0) continue; // (filed again since at less)
        m_done[node] = 1;
        ++m_opened;
        if (node == m_to) {
            m_cost = m_so_far[node];
            usize nodes = 0; // (counted first: the way's list is made at its size, once)
            for (u32 at = node; at != k_no_route_node; at = m_came_from[at]) ++nodes;
            m_path.reserve(nodes);
            for (u32 at = node; at != k_no_route_node; at = m_came_from[at]) m_path.push_back(at);
            std::reverse(m_path.begin(), m_path.end());
            for (usize i = 1; i < m_path.size(); ++i) {
                m_length += distance(m_graph->position(m_path[i - 1]), m_graph->position(m_path[i]));
            }
            m_state = RouteState::found;
            break;
        }
        const std::span<const RouteGraph::Edge> edges = m_graph->edges(node);
        spent += edges.size();
        for (const RouteGraph::Edge& edge : edges) {
            const double reached = m_so_far[node] + static_cast<double>(edge.cost);
            if (m_done[edge.to] != 0 || reached >= m_so_far[edge.to]) continue;
            m_so_far[edge.to] = reached;
            m_came_from[edge.to] = node;
            m_open.push_back({reached + rest(edge.to), edge.to});
            std::push_heap(m_open.begin(), m_open.end(), later);
        }
    }
    return m_state;
}

} // namespace giant::nav
