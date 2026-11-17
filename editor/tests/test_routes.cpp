// Routes in the editor: roads and paths laid, joined, changed and removed
// by command (undoable, saved with the scene), and the way found along them.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"

#include "giant_test.h"

#include "giant/world/routes.h"

#include <cmath>
#include <string>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        register_scene_commands(commands, state);
        register_route_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
    bool script(const char* name, const Json& args) { // outside the history, as C# scripts run commands
        ai::ExecuteOptions options;
        options.record_history = false;
        return commands.execute(name, args, options).ok;
    }
};

Json at(double x, double y, double z) { return Json::array({x, y, z}); }

} // namespace

GIANT_TEST("route commands: roads laid one after another meet at crossings; changed, removed, undone") {
    Fixture f;
    const world::Routes& routes = f.state.scene.routes;
    // A road east, 3 points.
    ai::CommandResult r = f.run("create_route", {{"points", {at(0, 0, 0), at(100, 0, 0), at(200, 0, 0)}}, {"kind", "road"}, {"width", 6.0}});
    GIANT_CHECK(r.ok && r.value["nodes"] == Json::array({1, 2, 3}) && r.value["made"] == 3 && r.value["links_made"] == 2);
    GIANT_CHECK(routes.nodes.size() == 3 && routes.links.size() == 2 && routes.next_id == 4 &&
                routes.links[0].kind == world::RouteKind::road && routes.links[0].width == 6.0f);
    // A path north from its middle: its first point joins node 2 (0.5 m off); its last comes back to it too.
    r = f.run("create_route", {{"points", {at(100.3, 0, 0.4), at(100, 0, 80), at(100, 0, 160)}}});
    GIANT_CHECK(r.ok && r.value["nodes"] == Json::array({2, 4, 5}) && r.value["made"] == 2 && routes.links.size() == 4);
    // A ring: closed; points of one call join each other; a link there already is kept as it is.
    r = f.run("create_route", {{"points", {at(200, 0, 0), at(200, 0, 80), at(100, 0, 80), at(100, 0, 0.2)}}, {"closed", true}, {"width", 9.0}});
    GIANT_CHECK(r.ok && r.value["nodes"] == Json::array({3, 6, 4, 2}) && r.value["made"] == 1 && r.value["links_made"] == 2);
    GIANT_CHECK(routes.links.size() == 6 && world::find_route_link(routes.links, 2, 3)->width == 6.0f &&
                world::find_route_link(routes.links, 4, 6)->width == 9.0f);
    GIANT_CHECK(!world::check_routes(routes));
    // join 0: never joined; one point: a node alone.
    r = f.run("create_route", {{"points", {at(0, 0, 0)}}, {"join", 0.0}});
    GIANT_CHECK(r.ok && r.value["nodes"] == Json::array({7}) && r.value["links_made"] == 0 && routes.nodes.size() == 7);
    GIANT_CHECK(!f.run("create_route", {{"points", Json::array()}}).ok && !f.run("create_route", {{"points", {at(3e8, 0, 0)}}}).ok);
    GIANT_CHECK(routes.nodes.size() == 7 && routes.next_id == 8);
    // Links made and changed by hand; refused between a node and itself or one not there.
    r = f.run("link_route_nodes", {{"a", 7}, {"b", 1}, {"kind", "road"}});
    GIANT_CHECK(r.ok && r.value["a"] == 1 && r.value["b"] == 7 && r.value["width"] == 2.0 && routes.links.size() == 7);
    r = f.run("link_route_nodes", {{"a", 1}, {"b", 7}, {"width", 3.5}});
    GIANT_CHECK(r.ok && r.value["kind"] == "road" && routes.links.size() == 7 && world::find_route_link(routes.links, 7, 1)->width == 3.5f);
    GIANT_CHECK(!f.run("link_route_nodes", {{"a", 1}, {"b", 1}}).ok && !f.run("link_route_nodes", {{"a", 1}, {"b", 70}}).ok);
    GIANT_CHECK(f.run("undo").ok && world::find_route_link(routes.links, 1, 7)->width == 2.0f);
    GIANT_CHECK(f.run("undo").ok && world::find_route_link(routes.links, 1, 7) == nullptr && f.run("redo").ok && f.run("redo").ok);
    GIANT_CHECK(f.run("unlink_route_nodes", {{"a", 7}, {"b", 1}}).ok && routes.links.size() == 6);
    GIANT_CHECK(!f.run("unlink_route_nodes", {{"a", 7}, {"b", 1}}).ok && f.run("undo").ok && routes.links.size() == 7);
    // A node moved: its links follow.
    GIANT_CHECK(f.run("set_route_node", {{"id", 5}, {"position", at(100, 2, 170)}}).ok && routes.links.size() == 7);
    GIANT_CHECK(world::find_route_node(routes.nodes, 5)->position.z == 170.0 && !f.run("set_route_node", {{"id", 50}, {"position", at(0, 0, 0)}}).ok);
    GIANT_CHECK(f.run("undo").ok && world::find_route_node(routes.nodes, 5)->position.z == 160.0 && f.run("redo").ok);
    // Listed, with each node's links.
    r = f.run("get_routes");
    GIANT_CHECK(r.ok && r.value["count"] == 7 && r.value["nodes"] == 7 && r.value["links"] == 7 && r.value["list"][1]["links"].size() == 3);
    r = f.run("get_routes", {{"near", at(100, 0, 0)}, {"within", 1.0}});
    GIANT_CHECK(r.value["count"] == 1 && r.value["list"][0]["id"] == 2);
    bool east = false;
    for (const Json& l : r.value["list"][0]["links"]) east = east || (l["to"] == 3 && l["kind"] == "road" && l["length"] == 100.0);
    GIANT_CHECK(east);
    r = f.run("get_routes", {{"offset", 5}, {"limit", 1}});
    GIANT_CHECK(r.value["count"] == 7 && r.value["list"].size() == 1 && r.value["list"][0]["id"] == 6);
    // Removed with their links, whole or not at all; undone to what was.
    const world::Routes before = routes;
    GIANT_CHECK(!f.run("remove_route_nodes", {{"ids", {2, 99}}}).ok && !f.run("remove_route_nodes", {{"ids", {2, 2}}}).ok);
    r = f.run("remove_route_nodes", {{"ids", {4, 2}}});
    GIANT_CHECK(r.ok && r.value["removed"] == 2 && r.value["links_removed"] == 5 && routes.nodes.size() == 5 && routes.links.size() == 2);
    GIANT_CHECK(!world::check_routes(routes) && f.run("undo").ok && routes == before && f.run("redo").ok && f.run("undo").ok);
    // New nodes never take an id used before, even after undo.
    GIANT_CHECK(f.run("create_route", {{"points", {at(900, 0, 900)}}}).ok && f.run("undo").ok && routes.next_id == 9);
    r = f.run("create_route", {{"points", {at(900, 0, 900)}}});
    GIANT_CHECK(r.ok && r.value["nodes"] == Json::array({9}));
    // Saved and loaded with the scene.
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(f.state.scene), loaded).has_value() && loaded.routes == routes);
}

