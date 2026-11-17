#include "people_travel.h"

#include "physics_state.h"

#include "giant/core/assert.h"
#include "giant/world/people.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace giant::editor {

namespace {

constexpr u32 k_slice = 256; // of search work at a time

double distance(const DVec3& a, const DVec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string whole(double m) { return std::to_string(static_cast<long long>(std::llround(m))); }

} // namespace

const char* PeopleTravel::to_string(Phase phase) {
    switch (phase) {
    case Phase::searching: return "searching";
    case Phase::travelling: return "travelling";
    case Phase::failed: return "failed";
    }
    return "failed";
}

PeopleTravel::Clock PeopleTravel::clock_of(const PhysicsState& physics) {
    if (!physics.simulating()) return {};
    return {physics.runs(), static_cast<double>(physics.world().stats().steps) * sim::PhysicsWorld::k_step_seconds};
}

PeopleTravel::Trip* PeopleTravel::find(world::PersonId id) {
    const auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : &m_trips[it->second];
}

const PeopleTravel::Trip* PeopleTravel::trip(world::PersonId id) const {
    const auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : &m_trips[it->second];
}

double PeopleTravel::gone(const Trip& trip) const {
    if (trip.phase != Phase::travelling) return 0.0;
    if (trip.held) return trip.from;
    const double moved = std::max(m_seconds - trip.since, 0.0) * trip.speed;
    return std::min(trip.from + moved, length(trip));
}

// Where a trip is `metres` along its way, facing along it.
PeopleTravel::Where PeopleTravel::at(const Trip& trip, double metres) {
    const std::vector<DVec3>& points = trip.points;
    const std::vector<double>& along = trip.along;
    Where w;
    w.yaw = trip.yaw;
    GIANT_ASSERT(points.size() >= 2 && along.size() == points.size(), "a trip's way has its start and its end");
    if (points.size() < 2) return w;
    // The stretch it is on: the first whose end is past `metres` (the last, at the end).
    usize i = static_cast<usize>(std::upper_bound(along.begin(), along.end(), metres) - along.begin());
    i = std::clamp<usize>(i, 1, points.size() - 1);
    const DVec3& a = points[i - 1];
    const DVec3& b = points[i];
    const double stretch = along[i] - along[i - 1];
    const double t = stretch > 0.0 ? std::clamp((metres - along[i - 1]) / stretch, 0.0, 1.0) : 1.0;
    // (At a stretch's end, its end exactly: one arrived is where it was sent.)
    w.position = t >= 1.0 ? b : DVec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    // Facing along the stretch (one of no length across the ground: along the nearest before it that has some).
    for (usize k = i; k >= 1; --k) {
        const double dx = points[k].x - points[k - 1].x, dz = points[k].z - points[k - 1].z;
        if (dx * dx + dz * dz > 1e-12) {
            w.yaw = radians_to_degrees(static_cast<float>(std::atan2(dx, dz)));
            break;
        }
    }
    return w;
}

PeopleTravel::Where PeopleTravel::where(const world::Person& person) const {
    if (m_trips.empty() && m_places.empty()) return {person.position, person.yaw}; // (nobody has travelled)
    if (const Trip* t = trip(person.id); t != nullptr && t->phase == Phase::travelling) return at(*t, gone(*t));
    if (const auto it = m_places.find(person.id); it != m_places.end()) return it->second;
    return {person.position, person.yaw};
}

void PeopleTravel::hold(world::PersonId id, bool held) {
    if (held) {
        m_held.insert(id);
    } else {
        m_held.erase(id);
    }
    Trip* t = find(id);
    if (t == nullptr || t->held == held) return;
    t->from = gone(*t); // (as it was until now)
    t->since = m_seconds;
    t->held = held;
}

void PeopleTravel::walked(world::PersonId id, double metres) {
    Trip* t = find(id);
    if (t == nullptr || t->phase != Phase::travelling || !t->held || !(metres > t->from)) return;
    t->from = std::min(metres, length(*t));
    t->since = m_seconds;
}

void PeopleTravel::arrive(world::PersonId id, const Where& there) {
    const auto it = m_index.find(id);
    if (it == m_index.end() || m_trips[it->second].phase != Phase::travelling) return;
    end(it->second, true);
    m_places[id] = there; // (where it stands, a step from the way's end)
}

PeopleTravel::Stats PeopleTravel::stats() const {
    Stats s = m_stats;
    s.trips = static_cast<u32>(m_trips.size());
    s.moved = static_cast<u32>(m_places.size());
    return s;
}

void PeopleTravel::clear() {
    ++m_resets;
    m_trips.clear();
    m_index.clear();
    m_places.clear();
    m_queue.clear();
    m_queue_head = 0;
    m_searching = 0;
    m_cursor = 0;
    m_stats = {};
}

// Takes a trip away: one arrived stays at its end, else where it is.
void PeopleTravel::end(usize index, bool arrived) {
    Trip& t = m_trips[index];
    if (t.phase == Phase::travelling) m_places[t.person] = at(t, arrived ? length(t) : gone(t));
    if (t.phase == Phase::searching) --m_stats.searching;
    if (arrived) ++m_stats.arrived;
    if (m_searching == t.person) m_searching = 0; // (its search's result is nobody's)
    m_index.erase(t.person);
    if (index + 1 != m_trips.size()) {
        t = std::move(m_trips.back());
        m_index[t.person] = static_cast<u32>(index);
    }
    m_trips.pop_back();
}

void PeopleTravel::fail(Trip& trip, const char* reason) {
    if (trip.phase == Phase::searching) --m_stats.searching;
    trip.phase = Phase::failed;
    trip.reason = reason;
    ++m_stats.failed;
}

// The way's lengths counted, and off it goes.
void PeopleTravel::set_off(Trip& trip, float yaw) {
    trip.yaw = yaw;
    trip.along.assign(trip.points.size(), 0.0);
    for (usize i = 1; i < trip.points.size(); ++i) {
        trip.along[i] = trip.along[i - 1] + distance(trip.points[i - 1], trip.points[i]);
    }
    if (trip.phase == Phase::searching) --m_stats.searching;
    trip.phase = Phase::travelling;
    trip.from = 0.0;
    trip.since = m_seconds;
    trip.held = m_held.contains(trip.person);
}

// A trip's search found its way: from where the person is, along the nodes, to the goal.
void PeopleTravel::found(const world::Scene& scene, Trip& trip) {
    const world::Person* person = world::find_person(scene.people, trip.person);
    if (person == nullptr) return; // (gone: its trip goes when it is next looked at)
    const std::vector<u32>& path = m_search.path();
    const Where now = where(*person);
    const DVec3 here = now.position;
    usize first = 0, last = path.size();
    // The nearest node may lie behind: one walks to the road's next node
    // when that is nearer than the road's own way there (and likewise leaves
    // the road a node early for a goal beside its last link).
    if (last - first >= 2 && distance(here, m_graph.position(path[1])) <
                                 distance(m_graph.position(path[0]), m_graph.position(path[1]))) {
        ++first;
    }
    if (last - first >= 2 && distance(m_graph.position(path[last - 2]), trip.to) <
                                 distance(m_graph.position(path[last - 2]), m_graph.position(path[last - 1]))) {
        --last;
    }
    trip.points.clear();
    trip.points.reserve(last - first + 2);
    trip.points.push_back(here);
    for (usize i = first; i < last; ++i) trip.points.push_back(m_graph.position(path[i]));
    trip.points.push_back(trip.to);
    set_off(trip, now.yaw);
}

// The graph, as the routes are now (a search under way begins again in it).
void PeopleTravel::build(const world::Routes& routes) {
    m_routes = routes;
    m_graph = nav::RouteGraph(m_routes);
    m_built = true;
    // Which joined-up part of the roads each node is in (a flood from each not yet reached).
    const u32 n = static_cast<u32>(m_graph.size());
    m_island.assign(n, nav::k_no_route_node);
    std::vector<u32> open;
    for (u32 start = 0; start < n; ++start) {
        if (m_island[start] != nav::k_no_route_node) continue;
        m_island[start] = start;
        open.push_back(start);
        while (!open.empty()) {
            const u32 node = open.back();
            open.pop_back();
            for (const nav::RouteGraph::Edge& edge : m_graph.edges(node)) {
                if (m_island[edge.to] != nav::k_no_route_node) continue;
                m_island[edge.to] = start;
                open.push_back(edge.to);
            }
        }
    }
    m_node_cursor = 0;
    m_link_cursor = 0;
    if (m_searching != 0) {
        m_queue.insert(m_queue.begin() + static_cast<std::ptrdiff_t>(m_queue_head), m_searching);
        m_searching = 0;
    }
}

// Whether the scene's routes are no longer those the graph was built from:
// their numbers at once, what each is a stretch a frame (a node moved is
// seen within some frames).
bool PeopleTravel::routes_changed(const world::Routes& routes) {
    if (routes.nodes.size() != m_routes.nodes.size() || routes.links.size() != m_routes.links.size() ||
        routes.next_id != m_routes.next_id) {
        return true;
    }
    constexpr usize k_compared = 2048;
    if (m_node_cursor >= routes.nodes.size()) m_node_cursor = 0;
    const usize nodes_end = std::min(routes.nodes.size(), m_node_cursor + k_compared);
    if (!std::equal(routes.nodes.begin() + static_cast<std::ptrdiff_t>(m_node_cursor),
                    routes.nodes.begin() + static_cast<std::ptrdiff_t>(nodes_end),
                    m_routes.nodes.begin() + static_cast<std::ptrdiff_t>(m_node_cursor))) {
        return true;
    }
    m_node_cursor = nodes_end;
    if (m_link_cursor >= routes.links.size()) m_link_cursor = 0;
    const usize links_end = std::min(routes.links.size(), m_link_cursor + k_compared);
    if (!std::equal(routes.links.begin() + static_cast<std::ptrdiff_t>(m_link_cursor),
                    routes.links.begin() + static_cast<std::ptrdiff_t>(links_end),
                    m_routes.links.begin() + static_cast<std::ptrdiff_t>(m_link_cursor))) {
        return true;
    }
    m_link_cursor = links_end;
    return false;
}

std::string PeopleTravel::send(const world::Scene& scene, world::PersonId id, const DVec3& to, float speed, double within,
                               std::string place) {
    if (!running() || scene.identity() != m_scene) {
        return "not simulating (simulate on first): people travel by the simulation's clock";
    }
    const world::Person* person = world::find_person(scene.people, id);
    if (person == nullptr) return "no person " + std::to_string(id);
    if (!std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z) || std::fabs(to.x) > 1e8 ||
        std::fabs(to.y) > 1e8 || std::fabs(to.z) > 1e8) {
        return "where it is sent must be finite and within 100,000 km";
    }
    if (!(speed >= 0.1f && speed <= 100.0f)) return "speed must be from 0.1 to 100 m/s";
    // (As the command gave it: 1.4, not the float nearest it, to a few places.)
    const double pace = std::round(static_cast<double>(speed) * 1000.0) / 1000.0;
    if (!(within >= 0.0)) return "within must be 0 or more";
    within = std::min(within, k_join_within);
    if (find(id) == nullptr && m_trips.size() >= k_max_trips) return "too many people are travelling";
    if (!m_built || !(scene.routes == m_routes)) build(scene.routes); // (compared whole: a command's cost)
    if (m_trips.empty() && m_places.empty()) { // the first of a run: room for everyone, taken once
        const usize room = std::min(scene.people.size(), k_max_trips);
        m_trips.reserve(room);
        m_index.reserve(room);
        m_places.reserve(room);
    }
    const Where now = where(*person);
    const DVec3 here = now.position;
    const auto first = m_graph.nearest(here, within), last = m_graph.nearest(to, within);
    const double straight = distance(here, to);
    // The roads are no help to one going no farther than to them and from them (or with none near at
    // either end but the goal within reach): straight there.
    const bool direct = (first && last && (*first == *last || straight <= distance(here, m_graph.position(*first)) +
                                                                              distance(m_graph.position(*last), to))) ||
                        ((!first || !last) && straight <= within);
    if (!direct && !first) return "no road or path within " + whole(within) + " m of person " + std::to_string(id);
    if (!direct && !last) return "no road or path within " + whole(within) + " m of where it is sent";
    if (!direct && m_island[*first] != m_island[*last]) { // (known at once: no search to spend frames on)
        return "no way along the roads and paths joins where person " + std::to_string(id) + " is and where it is sent";
    }

