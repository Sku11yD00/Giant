// Routes across the world (nav/route.h): the graph built from the world's
// nodes and links, the nearest node, and the cheapest way in bounded slices.
#include "giant/nav/route.h"

#include "giant/world/routes.h"

#include "giant_test.h"

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace giant;
using namespace giant::nav;

namespace {

world::RouteLink link(u32 a, u32 b, world::RouteKind kind = world::RouteKind::path) {
    world::RouteLink l;
    l.a = a;
    l.b = b;
    l.kind = kind;
    return l;
}

std::vector<world::RouteNodeId> ids(const RouteGraph& graph, const RouteSearch& search) {
    std::vector<world::RouteNodeId> out;
    for (const u32 node : search.path()) out.push_back(graph.id(node));
    return out;
}

RouteSearch whole(const RouteGraph& graph, world::RouteNodeId from, world::RouteNodeId to) {
    RouteSearch search(graph, graph.index_of(from), graph.index_of(to));
    while (search.state() == RouteState::searching) search.advance(8);
    return search;
}

} // namespace

GIANT_TEST("route: the graph has every link both ways, finds nodes by id and the nearest by place") {
    world::Routes routes;
    routes.nodes = {{2, {0, 0, 0}}, {3, {30, 4, 40}}, {7, {300, 0, 0}}, {8, {-5000, 0, 9000}}};
    routes.links = {link(2, 3), link(3, 7, world::RouteKind::road)};
    const RouteGraph graph(routes);
    GIANT_CHECK(graph.size() == 4 && graph.links() == 2);
    GIANT_CHECK(graph.index_of(3) == 1 && graph.index_of(8) == 3 && graph.index_of(4) == k_no_route_node &&
                graph.index_of(0) == k_no_route_node);
    GIANT_CHECK(graph.edges(0).size() == 1 && graph.edges(1).size() == 2 && graph.edges(2).size() == 1 && graph.edges(3).empty());
    const RouteGraph::Edge& up = graph.edges(0)[0];
    const float length = std::sqrt(30.0f * 30.0f + 4.0f * 4.0f + 40.0f * 40.0f);
    GIANT_CHECK(up.to == 1 && std::fabs(up.length - length) < 1e-3f && std::fabs(up.cost - (length * 1.3f + 3.0f * 4.0f)) < 1e-2f);
    GIANT_CHECK(graph.edges(1)[0].to == 0 && graph.edges(1)[0].cost == up.cost); // back down costs the same
    GIANT_CHECK(graph.least_pace() == 1.0f);
    // The nearest within reach, across the 64 m squares; none beyond.
    GIANT_CHECK(graph.nearest({29, 4, 41}, 5.0) == 1u && graph.nearest({250, 0, 0}, 200.0) == 2u);
    GIANT_CHECK(graph.nearest({140, 0, -90}, 200.0) == 0u && !graph.nearest({140, 0, -90}, 100.0));
    GIANT_CHECK(graph.nearest({-5000, 0, 9000}, 0.0) == 3u && !graph.nearest({0, 0, 0}, -1.0));
    const RouteGraph empty{world::Routes{}};
    GIANT_CHECK(empty.size() == 0 && !empty.nearest({0, 0, 0}, 100.0));
    RouteSearch nothing(empty, 0, 0);
    GIANT_CHECK(nothing.state() == RouteState::none && nothing.advance(10) == RouteState::none);
}

GIANT_TEST("route: the cheapest way: a road round beats a path across, a climb costs, an island has none") {
    // A square 100 m a side: 1 - 2 by a path straight across, or 1 - 3 - 4 - 2 round by road (300 m).
    world::Routes routes;
    routes.nodes = {{1, {0, 0, 0}}, {2, {100, 0, 0}}, {3, {0, 0, 100}}, {4, {100, 0, 100}}, {5, {50, 40, 0}}, {9, {900, 0, 900}}};
    routes.links = {link(1, 2), link(1, 3, world::RouteKind::road), link(2, 4, world::RouteKind::road),
                    link(3, 4, world::RouteKind::road)};
    {
        const RouteGraph graph(routes);
        RouteSearch search = whole(graph, 1, 2);
        GIANT_CHECK(search.state() == RouteState::found && ids(graph, search) == (std::vector<world::RouteNodeId>{1, 2}));
        GIANT_CHECK(std::fabs(search.length() - 100.0) < 1e-6 && std::fabs(search.cost() - 130.0) < 1e-3);
        GIANT_CHECK(whole(graph, 1, 9).state() == RouteState::none); // an island
        RouteSearch same = whole(graph, 4, 4);
        GIANT_CHECK(same.state() == RouteState::found && same.path().size() == 1 && same.length() == 0.0);
    }
    {
        // Paths made dear: the road round wins.
        RouteSettings settings;
        settings.path_pace = 4.0f;
        const RouteGraph graph(routes, settings);
        RouteSearch search = whole(graph, 1, 2);
        GIANT_CHECK(ids(graph, search) == (std::vector<world::RouteNodeId>{1, 3, 4, 2}) && std::fabs(search.length() - 300.0) < 1e-6);
    }
    {
        // Over the hill (1 - 5 - 2, 128 m of path climbing and dropping 40 m) or the flat path: flat wins;
        // with slopes free and the flat path gone, over the hill.
        routes.links = {link(1, 2), link(1, 5), link(2, 5)};
        const RouteGraph graph(routes);
        GIANT_CHECK(ids(graph, whole(graph, 1, 2)) == (std::vector<world::RouteNodeId>{1, 2}));
        routes.links = {link(1, 3), link(1, 5), link(2, 4), link(2, 5), link(3, 4)};
        const RouteGraph hilly(routes);
        GIANT_CHECK(ids(hilly, whole(hilly, 1, 2)) == (std::vector<world::RouteNodeId>{1, 3, 4, 2})); // 390 against 406
        RouteSettings free_slopes;
        free_slopes.slope_cost = 0.0f;
        const RouteGraph easy(routes, free_slopes);
        GIANT_CHECK(ids(easy, whole(easy, 1, 2)) == (std::vector<world::RouteNodeId>{1, 5, 2}));
    }
}