GIANT_TEST("route commands: get_route finds the cheapest way between points or nodes, or says why not") {
    Fixture f;
    // A path straight across a 100 m gap, or the road round by the north (300 m): the path is cheaper.
    GIANT_CHECK(f.run("create_route", {{"points", {at(0, 0, 0), at(100, 0, 0)}}}).ok);
    GIANT_CHECK(f.run("create_route", {{"points", {at(0, 0, 0), at(0, 0, 100), at(100, 0, 100), at(100, 0, 0)}}, {"kind", "road"}}).ok);
    GIANT_CHECK(f.run("create_route", {{"points", {at(5000, 0, 5000), at(5100, 0, 5000)}}}).ok); // an island
    ai::CommandResult r = f.run("get_route", {{"from", at(3, 0, -4)}, {"to", at(98, 1, 2)}});
    GIANT_CHECK(r.ok && r.value["found"] == true && r.value["nodes"] == Json::array({1, 2}) && r.value["length"] == 100.0 &&
                std::fabs(r.value["cost"].get<double>() - 130.0) < 1e-3 && r.value["points"][1][0] == 100.0);
    GIANT_CHECK(f.run("unlink_route_nodes", {{"a", 1}, {"b", 2}}).ok);
    r = f.run("get_route", {{"from", 1}, {"to", 2}});
    GIANT_CHECK(r.ok && r.value["nodes"] == Json::array({1, 3, 4, 2}) && r.value["length"] == 300.0);
    r = f.run("get_route", {{"from", 1}, {"to", at(5100, 0, 5001)}});
    GIANT_CHECK(r.ok && r.value["found"] == false && r.value.contains("why"));
    // Refused: no node near enough, no such node, neither a point nor an id.
    GIANT_CHECK(!f.run("get_route", {{"from", at(1000, 0, 1000)}, {"to", 2}}).ok);
    GIANT_CHECK(f.run("get_route", {{"from", at(1000, 0, 1000)}, {"to", 2}, {"within", 4000.0}}).ok);
    GIANT_CHECK(!f.run("get_route", {{"from", 77}, {"to", 2}}).ok && !f.run("get_route", {{"from", "home"}, {"to", 2}}).ok);
    GIANT_CHECK(!f.run("get_route", {{"from", Json::array({1, 2})}, {"to", 2}}).ok && !f.run("get_route", {{"from", 0}, {"to", 2}}).ok);
}

