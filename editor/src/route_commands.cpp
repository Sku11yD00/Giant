// Route commands: the world's roads and paths (world/routes.h, nav/route.h;
// living-world plan LW2). Scene data (saved with the scene and the world's
// world.json, undoable). Nodes and links are many, so an undo step keeps
// what one command changed, never the lists.
#include "editor_commands.h"

#include "giant/core/assert.h"
#include "giant/nav/route.h"
#include "giant/world/routes.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
using world::RouteLink;
using world::RouteNode;
using world::RouteNodeId;
using world::Routes;

constexpr usize k_most_points = 1024;   // a road's points, or nodes removed, a call
constexpr usize k_most_listed = 1000;   // nodes one get_routes lists
constexpr usize k_most_links_listed = 16384; // and links, about
constexpr double k_most_snap = 4096.0;  // metres a route's ends may be from a node

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "route command could not be registered");
}

Json id_schema(const char* description) {
    return {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295.0}, {"description", description}};
}
Json point_schema(const char* description) {
    return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
            {"description", description}};
}
Json object_schema(Json required, Json properties) {
    Json schema = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}
Json kind_schema() {
    return {{"enum", {"path", "road"}}, {"description", "A road is quicker going than a path (default path)."}};
}
Json width_schema() {
    return {{"type", "number"}, {"minimum", 0.5}, {"maximum", 100}, {"description", "Metres across (default 2)."}};
}

DVec3 point(const Json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }
Json point_json(const DVec3& p) { return Json::array({p.x, p.y, p.z}); }

