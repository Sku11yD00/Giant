// The world's roads and paths (world/routes.h): checking nodes and links,
// finding them, and saving them with the scene and the world.
#include "giant/world/routes.h"

#include "giant_test.h"

#include <limits>
#include <string>
#include <utility>

using namespace giant;
using namespace giant::world;

namespace {

Routes crossroads() {
    Routes r;
    r.nodes = {{1, {0, 0, 0}}, {2, {50, 1.5, 0}}, {5, {50, 0, 60.25}}, {9, {-1e6, 3, 2e6}}};
    r.links = {{1, 2, 4.0f, RouteKind::road}, {2, 5, 2.0f, RouteKind::path}, {2, 9, 0.5f, RouteKind::path}};
    r.next_id = 12;
    return r;
}

std::string load_error(const std::string& routes) {
    std::string text = save_scene(Scene{});
    text = text.substr(0, text.rfind('}')) + ",\"routes\":" + routes + "}";
    Scene scene;
    const auto error = load_scene(text, scene);
    return error ? error->message : std::string();
}

} // namespace

GIANT_TEST("routes: nodes and links are checked alone and together, and found by halving") {
    GIANT_CHECK(!check_routes(crossroads()) && !check_routes(Routes{}));
    GIANT_CHECK(check_route_node({0, {0, 0, 0}}) && check_route_node({1, {2e8, 0, 0}}));
    GIANT_CHECK(check_route_node({1, {0, std::numeric_limits<double>::quiet_NaN(), 0}}));
    GIANT_CHECK(check_route_link({2, 1, 2.0f, RouteKind::path}) && check_route_link({1, 1, 2.0f, RouteKind::path}));
    GIANT_CHECK(check_route_link({0, 1, 2.0f, RouteKind::path}) && check_route_link({1, 2, 0.1f, RouteKind::path}));
    GIANT_CHECK(check_route_link({1, 2, 200.0f, RouteKind::road}) && !check_route_link({1, 2, 100.0f, RouteKind::road}));
    Routes r = crossroads();
    r.next_id = 9;
    GIANT_CHECK(check_routes(r)); // a node not under the next id
    r = crossroads();
    std::swap(r.nodes[1], r.nodes[2]);
    GIANT_CHECK(check_routes(r));
    r = crossroads();
    r.nodes[2].id = 2; // an id twice
    GIANT_CHECK(check_routes(r));
    r = crossroads();
    std::swap(r.links[0], r.links[1]);
    GIANT_CHECK(check_routes(r));
    r = crossroads();
    r.links[2] = r.links[1]; // a pair twice
    GIANT_CHECK(check_routes(r));
    r = crossroads();
    r.links[2].b = 10; // an end that is no node
    GIANT_CHECK(check_routes(r));

    r = crossroads();
    GIANT_CHECK(find_route_node(r.nodes, 5) == &r.nodes[2] && find_route_node(r.nodes, 3) == nullptr &&
                find_route_node(r.nodes, 0) == nullptr && find_route_node(r.nodes, 10) == nullptr);
    GIANT_CHECK(find_route_link(r.links, 2, 5) == &r.links[1] && find_route_link(r.links, 5, 2) == &r.links[1]);
    GIANT_CHECK(find_route_link(r.links, 1, 5) == nullptr && find_route_link(r.links, 9, 2) == &r.links[2]);
    GIANT_CHECK(route_kind_from_name("road") == RouteKind::road && route_kind_from_name("path") == RouteKind::path &&
                !route_kind_from_name("lane") && route_kind_name(RouteKind::road) == "road");
}