GIANT_TEST("route commands: undo after commands outside the history never leaves routes that cannot be loaded") {
    Fixture f;
    world::Routes& routes = f.state.scene.routes;
    GIANT_CHECK(f.run("create_route", {{"points", {at(0, 0, 0), at(50, 0, 0)}}}).ok);       // 1 - 2
    GIANT_CHECK(f.run("unlink_route_nodes", {{"a", 1}, {"b", 2}}).ok);
    // A node of the link is removed outside the history: undoing the unlink brings no link to nowhere.
    GIANT_CHECK(f.script("remove_route_nodes", {{"ids", {2}}}) && f.run("undo").ok);
    GIANT_CHECK(routes.links.empty() && !world::check_routes(routes));
    // Undoing the road's making takes what is left of it; redoing brings it back whole.
    GIANT_CHECK(f.run("undo").ok && routes.nodes.empty() && f.run("redo").ok && routes.nodes.size() == 2 && routes.links.size() == 1);
    GIANT_CHECK(!world::check_routes(routes));
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(f.state.scene), loaded).has_value() && loaded.routes == routes);
}

GIANT_TEST("route commands: a point far past the world is refused at once, never searched for") {
    Fixture f;
    GIANT_CHECK(f.run("create_route", {{"points", {at(0, 0, 0)}}}).ok);
    // (Squares counted out to such a point would never end.)
    GIANT_CHECK(!f.run("create_route", {{"points", {at(2e11, 0, 0)}}}).ok && !f.run("create_route", {{"points", {at(0, 0, 1e300)}}}).ok);
    GIANT_CHECK(!f.run("get_route", {{"from", at(1e300, 0, 0)}, {"to", 1}}).ok && !f.run("get_route", {{"from", 1}, {"to", at(0, 0, 2e11)}}).ok);
    GIANT_CHECK(!f.run("get_route", {{"from", at(0, 1e300, 0)}, {"to", 1}}).ok);
    GIANT_CHECK(f.state.scene.routes.nodes.size() == 1);
    // A node to itself is a way of no length; an id may be written as any whole number.
    const ai::CommandResult same = f.run("get_route", {{"from", 1.0}, {"to", at(0.2, 0, 0)}, {"within", 0.5}});
    GIANT_CHECK(same.ok && same.value["found"] == true && same.value["nodes"].size() == 1 && same.value["length"] == 0.0);
    GIANT_CHECK(!f.run("get_route", {{"from", 1.5}, {"to", 1}}).ok && !f.run("get_route", {{"from", at(0.2, 0, 0)}, {"to", 1}, {"within", 0}}).ok);
    GIANT_CHECK(!f.run("get_routes", {{"within", 5}}).ok && f.run("get_routes", {{"near", at(0, 0, 0)}, {"within", 5}}).ok);
}