double distance(const DVec3& a, const DVec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Json link_json(const RouteLink& l) {
    return {{"a", l.a}, {"b", l.b}, {"width", l.width}, {"kind", std::string(world::route_kind_name(l.kind))}};
}

// What one command changed: the nodes and links it took out and put in
// (each list in the order the scene keeps; one changed is in both).
struct RouteChange {
    std::vector<RouteNode> nodes_out, nodes_in;
    std::vector<RouteLink> links_out, links_in;
    RouteNodeId next_after = 0; // the routes' next id after it (0: as it was); undoing never gives an id back
};

template <typename T, typename Before>
bool among(const std::vector<T>& list, const T& t, Before before) {
    const auto it = std::lower_bound(list.begin(), list.end(), t, before);
    return it != list.end() && !before(t, *it);
}

// How long a list is after swap_sorted with these.
template <typename T, typename Before>
usize size_after(const std::vector<T>& list, const std::vector<T>& out, const std::vector<T>& in, Before before) {
    usize size = list.size();
    for (const T& t : out) size -= among(list, t, before) ? 1u : 0u;
    for (const T& t : in) size += !among(list, t, before) || among(out, t, before) ? 1u : 0u;
    return size;
}

// Takes `out` from a list and puts `in` among it (all three in `before`'s
// order; one of `in` there already takes its place). A few are changed
// where they are; many, once through the list.
template <typename T, typename Before>
void swap_sorted(std::vector<T>& list, const std::vector<T>& out, const std::vector<T>& in, Before before) {
    const auto same = [&](const T& l, const T& r) { return !before(l, r) && !before(r, l); };
    if (out.size() + in.size() <= 8) {
        for (const T& t : out) {
            const auto it = std::lower_bound(list.begin(), list.end(), t, before);
            if (it != list.end() && same(*it, t)) list.erase(it);
        }
        for (const T& t : in) {
            const auto it = std::lower_bound(list.begin(), list.end(), t, before);
            if (it != list.end() && same(*it, t)) {
                *it = t;
            } else {
                list.insert(it, t);
            }
        }
        return;
    }
    std::vector<T> next;
    next.reserve(list.size() + in.size());
    usize o = 0, i = 0;
    for (const T& t : list) {
        while (i < in.size() && before(in[i], t)) next.push_back(in[i++]);
        while (o < out.size() && before(out[o], t)) ++o;
        if (o < out.size() && same(out[o], t)) continue;
        if (i < in.size() && same(in[i], t)) continue; // (there already: `in` wins, next)
        next.push_back(t);
    }
    while (i < in.size()) next.push_back(in[i++]);
    list = std::move(next);
}

// Does a change, or undoes it. Commands run outside the history (scripts)
// may have changed the routes since it was recorded, so what would leave
// them broken is left out: a link an end of which is not there, and the
// whole step if the scene would hold more nodes or links than it may
// (false: nothing changed). Undoing a change to a node or link that is no
// longer there brings it back as it was before (a node without its links).
bool apply(Routes& routes, const RouteChange& change, bool forward) {
    const auto node_before = [](const RouteNode& l, const RouteNode& r) { return l.id < r.id; };
    const std::vector<RouteNode>& nodes_out = forward ? change.nodes_out : change.nodes_in;
    const std::vector<RouteNode>& nodes_in = forward ? change.nodes_in : change.nodes_out;
    const std::vector<RouteLink>& links_out = forward ? change.links_out : change.links_in;
    const std::vector<RouteLink>& given = forward ? change.links_in : change.links_out;
    // The nodes that go (taken out and not put back), and whether a node is there after.
    std::vector<RouteNodeId> gone;
    for (const RouteNode& n : nodes_out) {
        if (!among(nodes_in, n, node_before)) gone.push_back(n.id);
    }
    const auto there_after = [&](RouteNodeId id) {
        RouteNode key;
        key.id = id;
        if (among(nodes_in, key, node_before)) return true;
        return world::find_route_node(routes.nodes, id) != nullptr && !std::binary_search(gone.begin(), gone.end(), id);
    };
    const bool whole = std::all_of(given.begin(), given.end(),
                                   [&](const RouteLink& l) { return there_after(l.a) && there_after(l.b); });
    std::vector<RouteLink> fewer; // (only when a link given has an end gone)
    if (!whole) {
        for (const RouteLink& l : given) {
            if (there_after(l.a) && there_after(l.b)) fewer.push_back(l);
        }
    }
    const std::vector<RouteLink>& links_in = whole ? given : fewer;
    if (size_after(routes.nodes, nodes_out, nodes_in, node_before) > world::k_max_route_nodes ||
        size_after(routes.links, links_out, links_in, world::route_link_before) > world::k_max_route_links) {
        return false;
    }
    swap_sorted(routes.nodes, nodes_out, nodes_in, node_before);
    swap_sorted(routes.links, links_out, links_in, world::route_link_before);
    if (!gone.empty()) { // the links that ended at them go too
        std::erase_if(routes.links, [&](const RouteLink& l) {
            return std::binary_search(gone.begin(), gone.end(), l.a) || std::binary_search(gone.begin(), gone.end(), l.b);
        });
    }
    if (forward) routes.next_id = std::max(routes.next_id, change.next_after);
    return true;
}

void commit(EditorState& state, CommandContext& context, RouteChange change) {
    const bool done = apply(state.scene.routes, change, true);
    GIANT_ASSERT(done, "a route command checks the limits before it commits");
    const auto kept = std::make_shared<const RouteChange>(std::move(change));
    context.record(scene_change(
        state, [&state, kept] { apply(state.scene.routes, *kept, false); },
        [&state, kept] { apply(state.scene.routes, *kept, true); }));
}

// The node of `routes` or of `made` (this command's new ones) nearest
// `p` within `join` metres, or 0.
RouteNodeId joined(const nav::RouteGraph& graph, const std::vector<RouteNode>& made, const DVec3& p, double join) {
    RouteNodeId best = 0;
    double best_distance = join;
    if (const auto near = graph.nearest(p, join)) {
        best = graph.id(*near);
        best_distance = distance(graph.position(*near), p);
    }
    for (const RouteNode& n : made) {
        const double d = distance(n.position, p);
        if (d < best_distance || (d <= best_distance && best == 0)) {
            best = n.id;
            best_distance = d;
        }
    }
    return best;
}

RouteLink link_between(RouteNodeId a, RouteNodeId b, float width, world::RouteKind kind) {
    RouteLink l;
    l.a = std::min(a, b);
    l.b = std::max(a, b);
    l.width = width;
    l.kind = kind;
    return l;
}

} // namespace