GIANT_TEST("routes: saved and loaded with the scene and the world; a wrong file is refused, saying where") {
    Scene scene;
    scene.routes = crossroads();
    const std::string text = save_scene(scene);
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value() && loaded.routes == scene.routes && save_scene(loaded) == text);
    Scene copy(loaded);
    Scene moved(std::move(copy));
    Scene assigned;
    assigned = std::move(moved);
    GIANT_CHECK(assigned.routes == scene.routes);
    GIANT_CHECK(save_scene(Scene{}).find("routes") == std::string::npos);
    Scene emptied; // ids once given are kept
    emptied.routes.next_id = 4;
    GIANT_CHECK(!load_scene(save_scene(emptied), loaded).has_value() && loaded.routes.next_id == 4 && loaded.routes.nodes.empty());
    WorldIndex world;
    world.routes = crossroads();
    WorldIndex world_loaded;
    GIANT_CHECK(!load_world_index(save_world_index(world), world_loaded).has_value() && world_loaded.routes == world.routes);

    const auto refused = [](const std::string& routes, const char* where) {
        const std::string error = load_error(routes);
        return !error.empty() && error.find(where) != std::string::npos;
    };
    GIANT_CHECK(load_error(R"({"next_id":3,"nodes":[[1,0,0,0],[2,5,0,0]],"links":[[1,2,2,"road"]]})").empty());
    GIANT_CHECK(refused(R"({"nodes":[],"links":[]})", "routes.next_id"));
    GIANT_CHECK(refused(R"({"next_id":0,"nodes":[],"links":[]})", "routes.next_id"));
    GIANT_CHECK(refused(R"({"next_id":1,"links":[]})", "routes.nodes"));
    GIANT_CHECK(refused(R"({"next_id":1,"nodes":[]})", "routes.links"));
    GIANT_CHECK(refused(R"({"next_id":1,"nodes":{},"links":[]})", "routes.nodes"));
    GIANT_CHECK(refused(R"({"next_id":1,"nodes":[],"links":[],"lanes":1})", "routes.lanes"));
    GIANT_CHECK(refused(R"([])", "routes"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1,0,0]],"links":[]})", "routes.nodes[0]"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1.5,0,0,0]],"links":[]})", "routes.nodes[0]"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1,0,"0",0]],"links":[]})", "routes.nodes[0]"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1,0,0,1e300]],"links":[]})", "routes.nodes[0]"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[4294967296,0,0,0]],"links":[]})", "routes.nodes[0]"));
    GIANT_CHECK(refused(R"({"next_id":2,"nodes":[[1,0,0,0],[2,5,0,0]],"links":[]})", "next_id"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[2,0,0,0],[1,5,0,0]],"links":[]})", "order"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1,0,0,0],[2,5,0,0]],"links":[[1,2,2]]})", "routes.links[0]"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1,0,0,0],[2,5,0,0]],"links":[[2,1,2,"road"]]})", "routes.links[0]"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1,0,0,0],[2,5,0,0]],"links":[[1,2,1e300,"road"]]})", "width"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1,0,0,0],[2,5,0,0]],"links":[[1,2,2,"lane"]]})", "kind"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1,0,0,0],[2,5,0,0]],"links":[[1,3,2,"road"]]})", "not there"));
    GIANT_CHECK(refused(R"({"next_id":3,"nodes":[[1,0,0,0],[2,5,0,0]],"links":[[1,2,2,"road"],[1,2,3,"path"]]})", "once"));
    GIANT_CHECK(refused(R"({"next_id":-3,"nodes":[],"links":[]})", "routes.next_id"));
    GIANT_CHECK(refused(R"({"next_id":2.5,"nodes":[],"links":[]})", "routes.next_id"));
    GIANT_CHECK(refused(R"({"next_id":4294967296,"nodes":[],"links":[]})", "routes.next_id"));
    GIANT_CHECK(load_error(R"({"next_id":4294967295,"nodes":[[4294967294,0,0,0]],"links":[]})").empty()); // the last id there is
    // A world's index refuses the same.
    std::string index = save_world_index(WorldIndex{});
    index = index.substr(0, index.rfind('}')) + R"(,"routes":{"next_id":3,"nodes":[[1,0,0,0]],"links":[[1,2,2,"road"]]}})";
    const auto index_error = load_world_index(index, world_loaded);
    GIANT_CHECK(index_error.has_value() && index_error->message.find("not there") != std::string::npos);
}