GIANT_TEST("route commands: a command that changes nothing makes no undo step; rings and joined points") {
    Fixture f;
    world::Routes& routes = f.state.scene.routes;
    GIANT_CHECK(f.run("create_route", {{"points", {at(0, 0, 0), at(50, 0, 0)}}, {"closed", true}}).ok); // two points: one link
    GIANT_CHECK(routes.nodes.size() == 2 && routes.links.size() == 1);
    const auto steps = [&] { return f.run("get_history").value["undo"].size(); };
    const usize before = steps();
    // Every point joins a node there already and the link is there; the link as it is; the node where it is.
    const ai::CommandResult again = f.run("create_route", {{"points", {at(0.1, 0, 0), at(50, 0, 0.1)}}});
    GIANT_CHECK(again.ok && again.value["made"] == 0 && again.value["links_made"] == 0 && again.value["nodes"] == Json::array({1, 2}));
    GIANT_CHECK(f.run("link_route_nodes", {{"a", 2}, {"b", 1}, {"width", 2.0}, {"kind", "path"}}).ok);
    GIANT_CHECK(f.run("set_route_node", {{"id", 1}, {"position", at(0, 0, 0)}}).ok && steps() == before);
    // Points that all join one node: the node, no link to itself.
    const ai::CommandResult one = f.run("create_route", {{"points", {at(200, 0, 0), at(200.2, 0, 0), at(200, 0, 0.2)}}, {"closed", true}});
    GIANT_CHECK(one.ok && one.value["made"] == 1 && one.value["links_made"] == 0 && routes.nodes.size() == 3 && steps() == before + 1);
    GIANT_CHECK(!world::check_routes(routes));
}

GIANT_TEST("route commands: the most nodes, links and ids a scene may have are kept, by redo too") {
    Fixture f;
    world::Routes& routes = f.state.scene.routes;
    // A road of 1024 nodes, undone; then the scene filled outside the history; redo would pass the most.
    Json points = Json::array();
    for (int i = 0; i < 1024; ++i) points.push_back(at(i * 10.0, 0, 0));
    GIANT_CHECK(f.run("create_route", {{"points", points}}).ok && f.run("undo").ok && routes.nodes.empty());
    for (u32 i = 0; i < world::k_max_route_nodes; ++i) routes.nodes.push_back({2000 + i, {i * 10.0, 0.0, 5000.0}});
    routes.next_id = 2000 + world::k_max_route_nodes;
    GIANT_CHECK(!world::check_routes(routes));
    GIANT_CHECK(f.run("redo").ok && routes.nodes.size() == world::k_max_route_nodes && routes.links.empty() && !world::check_routes(routes));
    // Full: no node more by command either, but a point that joins makes none.
    GIANT_CHECK(!f.run("create_route", {{"points", {at(0, 0, -900)}}}).ok);
    GIANT_CHECK(f.run("create_route", {{"points", {at(0, 0, 5000.5)}}}).ok && routes.nodes.size() == world::k_max_route_nodes);
    // The links' most: filled, then one more refused, one there already still changed.
    routes.nodes.resize(1000);
    for (u32 a = 0; a < 1000 && routes.links.size() < world::k_max_route_links; ++a) {
        for (u32 b = a + 1; b < 1000 && routes.links.size() < world::k_max_route_links; ++b) {
            world::RouteLink l;
            l.a = 2000 + a;
            l.b = 2000 + b;
            routes.links.push_back(l);
        }
    }
    GIANT_CHECK(routes.links.size() == world::k_max_route_links && !world::check_routes(routes));
    const world::RouteLink last = routes.links.back();
    GIANT_CHECK(!f.run("link_route_nodes", {{"a", 2998}, {"b", 2999}}).ok && !f.run("create_route", {{"points", {at(9980, 0, 5000), at(9990, 0, 5000)}}}).ok);
    GIANT_CHECK(f.run("link_route_nodes", {{"a", last.a}, {"b", last.b}, {"kind", "road"}}).ok);
    GIANT_CHECK(routes.links.size() == world::k_max_route_links && routes.links.back().kind == world::RouteKind::road);
    // A link taken out by command, the room filled outside the history: undoing it would pass the most.
    GIANT_CHECK(f.run("unlink_route_nodes", {{"a", last.a}, {"b", last.b}}).ok);
    world::RouteLink other;
    other.a = 2998;
    other.b = 2999;
    routes.links.push_back(other);
    GIANT_CHECK(!world::check_routes(routes) && f.run("undo").ok && routes.links.size() == world::k_max_route_links);
    GIANT_CHECK(!world::check_routes(routes) && world::find_route_link(routes.links, last.a, last.b) == nullptr);
    // The ids run out before they wrap.
    routes.next_id = 0xffffffffu;
    routes.nodes.resize(10);
    routes.links.clear();
    GIANT_CHECK(!f.run("create_route", {{"points", {at(0, 0, -900)}}}).ok && !world::check_routes(routes));
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(f.state.scene), loaded).has_value() && loaded.routes == routes);
}