void register_route_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"create_route",
         "Lays a road or path: a node at each of `points` (1 to 1024, [x, y, z] on the ground, in order) "
         "and a link between each and the next (`closed`: and from the last to the first). A point within "
         "`join` metres (default 1; 0: never) of a node there already, or of an earlier point of this call, "
         "uses that node, so roads laid one after another meet at crossings and branch from each other. "
         "People travel the world along these (get_route finds the way); links are walked both ways. Keep "
         "nodes where the way turns or the ground's slope changes: about 20 to 100 m apart. Saved with the "
         "scene or the world (at most 65536 nodes, 262144 links). Returns the `nodes` (ids, in the points' "
         "order). One undo step.",
         object_schema({"points"},
                       {{"points", {{"type", "array"}, {"minItems", 1}, {"maxItems", k_most_points},
                                    {"items", point_schema("A point of the way, metres.")}}},
                        {"kind", kind_schema()},
                        {"width", width_schema()},
                        {"closed", {{"type", "boolean"}, {"description", "A ring: the last point is linked to the first."}}},
                        {"join", {{"type", "number"}, {"minimum", 0}, {"maximum", 100}}}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             const Routes& routes = state.scene.routes;
             const auto kind = world::route_kind_from_name(args.value("kind", std::string("path")));
             const float width = args.value("width", 2.0f);
             const double join = args.value("join", 1.0);
             const nav::RouteGraph graph = join > 0.0 ? nav::RouteGraph(routes) : nav::RouteGraph(); // (for the joining)
             RouteChange change;
             std::vector<RouteNodeId> ids;
             RouteNodeId next = routes.next_id;
             for (const Json& p : args["points"]) {
                 RouteNode node;
                 node.position = point(p);
                 node.id = 1; // (for the check: the place is what is asked)
                 if (const auto bad = world::check_route_node(node)) {
                     return CommandResult::failure("points[" + std::to_string(ids.size()) + "]: " + *bad);
                 }
                 RouteNodeId id = join > 0.0 ? joined(graph, change.nodes_in, node.position, join) : 0;
                 if (id == 0) {
                     if (next == 0xffffffffu) return CommandResult::failure("this scene has run out of route node ids");
                     node.id = id = next++;
                     change.nodes_in.push_back(node);
                 }
                 ids.push_back(id);
             }
             if (routes.nodes.size() + change.nodes_in.size() > world::k_max_route_nodes) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_route_nodes) +
                                               " route nodes (" + std::to_string(routes.nodes.size()) + " now)");
             }
             const usize steps = ids.size() - (args.value("closed", false) && ids.size() > 2 ? 0 : 1);
             for (usize i = 0; i < steps; ++i) {
                 const RouteNodeId a = ids[i], b = ids[(i + 1) % ids.size()];
                 if (a == b) continue; // (two points joined to one node)
                 const RouteLink link = link_between(a, b, width, *kind);
                 if (world::find_route_link(routes.links, a, b) != nullptr) continue; // (there already: kept as it is)
                 change.links_in.push_back(link);
             }
             std::sort(change.links_in.begin(), change.links_in.end(), world::route_link_before);
             change.links_in.erase(std::unique(change.links_in.begin(), change.links_in.end(),
                                               [](const RouteLink& l, const RouteLink& r) { return l.a == r.a && l.b == r.b; }),
                                   change.links_in.end());
             if (routes.links.size() + change.links_in.size() > world::k_max_route_links) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_route_links) +
                                               " route links");
             }
             const usize made = change.nodes_in.size(), linked = change.links_in.size();
             change.next_after = next;
             if (made + linked > 0) commit(state, context, std::move(change)); // (all there already: no step)
             return CommandResult::success({{"nodes", ids}, {"made", made}, {"links_made", linked}});
         }});
    add(registry,
        {"link_route_nodes",
         "Links two route nodes (by id), or changes the link between them: `kind`, `width` (those left out "
         "stay, or take their defaults for a new link). Undoable.",
         object_schema({"a", "b"}, {{"a", id_schema("A node.")}, {"b", id_schema("Another.")}, {"kind", kind_schema()},
                                    {"width", width_schema()}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             const Routes& routes = state.scene.routes;
             const RouteNodeId a = args["a"].get<RouteNodeId>(), b = args["b"].get<RouteNodeId>();
             if (a == b) return CommandResult::failure("a link joins two different nodes");
             for (const RouteNodeId id : std::initializer_list<RouteNodeId>{a, b}) {
                 if (world::find_route_node(routes.nodes, id) == nullptr) {
                     return CommandResult::failure("no route node " + std::to_string(id));
                 }
             }
             const RouteLink* had = world::find_route_link(routes.links, a, b);
             if (had == nullptr && routes.links.size() >= world::k_max_route_links) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_route_links) +
                                               " route links");
             }
             RouteLink link = had != nullptr ? *had : link_between(a, b, 2.0f, world::RouteKind::path);
             link.width = args.value("width", link.width);
             if (args.contains("kind")) link.kind = *world::route_kind_from_name(args["kind"].get<std::string>());
             if (had == nullptr || had->width != link.width || had->kind != link.kind) { // (as it is: no step)
                 RouteChange change;
                 if (had != nullptr) change.links_out.push_back(*had);
                 change.links_in.push_back(link);
                 commit(state, context, std::move(change));
             }
             return CommandResult::success(link_json(link));
         }});
    add(registry,
        {"unlink_route_nodes",
         "Removes the link between two route nodes (the nodes stay). Undoable.",
         object_schema({"a", "b"}, {{"a", id_schema("A node.")}, {"b", id_schema("Another.")}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             const RouteNodeId a = args["a"].get<RouteNodeId>(), b = args["b"].get<RouteNodeId>();
             const RouteLink* had = world::find_route_link(state.scene.routes.links, a, b);
             if (had == nullptr) {
                 return CommandResult::failure("no link between route nodes " + std::to_string(a) + " and " + std::to_string(b));
             }
             RouteChange change;
             change.links_out.push_back(*had);
             commit(state, context, std::move(change));
             return CommandResult::success({{"removed", Json::array({std::min(a, b), std::max(a, b)})}});
         }});
    add(registry,
        {"set_route_node",
         "Moves a route node (by id) to `position`; its links follow. Undoable.",
         object_schema({"id", "position"}, {{"id", id_schema("The node.")}, {"position", point_schema("Metres, on the ground.")}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             const RouteNodeId id = args["id"].get<RouteNodeId>();
             const RouteNode* had = world::find_route_node(state.scene.routes.nodes, id);
             if (had == nullptr) return CommandResult::failure("no route node " + std::to_string(id));
             RouteNode moved = *had;
             moved.position = point(args["position"]);
             if (const auto bad = world::check_route_node(moved)) return CommandResult::failure(*bad);
             if (moved.position != had->position) { // (where it is: no step)
                 RouteChange change;
                 change.nodes_out.push_back(*had);
                 change.nodes_in.push_back(moved); // (the same node: its links stay)
                 commit(state, context, std::move(change));
             }
             return CommandResult::success({{"id", id}, {"position", point_json(moved.position)}});
         }});
    add(registry,
        {"remove_route_nodes",
         "Removes route nodes by id (1 to 1024 a call, each once; refused whole if one is not there) with "
         "the links that end at them. One undo step.",
         object_schema({"ids"}, {{"ids", {{"type", "array"}, {"minItems", 1}, {"maxItems", k_most_points},
                                          {"items", id_schema("A node.")}}}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             const Routes& routes = state.scene.routes;
             std::vector<RouteNodeId> ids = args["ids"].get<std::vector<RouteNodeId>>();
             std::sort(ids.begin(), ids.end());
             if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
                 return CommandResult::failure("ids names a node twice");
             }
             RouteChange change;
             for (const RouteNodeId id : ids) {
                 const RouteNode* node = world::find_route_node(routes.nodes, id);
                 if (node == nullptr) return CommandResult::failure("no route node " + std::to_string(id));
                 change.nodes_out.push_back(*node);
             }
             for (const RouteLink& l : routes.links) {
                 if (std::binary_search(ids.begin(), ids.end(), l.a) || std::binary_search(ids.begin(), ids.end(), l.b)) {
                     change.links_out.push_back(l);
                 }
             }
             const usize links = change.links_out.size();
             commit(state, context, std::move(change));
             return CommandResult::success({{"removed", ids.size()}, {"links_removed", links}});
         }});
    add(registry,
        {"get_routes",
         "Lists route nodes by rising id: all, or those within `within` metres (default 100) of `near`, "
         "`limit` at a time (default 100, at most 1000; 0 only counts) from `offset`; the `list` gives each "
         "its `position` and its `links` (`to` the node at the other end, `width`, `kind`, `length` metres; "
         "at most 16384 links a call: `links_cut` says some were left out, ask for fewer nodes). `count` "
         "is how many match; `nodes` and `links` how many there are. `within` is only for `near`.",
         object_schema(Json::array(),
                       {{"near", point_schema("Only nodes within `within` metres of here.")},
                        {"within", {{"type", "number"}, {"minimum", 0}, {"maximum", 1e8}}},
                        {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1 << 20}}},
                        {"limit", {{"type", "integer"}, {"minimum", 0}, {"maximum", k_most_listed}}}}),
         true,
         [&state](const Json& args, CommandContext&) {
             const Routes& routes = state.scene.routes;
             const bool by_place = args.contains("near");
             const DVec3 near = by_place ? point(args["near"]) : DVec3{};
             if (!by_place && args.contains("within")) return CommandResult::failure("`within` is for `near`: give both");
             const double within = args.value("within", 100.0);
             const usize offset = args.value("offset", usize{0}), limit = args.value("limit", usize{100});
             // The links of the nodes listed: each link is filed under its lower node, so those
             // under a higher one are looked for once through the links.
             std::vector<const RouteNode*> listed;
             usize count = 0;
             for (const RouteNode& n : routes.nodes) {
                 if (by_place && distance(n.position, near) > within) continue;
                 if (count >= offset && listed.size() < limit) listed.push_back(&n);
                 ++count;
             }
             usize links_listed = 0;
             bool links_cut = false;
             Json list = Json::array();
             for (const RouteNode* n : listed) list.push_back({{"id", n->id}, {"position", point_json(n->position)}, {"links", Json::array()}});
             if (!listed.empty()) {
                 const auto place = [&](RouteNodeId id) -> std::optional<usize> {
                     const auto it = std::lower_bound(listed.begin(), listed.end(), id,
                                                      [](const RouteNode* n, RouteNodeId v) { return n->id < v; });
                     if (it == listed.end() || (*it)->id != id) return std::nullopt;
                     return static_cast<usize>(it - listed.begin());
                 };
                 for (const RouteLink& l : routes.links) {
                     const std::optional<usize> at_a = place(l.a), at_b = place(l.b);
                     if (!at_a && !at_b) continue;
                     if (links_listed >= k_most_links_listed) {
                         links_cut = true;
                         break;
                     }
                     links_listed += (at_a ? 1u : 0u) + (at_b ? 1u : 0u);
                     const RouteNode* a = world::find_route_node(routes.nodes, l.a);
                     const RouteNode* b = world::find_route_node(routes.nodes, l.b);
                     const double length = distance(a->position, b->position);
                     const std::string kind(world::route_kind_name(l.kind));
                     if (at_a) list[*at_a]["links"].push_back({{"to", l.b}, {"width", l.width}, {"kind", kind}, {"length", length}});
                     if (at_b) list[*at_b]["links"].push_back({{"to", l.a}, {"width", l.width}, {"kind", kind}, {"length", length}});
                 }
             }
             return CommandResult::success({{"count", count},
                                            {"nodes", routes.nodes.size()},
                                            {"links", routes.links.size()},
                                            {"links_cut", links_cut},
                                            {"list", std::move(list)}});
         }});
    add(registry,
        {"get_route",
         "Finds the way along the roads and paths from `from` to `to`: each a point [x, y, z] (the nearest "
         "node within `within` metres, default 200, at most 4096) or a node's id. Returns `found`, and when "
         "found the `nodes` (ids in order), their `points`, the `length` in metres and the `cost` (length "
         "by pace: a road 1, a path 1.3 a metre, plus 3 a metre risen or fallen), or `why` not; `opened` is "
         "how many nodes the search looked at. The cheapest way, not always the shortest.",
         object_schema({"from", "to"},
                       {{"from", {{"description", "A point [x, y, z] or a route node's id."}}},
                        {"to", {{"description", "A point [x, y, z] or a route node's id."}}},
                        {"within", {{"type", "number"}, {"minimum", 0}, {"maximum", k_most_snap}}}}),
         true,
         [&state](const Json& args, CommandContext&) {
             const nav::RouteGraph graph(state.scene.routes);
             const double within = args.value("within", 200.0);
             std::string problem;
             const auto end = [&](const char* key) -> u32 {
                 const Json& v = args[key];
                 if (v.is_number()) { // an id (a whole number however it is written, as the other commands take one)
                     const double id = v.get<double>();
                     const bool whole = id >= 1.0 && id <= 4294967295.0 && id == std::floor(id);
                     const u32 node = whole ? graph.index_of(static_cast<RouteNodeId>(id)) : nav::k_no_route_node;
                     if (node == nav::k_no_route_node) problem = std::string("no route node `") + key + "` names";
                     return node;
                 }
                 if (v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number()) {
                     RouteNode asked;
                     asked.id = 1;
                     asked.position = point(v);
                     if (const auto bad = world::check_route_node(asked)) {
                         problem = std::string("`") + key + "`: " + *bad;
                         return nav::k_no_route_node;
                     }
                     const auto node = graph.nearest(asked.position, within);
                     if (!node) problem = std::string("no route node within ") + std::to_string(within) + " m of `" + key + "`";
                     return node.value_or(nav::k_no_route_node);
                 }
                 problem = std::string("`") + key + "` must be a point [x, y, z] or a route node's id";
                 return nav::k_no_route_node;
             };
             const u32 from = end("from");
             if (!problem.empty()) return CommandResult::failure(problem);
             const u32 to = end("to");
             if (!problem.empty()) return CommandResult::failure(problem);
             nav::RouteSearch search(graph, from, to);
             while (search.state() == nav::RouteState::searching) search.advance(4096);
             if (search.state() != nav::RouteState::found) {
                 return CommandResult::success({{"found", false}, {"why", "no way along the roads and paths joins them"},
                                                {"opened", search.opened()}});
             }
             Json nodes = Json::array(), points = Json::array();
             for (const u32 node : search.path()) {
                 nodes.push_back(graph.id(node));
                 points.push_back(point_json(graph.position(node)));
             }
             return CommandResult::success({{"found", true},
                                            {"nodes", std::move(nodes)},
                                            {"points", std::move(points)},
                                            {"length", search.length()},
                                            {"cost", search.cost()},
                                            {"opened", search.opened()}});
         }});
}

} // namespace giant::editor