GIANT_TEST("route: a long way is searched in slices, and is the cheapest over a big grid") {
    // A 100 x 100 grid of nodes 50 m apart, paths along the rows and columns, one row a road.
    world::Routes routes;
    const u32 side = 100;
    for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) routes.nodes.push_back({z * side + x + 1, {x * 50.0, 0.0, z * 50.0}});
    }
    for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) {
            const u32 id = z * side + x + 1;
            if (x + 1 < side) routes.links.push_back(link(id, id + 1, z == 60 ? world::RouteKind::road : world::RouteKind::path));
            if (z + 1 < side) routes.links.push_back(link(id, id + side));
        }
    }
    routes.next_id = side * side + 1;
    GIANT_CHECK(!world::check_routes(routes));
    const RouteGraph graph(routes);
    GIANT_CHECK(graph.size() == 10000 && graph.links() == 2 * side * (side - 1));
    // Along the road row, end to end: straight down it.
    RouteSearch search(graph, graph.index_of(60 * side + 1), graph.index_of(60 * side + side));
    u32 slices = 0;
    while (search.advance(16) == RouteState::searching) {
        ++slices;
        GIANT_CHECK(search.opened() <= (slices + 1) * 16);
    }
    GIANT_CHECK(search.state() == RouteState::found && slices >= 5 && search.path().size() == side);
    GIANT_CHECK(std::fabs(search.length() - 4950.0) < 1e-6 && std::fabs(search.cost() - 4950.0) < 0.5);
    // Corner to corner: 198 links whichever way; the cheapest takes the road for its 99 links across.
    RouteSearch corner = whole(graph, 1, side * side);
    GIANT_CHECK(corner.state() == RouteState::found && corner.path().size() == 199 && std::fabs(corner.length() - 9900.0) < 1e-6);
    GIANT_CHECK(std::fabs(corner.cost() - (4950.0 * 1.3 + 4950.0)) < 1.0);
    usize on_road = 0;
    for (const u32 node : corner.path()) on_road += (graph.id(node) - 1) / side == 60 ? 1u : 0u;
    GIANT_CHECK(on_road == side);
}

GIANT_TEST("route: the nearest to a point far past the world, or not a number, is none, at once") {
    world::Routes routes;
    routes.nodes.push_back({1, {0.0, 0.0, 0.0}});
    routes.nodes.push_back({2, {1e8, 0.0, -1e8}}); // the world's corner
    routes.next_id = 3;
    const RouteGraph graph(routes);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    for (const DVec3& far : {DVec3{1.4e11, 0.0, 0.0}, DVec3{0.0, 0.0, 1e300}, DVec3{-1e300, 0.0, 0.0}, DVec3{inf, 0.0, 0.0},
                             DVec3{nan, 0.0, 0.0}, DVec3{0.0, nan, 0.0}, DVec3{0.0, 0.0, -inf}}) {
        GIANT_CHECK(!graph.nearest(far, 4096.0));
    }
    GIANT_CHECK(graph.nearest({1e8 + 100.0, 0.0, -1e8 - 100.0}, 200.0) == std::optional<u32>(1));
    GIANT_CHECK(!graph.nearest({0.0, 0.0, 0.0}, nan) && graph.nearest({0.0, 0.0, 0.0}, 0.0) == std::optional<u32>(0));
    // Paces that are not numbers take the least a pace may be.
    RouteSettings odd;
    odd.road_pace = std::numeric_limits<float>::quiet_NaN();
    odd.path_pace = -1.0f;
    odd.slope_cost = std::numeric_limits<float>::quiet_NaN();
    GIANT_CHECK(RouteGraph(routes, odd).least_pace() == 0.1f);
    odd.road_pace = odd.path_pace = odd.slope_cost = std::numeric_limits<float>::infinity();
    GIANT_CHECK(RouteGraph(routes, odd).least_pace() == 1000.0f);
}

GIANT_TEST("route: a slice's budget counts the links looked along") {
    // A hub with 2000 spokes, and one spoke's end the goal: opening the hub alone is a slice's work.
    world::Routes routes;
    routes.nodes.push_back({1, {0.0, 0.0, 0.0}});
    for (u32 i = 0; i < 2000; ++i) {
        routes.nodes.push_back({i + 2, {std::cos(i * 0.003) * 500.0, 0.0, std::sin(i * 0.003) * 500.0}});
        routes.links.push_back(link(1, i + 2));
    }
    routes.next_id = 2002;
    GIANT_CHECK(!world::check_routes(routes));
    const RouteGraph graph(routes);
    RouteSearch search(graph, graph.index_of(1), graph.index_of(2001));
    GIANT_CHECK(search.advance(64) == RouteState::searching && search.opened() == 1); // the hub's links were the slice
    u32 slices = 1;
    while (search.advance(64) == RouteState::searching) ++slices;
    GIANT_CHECK(search.state() == RouteState::found && search.path().size() == 2 && slices <= 2001 / 32 + 2);
}
