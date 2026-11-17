// Routes: the way across the whole world along its roads and paths
// (world::Routes: nodes and links kept by the world, not by cells), for
// travel farther than the walkable tiles that are loaded.
//
//   nav::RouteGraph graph(scene.routes);              // when the routes change
//   const auto from = graph.nearest(here, 200.0), to = graph.nearest(there, 200.0);
//   nav::RouteSearch search(graph, *from, *to);
//   while (search.state() == nav::RouteState::searching) search.advance(2048); // a budget a frame
//   for (const u32 node : search.path()) graph.position(node);
//
// A link costs its length times its kind's pace (a road is quicker going
// than a path), more the more it rises or falls. The search is A* in bounded slices
// (rule 9), the straight line at the quickest pace as its guess, so the
// way found is the cheapest.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/cell.h"
#include "giant/world/scene.h"

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace giant::nav {

struct RouteSettings {
    float road_pace = 1.0f;  // cost a metre of road (0.1 to 1000)
    float path_pace = 1.3f;  // cost a metre of path (0.1 to 1000)
    float slope_cost = 3.0f; // cost a metre a link rises or falls, on top (0: slopes are free; at most 1000)
};

inline constexpr u32 k_no_route_node = 0xffffffffu;

class RouteGraph {
public:
    RouteGraph() = default;
    // Built once through the nodes and links (which must pass world::check_routes).
    explicit RouteGraph(const world::Routes& routes, const RouteSettings& settings = {});

    struct Edge {
        u32 to = 0;          // a node's index
        float length = 0.0f; // metres, along the link
        float cost = 0.0f;   // length by pace and slope
    };

    usize size() const { return m_ids.size(); } // nodes
    usize links() const { return m_edges.size() / 2; }
    world::RouteNodeId id(u32 node) const { return m_ids[node]; }
    const DVec3& position(u32 node) const { return m_positions[node]; }
    std::span<const Edge> edges(u32 node) const {
        return {m_edges.data() + m_first[node], m_edges.data() + m_first[node + 1]};
    }
    // The index of the node with this id, or k_no_route_node.
    u32 index_of(world::RouteNodeId id) const;
    // The node nearest `point` within `within` metres (at most 4096), if any
    // (none for a point that is not finite or past 200,000 km out).
    std::optional<u32> nearest(const DVec3& point, double within) const;
    // The least a metre costs (the search's guess is the straight line at this).
    float least_pace() const { return m_least_pace; }

private:
    std::vector<world::RouteNodeId> m_ids; // rising
    std::vector<DVec3> m_positions;
    std::vector<u32> m_first; // into m_edges, size() + 1
    std::vector<Edge> m_edges;
    // Nodes by the 64 m square they are in (for nearest).
    std::unordered_map<world::CellCoord, std::vector<u32>, world::CellCoordHash> m_grid;
    float m_least_pace = 1.0f;
};

enum class RouteState : u8 { searching, found, none };

class RouteSearch {
public:
    RouteSearch() = default; // none, until start
    // From and to are node indices of `graph`, which must outlive the search unchanged.
    RouteSearch(const RouteGraph& graph, u32 from, u32 to) { start(graph, from, to); }
    // Begins another search in this one's lists (their room is kept: a
    // search over the same graph allocates nothing but its way).
    void start(const RouteGraph& graph, u32 from, u32 to);

    // Does about `budget` of work (1 a node taken from the open ones, 1 a
    // link looked along; always a node at least, and the node begun is
    // finished: a slice is past its budget by one node's links at most).
    // Nothing is allocated but the way's list, once, in the slice that finds
    // it. Returns the state after.
    RouteState advance(u32 budget);
    RouteState state() const { return m_state; }
    // Found: the nodes in order, from first, to last.
    const std::vector<u32>& path() const { return m_path; }
    double length() const { return m_length; } // metres along the way found
    double cost() const { return m_cost; }
    u32 opened() const { return m_opened; } // nodes opened so far

private:
    struct Open {
        double guess = 0.0; // cost so far + the guess of the rest
        u32 node = 0;
    };
    double rest(u32 node) const;

    const RouteGraph* m_graph = nullptr;
    u32 m_to = 0;
    RouteState m_state = RouteState::none;
    std::vector<double> m_so_far; // by node; infinity: not reached (double: a way across the world is long)
    std::vector<u32> m_came_from; // by node
    std::vector<u8> m_done;       // by node
    std::vector<Open> m_open;     // a heap, least guess first
    std::vector<u32> m_path;
    double m_length = 0.0;
    double m_cost = 0.0;
    u32 m_opened = 0;
};

} // namespace giant::nav