    if (const auto had = m_index.find(id); had != m_index.end()) end(had->second, false); // (it stays where it has got to)
    m_places.try_emplace(id, now); // (its place made now, in the command: arriving allocates nothing)
    Trip trip;
    trip.person = id;
    trip.serial = ++m_serial;
    trip.to = to;
    trip.place = std::move(place);
    trip.speed = pace;
    trip.yaw = now.yaw;
    ++m_stats.searching; // (searching until it sets off)
    if (direct) {
        trip.points = {here, to};
        set_off(trip, now.yaw);
    } else {
        trip.first = m_graph.id(*first);
        trip.last = m_graph.id(*last);
        trip.held = m_held.contains(id);
        m_queue.push_back(id);
    }
    m_index[id] = static_cast<u32>(m_trips.size());
    m_trips.push_back(std::move(trip));
    return {};
}

bool PeopleTravel::stop(world::PersonId id) {
    const auto it = m_index.find(id);
    if (it == m_index.end()) return false;
    end(it->second, false);
    return true;
}

void PeopleTravel::update(const world::Scene& scene, u64 run, double seconds) {
    sync(scene, run, seconds);
    if (run == 0) return;
    update_rest(scene);
}

void PeopleTravel::sync(const world::Scene& scene, u64 run, double seconds) {
    if (scene.identity() != m_scene) {
        clear();
        m_held.clear(); // (the entities were the other scene's)
        m_scene = scene.identity();
    }
    if (run != m_run) {
        clear();
        m_run = run;
    }
    m_seconds = run != 0 ? std::max(seconds, 0.0) : 0.0;
}