GIANT_TEST("route commands: many changed at once among others, undone to the same; a change to one since removed") {
    Fixture f;
    world::Routes& routes = f.state.scene.routes;
    Json points = Json::array();
    for (int i = 0; i < 100; ++i) points.push_back(at(i * 10.0, 0, 0));
    GIANT_CHECK(f.run("create_route", {{"points", points}, {"closed", true}}).ok && routes.links.size() == 100);
    const world::Routes before = routes;
    // Twenty from the middle (every other one, so links between the rest go too), one of them asked for first.
    Json ids = Json::array({61});
    for (int id = 31; id < 70; id += 2) {
        if (id != 61) ids.push_back(id);
    }
    const ai::CommandResult removed = f.run("remove_route_nodes", {{"ids", ids}});
    GIANT_CHECK(removed.ok && removed.value["removed"] == 20 && removed.value["links_removed"] == 40);
    GIANT_CHECK(routes.nodes.size() == 80 && routes.links.size() == 60 && !world::check_routes(routes));
    GIANT_CHECK(f.run("undo").ok && routes == before && f.run("redo").ok && routes.nodes.size() == 80 && f.run("undo").ok && routes == before);
    // A road laid across them joins many there already and makes links among theirs: undone to the same too.
    Json across = Json::array();
    for (int i = 0; i < 30; ++i) across.push_back(at(i * 30.0, 0, i % 2 == 0 ? 0.0 : 40.0));
    GIANT_CHECK(f.run("create_route", {{"points", across}}).ok && routes.nodes.size() == 115 && !world::check_routes(routes));
    GIANT_CHECK(f.run("undo").ok && routes.nodes == before.nodes && routes.links == before.links); // (its ids are not given back)
    // A node moved, then removed outside the history: undoing the move brings it back where it was, without links.
    GIANT_CHECK(f.run("set_route_node", {{"id", 5}, {"position", at(40, 0, 7)}}).ok && f.script("remove_route_nodes", {{"ids", {5}}}));
    GIANT_CHECK(f.run("undo").ok && routes.nodes.size() == 100 && routes.links.size() == 98 && !world::check_routes(routes));
    GIANT_CHECK(world::find_route_node(routes.nodes, 5)->position == DVec3(40.0, 0.0, 0.0));
}

GIANT_TEST("route commands: get_routes lists so many links and says when it left some out") {
    Fixture f;
    world::Routes& routes = f.state.scene.routes;
    for (u32 i = 0; i < 400; ++i) routes.nodes.push_back({i + 1, {i * 10.0, 0.0, 0.0}});
    for (u32 a = 1; a <= 400; ++a) {
        for (u32 b = a + 1; b <= 400 && b <= a + 60; ++b) {
            world::RouteLink l;
            l.a = a;
            l.b = b;
            routes.links.push_back(l);
        }
    }
    routes.next_id = 401;
    GIANT_CHECK(!world::check_routes(routes) && routes.links.size() > 20000);
    const auto listed = [](const ai::CommandResult& r) {
        usize links = 0;
        for (const Json& node : r.value["list"]) links += node["links"].size();
        return links;
    };
    const ai::CommandResult all = f.run("get_routes", {{"limit", 400}});
    GIANT_CHECK(all.ok && all.value["links_cut"] == true && listed(all) >= 16384 && listed(all) <= 16386);
    const ai::CommandResult few = f.run("get_routes", {{"limit", 10}});
    GIANT_CHECK(few.ok && few.value["links_cut"] == false && few.value["list"][0]["links"].size() == 60 && listed(few) == 600 + 45); // (a link between two listed is under both)
}