void PeopleTravel::update_rest(const world::Scene& scene) {
    if (m_queue_head >= 1024) { // (those begun leave the list now and then, not each)
        m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<std::ptrdiff_t>(m_queue_head));
        m_queue_head = 0;
    }
    // While ways are looked for, the graph follows the roads (edited while the
    // world runs: built again at once, an editing cost).
    if (m_built && (m_searching != 0 || m_queue_head < m_queue.size()) && routes_changed(scene.routes)) build(scene.routes);
    // The ways looked for: one search at a time, a few slices a frame (starting one is a slice).
    const u32 slices = std::clamp<u32>(settings.search_work_a_frame, k_slice, 1u << 20) / k_slice;
    u32 free_skips = 256;
    for (u32 s = 0; s < slices; ++s) {
        if (m_searching == 0) {
            if (m_queue_head >= m_queue.size()) {
                m_queue.clear();
                m_queue_head = 0;
                break;
            }
            const world::PersonId id = m_queue[m_queue_head++];
            Trip* t = find(id);
            if (t == nullptr || t->phase != Phase::searching) { // (stopped or sent again since)
                if (free_skips > 0) { // (no slice spent on it, up to some a frame)
                    --free_skips;
                    --s;
                }
                continue;
            }
            const u32 a = m_graph.index_of(t->first), b = m_graph.index_of(t->last);
            if (a == nav::k_no_route_node || b == nav::k_no_route_node) {
                fail(*t, "the roads changed while its way was looked for: send it again");
                continue;
            }
            if (m_island[a] != m_island[b]) { // (the roads were cut since it was sent: no search spent)
                fail(*t, "no way along the roads and paths joins where it is and where it is sent");
                continue;
            }
            m_search.start(m_graph, a, b);
            m_searching = id;
            ++m_stats.searches;
            continue;
        }
        const nav::RouteState state = m_search.advance(k_slice);
        if (state == nav::RouteState::searching) continue;
        m_stats.opened += m_search.opened();
        Trip* t = find(m_searching);
        m_searching = 0;
        if (t == nullptr || t->phase != Phase::searching) continue;
        if (state == nav::RouteState::found) {
            found(scene, *t);
        } else {
            fail(*t, "no way along the roads and paths joins where it is and where it is sent");
        }
    }

    // Those arrived (and trips of people gone), a share of the trips a frame.
    const usize looked = std::min<usize>(std::clamp<u32>(settings.looked_a_frame, 1, 65536), m_trips.size());
    for (usize n = 0; n < looked && !m_trips.empty(); ++n) {
        if (m_cursor >= m_trips.size()) m_cursor = 0;
        const Trip& t = m_trips[m_cursor];
        if (world::find_person(scene.people, t.person) == nullptr) {
            const world::PersonId id = t.person;
            end(m_cursor, false);
            m_places.erase(id);
            continue; // (the last trip took its place: looked at next)
        }
        if (t.phase == Phase::travelling && !t.held && gone(t) >= length(t)) {
            end(m_cursor, true);
            continue;
        }
        ++m_cursor;
    }

    // Those kept somewhere whose person is gone (with no trip to be found by): a few buckets a frame.
    m_gone.clear();
    const usize buckets = m_places.bucket_count();
    for (usize n = 0; n < 64 && n < buckets && !m_places.empty(); ++n) {
        if (m_place_bucket >= buckets) m_place_bucket = 0;
        for (auto it = m_places.cbegin(m_place_bucket); it != m_places.cend(m_place_bucket); ++it) {
            if (world::find_person(scene.people, it->first) == nullptr) m_gone.push_back(it->first);
        }
        ++m_place_bucket;
    }
    for (const world::PersonId id : m_gone) m_places.erase(id);
}

} // namespace giant::editor
