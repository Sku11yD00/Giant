#!/usr/bin/env python3
"""The living-world gate (living-world plan LW8, the N6 gate): five thousand
people living their day in the N3 gate's valley, flown through.

    python3 tools/living_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames). It builds the N3 gate's edited valley and
peoples it: the town's houses and the hamlet's each a home, and villages
down both sides of the valley (eight houses, a well and a field each),
joined by roads; 5,000 people in all, each with a home and a routine
(farmers sleep, work their field, eat at the well; townsfolk sleep, work at
the market in the street, eat at home), who run home from what alarms them.
It saves that as a world and opens it again, simulating, with the clock
running a day in ten minutes (but for the flight, whose hour of the day is
made to last as long as the flight will, whatever the frame rate), and from
before dawn:
- sees everyone asleep at home;
- flies the N3 gate's flight (up the river, down the town's street at head
  height, spiralling up and out) at 2560x1440 where the screen allows,
  while the valley wakes and sets off to work, and fires a shot in the
  street;
- sees the morning's work, saves the game, lets the day run on to the
  midday meal, and loads the save.
It passes when:
- the flight holds 60 fps without micro stutter (as the other gates:
  average under 16.7 ms, 99th percentile under 20.8 ms, at most one
  hitch), over the whole flight and over each leg on its own;
- the people take under 0.5 ms of the main thread a frame (the `travel`,
  `senses`, `routines` and `people` phases: trips moved on, what they
  hear and see, their routines, entities for those near): the four
  phases' medians summed, in the worst sample of the flight (every 50
  stops), the valley's setting off among them;
- nobody appeared or went close by in the camera's view in the flight
  (set_people_settings: `appeared_in_view`, `vanished_in_view`), and
  people walked the street while it was flown;
- the day was lived: before dawn everyone sleeps at home; by mid morning
  they work, at their field or market; at midday they eat where they eat;
  nobody's routine failed to send it;
- townsfolk were out when the shot was fired (on their way or at the
  market), it sent those running home, and calm again they came back to
  their work;
- the saved game, loaded, has everyone where they were and doing what
  they did when it was saved, at the time of day it was, the day running
  on;
- nothing overflowed.

On CI (the null device, which draws nothing and times nothing) a smaller
valley's people (the town, the hamlet and a few villages) are put through
the same day, twice as long, on a shorter flight, and it checks all but the
speed and the people's budget: tools are tested, not timings.
LIVING_GATE_VILLAGES=<count> peoples that many villages whatever the device
(56: all 5,000 people), for trying the whole valley without a GPU.
"""
import math
import os
import sys
import tempfile
import time

from gate_common import Editor, describe, frame_stats, leg_slow_frames, slow_frames, speed_failures, weighted_median
from gate_person import write_person
from n2_gate import ALL_ON, set_resolution, settle
from n3_gate import (HAMLET, HOUSES_X, HOUSES_Z, SPACING, STOPS, TOWN, build_valley, flight_path, hamlet_houses, leg_ends,
                     leg_of, town_houses, wait_streaming)

RESOLUTION = (2560, 1440)
PEOPLE = 5000
VILLAGES = 56              # down both sides of the valley
SMALL_VILLAGES = 4         # off a real GPU (CI): the same day, fewer people
VILLAGE_X = 280.0          # metres either side of the river
VILLAGE_EVERY = 120.0      # metres between villages down a side
VILLAGE_HOUSES = 8
FIELD_IN = 40.0            # metres from a village towards the river: its field
PER_HOUSE = 4              # people to a house in the town and the hamlet (the villages take the rest)
DAY_SECONDS = 600.0        # a day in ten minutes
SMALL_DAY_SECONDS = 1200.0 # off a real GPU: frames are slow there, and the flight is to fit the morning
SMALL_STOPS = 400          # and a shorter flight (not so short that the camera leaps: 50 m a frame is a jump)
START_HOUR = 4.5
FLIGHT_HOUR = 5.8          # the flight sets out: the valley wakes at 6
FLIGHT_HOURS = 1.0         # of the day the flight is made to last (the clock's speed set from the frames' pace)
FRAMES_A_STOP = 2.15       # of the flight: two commands a stop, and its reads
WORK_HOUR = (9.0, 11.2)    # everyone at work from the first, at the latest by the second (the game saved then)
MEAL_HOUR = (12.6, 13.3)   # everyone at the midday meal (the save loaded then)
PEOPLE_BUDGET_MS = 0.5     # of the main thread a frame
SHOT_AT = 0.35             # the street leg's share gone when the shot is fired
FLED_AFTER = 40            # stops after the shot the fleeing are counted
READ_EVERY = 25            # stops between reads of the people in the flight
SAMPLE_EVERY = 50          # stops between the flight's reads of the frame times (the editor keeps 240 frames)
CALM_SECONDS = 30.0        # a full alarm eases to nothing over this
MOST_EMBODIED = 512        # entities held at once (the town's people and more)
SETTLED_M = 1.0            # one standing where it stood: loaded, within this of where it was saved
MOVING_M = 40.0            # one on its way when saved or when read: within this
LOOKS = (("villager red", [0.45, 0.14, 0.12]), ("villager blue", [0.14, 0.2, 0.42]), ("villager green", [0.16, 0.34, 0.15]))

ROUTINES = {
    # (A day in ten minutes is 25 seconds an hour, and people walk at their own pace by the real
    # clock: so work is near home and they step out briskly.)
    "farmer": [{"from": 0, "activity": "sleep", "at": "home"},
               {"from": 6, "activity": "work", "at": "tag", "name": "field", "speed": 3.0},
               {"from": 11.5, "activity": "eat", "at": "tag", "name": "well", "speed": 3.0},
               {"from": 13.5, "activity": "work", "at": "tag", "name": "field", "speed": 3.0},
               {"from": 19, "activity": "sleep", "at": "home", "speed": 3.0}],
    "townsfolk": [{"from": 0, "activity": "sleep", "at": "home"},
                  {"from": 6.15, "activity": "work", "at": "tag", "name": "market", "speed": 3.0},
                  {"from": 11.5, "activity": "eat", "at": "home", "speed": 3.0},
                  {"from": 13.5, "activity": "work", "at": "tag", "name": "market", "speed": 3.0},
                  {"from": 20, "activity": "sleep", "at": "home", "speed": 3.0}],
}
ALARMED = [{"over": 0.3, "activity": "flee", "at": "home", "speed": 4.0}]


def village_spots(count):
    """The middles (x, z) of `count` villages, down both sides of the valley by turns, clear of the
    town's road and the hamlet."""
    spots = []
    rows = (count + 1) // 2
    for i in range(count):
        row, side = i // 2, 1.0 if i % 2 == 0 else -1.0
        spots.append((side * VILLAGE_X, round((row - (rows - 1) / 2) * VILLAGE_EVERY + TOWN[1] + VILLAGE_EVERY / 2, 2)))
    return spots


def house_spots(middle):
    """A village's houses (x, z): two rows of four."""
    return [(middle[0] + (i % 2 - 0.5) * 14.0, middle[1] + (i // 2 - 1.5) * 12.0) for i in range(VILLAGE_HOUSES)]


def market_spots():
    """Where the town's alleys cross its street (x, z): the market's places, clear of the houses."""
    return [(TOWN[0], round(TOWN[1] + (j - HOUSES_Z / 2) * SPACING, 2)) for j in range(HOUSES_Z + 1)]


def plan(villages, people):
    """The valley's homes and who lives where, by arithmetic alone (no editor). Returns
    {"homes": [(name, x, z)], "fields": [...], "wells": [...], "markets": [...],
     "people": [(home index, routine)], "villages": [(x, z)]}: the town's and the hamlet's homes take
    PER_HOUSE each, the villages' homes share the rest in turn. The hamlet has a field and a well of
    its own (the last of each)."""
    homes, fields, wells = [], [], []
    for i, (x, z) in enumerate(town_houses()):
        homes.append((f"Home T{i}", x, round(z + 5.0, 2)))  # (before its door, in the alley)
    town_homes = len(homes)
    for i, (x, z) in enumerate(hamlet_houses()):
        homes.append((f"Home H{i}", x, round(z + 5.0, 2)))
    fixed_homes = len(homes)
    middles = village_spots(villages)
    for v, middle in enumerate(middles):
        for h, (x, z) in enumerate(house_spots(middle)):
            homes.append((f"Home V{v}-{h}", x, round(z + 5.0, 2)))
        side = 1.0 if middle[0] > 0 else -1.0
        fields.append((f"Field {v}", middle[0] - side * FIELD_IN, middle[1]))
        wells.append((f"Well {v}", middle[0], middle[1]))
    fields.append(("Field H", HAMLET[0] - 30.0, HAMLET[1] + 16.0))
    wells.append(("Well H", HAMLET[0] + 21.0, HAMLET[1] + 26.0))
    markets = [(f"Market {j}", x, z) for j, (x, z) in enumerate(market_spots())]
    who = []
    for home in range(fixed_homes):
        who += [(home, "townsfolk" if home < town_homes else "farmer")] * PER_HOUSE
    village_homes = len(homes) - fixed_homes
    rest = people - len(who)
    if village_homes == 0 or rest < village_homes:
        raise ValueError(f"{people} people do not fill {len(homes)} homes")
    for k in range(rest):
        who.append((fixed_homes + k % village_homes, "farmer"))
    return {"homes": homes, "fields": fields, "wells": wells, "markets": markets, "people": who, "villages": middles}


def people_count(villages):
    """How many people a valley of `villages` villages is given: PEOPLE with all of them, as crowded with fewer."""
    fixed = (HOUSES_X * HOUSES_Z + 12) * PER_HOUSE
    return fixed + (PEOPLE - fixed) * villages // VILLAGES


def ground_heights(tool, points):
    """The ground's height at each of `points` ((x, z)), 4096 a call."""
    heights = []
    for i in range(0, len(points), 4096):
        heights += tool("get_terrain", {"points": [[x, z] for x, z in points[i:i + 4096]]})["heights"]
    return heights


def keep_look(tool, name, mesh, colour, position, tints=None):
    """Makes the person look `name`: a character of `mesh` dressed in `colour` at `position`, kept as a
    look (in `tints`, colours its people come in, if given) and not needed after."""
    id_ = tool("create_entity", {"name": name, "shape": "mesh", "mesh": mesh, "body": "none", "position": position,
                                 "base_color": colour, "roughness": 0.85})["id"]
    tool("set_animation", {"id": id_, "clip": "idle", "matched": ["*"]})
    tool("set_person_look", {"name": name, "entity": id_})
    if tints:
        tool("set_person_look", {"name": name, "tints": tints})
    tool("delete_entities", {"ids": [id_]})


def settle_valley(tool, whole, town_h, villages, day_seconds):
    """Peoples the valley build_valley made: the villages' houses, the places, the roads, the looks, the
    routines and the people, and sets the clock to a day in `day_seconds`. Returns (the plan, the
    people's ids in the plan's order)."""
    laid = plan(villages, people_count(villages))

    def ground(points):
        return [round(h, 3) for h in ground_heights(tool, points)]

    # The villages' houses, on the ground as it lies.
    houses = [spot for middle in laid["villages"] for spot in house_spots(middle)]
    for i in range(0, len(houses), 512):
        part = houses[i:i + 512]
        tool("create_entities", {"entities": [{"kit": "house", "position": [x, h, z]}
                                              for (x, z), h in zip(part, ground(part))]})
    # The places: a home before each door, the villages' fields and wells, the market down the street.
    kinds = (("homes", 2.0, "home"), ("fields", 15.0, "field"), ("wells", 4.0, "well"), ("markets", 2.4, "market"))
    heights = {}
    for key, radius, tag in kinds:
        for (name, x, z), h in zip(laid[key], ground([(x, z) for _, x, z in laid[key]])):
            heights[name] = h
            tool("create_place", {"name": name, "position": [x, h, z], "radius": radius, "tags": [tag]})
    # The roads: one down each side through its villages, a path from each village to its field, a road
    # from each side to the town and the street down the town's middle.
    for side in (1.0, -1.0):
        line = sorted((m for m in laid["villages"] if m[0] * side > 0), key=lambda m: m[1])
        if len(line) > 1:
            tool("create_route", {"points": [[x, h, z] for (x, z), h in zip(line, ground(line))], "kind": "road", "width": 4})
        if line:
            nearest = min(line, key=lambda m: abs(m[1] - TOWN[1]))
            way = [nearest, (side * 150.0, TOWN[1]), (side * 50.0, TOWN[1]), (TOWN[0], TOWN[1])]
            tool("create_route", {"points": [[x, h, z] for (x, z), h in zip(way, ground(way))], "kind": "road", "width": 4})
    # (zip stops at the villages: the hamlet's field and well, the last, get their own path.)
    ways = [[middle, (fx, fz)] for middle, (_, fx, fz) in zip(laid["villages"], laid["fields"])]
    ways.append([laid["fields"][-1][1:], laid["wells"][-1][1:]])
    # (And the hamlet joined to the nearest village on its side: the roads are all one, so whichever
    # field or well is nearest one's home, there is a way to it.)
    well = laid["wells"][-1][1:]
    ways.append([well, min((m for m in laid["villages"] if m[0] * well[0] > 0), key=lambda m: math.dist(m, well))])
    for way in ways:
        tool("create_route", {"points": [[x, h, z] for (x, z), h in zip(way, ground(way))], "kind": "path"})
    street = market_spots()
    tool("create_route", {"points": [[x, town_h, z] for x, z in street], "kind": "road", "width": 4})
    for i, (name, colour) in enumerate(LOOKS):
        keep_look(tool, name, whole, colour, [TOWN[0] + 60.0 + i, town_h, TOWN[1]])
    for name, entries in ROUTINES.items():
        tool("set_routine", {"name": name, "entries": entries, "alarmed": ALARMED})
    # The people: each by its home, a step from the next.
    people = []
    at_home = {}
    for k, (home, routine) in enumerate(laid["people"]):
        name, x, z = laid["homes"][home]
        n = at_home.get(home, 0)
        at_home[home] = n + 1
        angle = 2.4 * n  # (the golden angle: spread round the spot)
        r = min(1.8, 0.45 * math.sqrt(n))
        people.append({"look": LOOKS[k % len(LOOKS)][0], "home": name, "routine": routine,
                       "position": [round(x + math.sin(angle) * r, 2), heights[name], round(z + math.cos(angle) * r, 2)],
                       "yaw": (k * 47) % 360, "seed": k + 1})
    ids = []
    for i in range(0, len(people), 4096):
        ids += tool("create_people", {"people": people[i:i + 4096]})["ids"]
    tool("set_time", {"on": True, "hours": START_HOUR, "day": 172, "latitude": 48, "speed": 86400.0 / day_seconds})
    return laid, ids


def everyone(tool):
    """Every person as get_people gives them, by id."""
    found = {}
    offset = 0
    while True:
        page = tool("get_people", {"limit": 1000, "offset": offset})
        for person in page["people"]:
            found[person["id"]] = person
        offset += len(page["people"])
        if not page["people"] or offset >= page["count"]:
            return found


def doing(person):
    return (person.get("doing") or {}).get("activity", "")


def travelling(person):
    return person.get("trip") is not None and person["trip"]["phase"] in ("searching", "travelling")


def day_check(now, laid, ids, want, where):
    """What of `want` ({routine: activity}) and `where` ({routine: place kind or "home"}) the people
    read `now` fail: lines. Of each routine all but a hundredth must be doing it, and all but a
    twentieth be there (the rest still on their way): by routine, so that the few townsfolk are not
    lost among the many farmers."""
    spots = {key: [(x, z) for _, x, z in laid[key]] for key in ("fields", "wells", "markets")}
    radius = {"fields": 15.0, "wells": 4.0, "markets": 2.4, "home": 2.0}
    count, wrong, away, missing = {}, {}, {}, 0
    for id_, (home, routine) in zip(ids, laid["people"]):
        count[routine] = count.get(routine, 0) + 1
        person = now.get(id_)
        if person is None:
            missing += 1
            continue
        if doing(person) != want[routine] or (person.get("doing") or {}).get("failed"):
            wrong[routine] = wrong.get(routine, 0) + 1
            continue
        x, z = person["at"][0], person["at"][2]
        kind = where[routine]
        places = [laid["homes"][home][1:]] if kind == "home" else spots[kind]
        if min(math.hypot(x - px, z - pz) for px, pz in places) > radius[kind] + 1.0:
            away[routine] = away.get(routine, 0) + 1
    failures = []
    if missing:
        failures.append(f"{missing} of {len(ids)} people are not there")
    for routine, n in count.items():
        if wrong.get(routine, 0) > n // 100:
            failures.append(f"{wrong[routine]} of {n} {routine} do not do what the hour says ({want[routine]})")
        if away.get(routine, 0) > n // 20:
            failures.append(f"{away[routine]} of {n} {routine} are not where the hour has them ({where[routine]})")
    return failures


def out_of_doors(people, markets):
    """The ids of those of `people` (get_people's) out when read: on their way, or at a market."""
    return {p["id"] for p in people
            if travelling(p) or min(math.hypot(p["at"][0] - x, p["at"][2] - z) for _, x, z in markets) <= 2.4 + 1.0}


def runs_home(person, home):
    """Whether `person` (get_people's) runs from what alarmed it to its `home` ((name, x, z)): told to
    flee, and on its way there or there already."""
    if doing(person) != "flee":
        return False
    if travelling(person):
        return person["trip"].get("place") == home[0]
    return math.hypot(person["at"][0] - home[1], person["at"][2] - home[2]) <= 2.0 + 1.0


def flight_clock_speed(frame_seconds, stops):
    """The clock's speed (game seconds a real second) that makes a flight of `stops` stops, at
    `frame_seconds` a frame, last FLIGHT_HOURS of the day: 240 frames hovering first, then
    FRAMES_A_STOP a stop."""
    return FLIGHT_HOURS * 3600.0 / max((240 + FRAMES_A_STOP * stops) * frame_seconds, 1e-3)


PEOPLE_PHASES = ("travel", "senses", "routines", "people")


def people_ms(typicals):
    """The people's share of a frame over a flight (Editor.fly's typicals: per sample its frames and
    each phase's median ms): (the worst sample's ms, that sample's parts by phase, the median of all
    the samples weighted by frames), or None when the editor times no such phases. A sample's ms is
    its four phases' medians summed."""
    samples = [(frames, {name: typical[name] for name in PEOPLE_PHASES}) for frames, typical, _ in typicals
               if all(name in typical for name in PEOPLE_PHASES)]
    if not samples:
        return None
    worst = max(samples, key=lambda s: sum(s[1].values()))[1]
    return sum(worst.values()), worst, weighted_median([(frames, sum(parts.values())) for frames, parts in samples])


def people_cost(typicals, budget_ms, judged):
    """The people's share of a frame over a flight (people_ms) told: (a line, or None when the editor
    times no such phases; what it fails: not timed, or, when `judged`, the worst sample over `budget_ms`)."""
    cost = people_ms(typicals)
    if cost is None:
        return None, [f"the editor does not time the people's share of a frame (the phases {PEOPLE_PHASES})"]
    parts = ", ".join(f"{name} {ms:.3f}" for name, ms in cost[1].items())
    told = (f"the people take {cost[2]:.3f} ms of the main thread a frame (the median), "
            f"{cost[0]:.3f} ms in the worst sample of the flight ({parts})")
    if judged and cost[0] > budget_ms:
        return told, [f"the people take {cost[0]:.3f} ms a frame in the flight's worst sample, over {budget_ms} ms ({parts})"]
    return told, []


def loaded_failures(before, after, moving_m=MOVING_M, settled_m=SETTLED_M):
    """What a loaded game's people (`after`, by id) fail of those read when it was saved (`before`):
    each where it was (one standing: within settled_m; one on its way then or now: within moving_m) and
    doing what it did. Lines; all but a hundredth must hold, and most must have been standing (else the
    check says little)."""
    moved, other, gone, standing = 0, 0, 0, 0
    for id_, was in before.items():
        now = after.get(id_)
        if now is None:
            gone += 1
            continue
        on_its_way = travelling(was) or travelling(now)
        standing += 0 if on_its_way else 1
        if math.dist(was["at"], now["at"]) > (moving_m if on_its_way else settled_m):
            moved += 1
        if doing(was) != doing(now):
            other += 1
    count = len(before)
    failures = []
    if gone:
        failures.append(f"loaded, {gone} of {count} people are gone")
    if moved > count // 100:
        failures.append(f"loaded, {moved} of {count} people are not where they were when the game was saved")
    if other > count // 100:
        failures.append(f"loaded, {other} of {count} people do something else than when the game was saved")
    if standing < count // 2:
        failures.append(f"only {standing} of {count} people stood still when the game was saved: the check says little")
    return failures


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    failures = []
    real_gpu = False
    editor = None
    folder = tempfile.TemporaryDirectory()
    try:
        whole = os.path.join(folder.name, "person.glb").replace("\\", "/")
        person = write_person(whole)
        editor = Editor(sys.argv[1], sys.argv[2:], "living_gate", timeout=2300)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"living_gate: {info['adapter']} ({info['backend']})")
        real_gpu = editor.real_gpu()
        if not real_gpu:
            print("living_gate: warning: not a real GPU, the timings mean nothing")
        width, height = set_resolution(editor, *RESOLUTION)
        print(f"living_gate: {width}x{height} pixels")
        if real_gpu and (width, height) != RESOLUTION:
            print(f"living_gate: warning: the budgets are for {RESOLUTION[0]}x{RESOLUTION[1]}")
        # (This gate measures the living world as it was measured before distant people were drawn:
        # what figures cost is the crowd gate's to measure.)
        tool("set_figure_settings", {"on": False})
        town_h = build_valley(tool)
        tool("import_mesh", {"path": whole})
        villages = int(os.environ.get("LIVING_GATE_VILLAGES", VILLAGES if real_gpu else SMALL_VILLAGES))
        stops = STOPS if real_gpu else SMALL_STOPS
        laid, ids = settle_valley(tool, whole, town_h, villages, DAY_SECONDS if real_gpu else SMALL_DAY_SECONDS)
        townsfolk = sum(1 for _, routine in laid["people"] if routine == "townsfolk")

        path = flight_path(stops, lambda points: ground_heights(tool, points))
        world_folder = os.path.join(folder.name, "valley")
        saved = tool("save_world", {"folder": world_folder})
        routes = tool("get_routes", {"limit": 0})
        print(f"living_gate: saved {saved['cells']} cells with {len(ids)} people ({townsfolk} townsfolk, "
              f"{len(ids) - townsfolk} farmers; {person['triangles']} triangles a person) in {len(laid['homes'])} homes, "
              f"{len(laid['villages'])} villages; {tool('get_places', {'limit': 0})['total']} places, "
              f"{routes['nodes']} road nodes, {routes['links']} links; town at {town_h:.1f} m")
        low_end, street_end = leg_ends(stops)
        # Opened where the flight sets out, and caught up there before the day begins (with the clock
        # running the sun moves, and the shadows and the light are never done).
        tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        tool("open_world", {"folder": world_folder})
        tool("set_render_settings", ALL_ON)
        tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        world, scatter, streamed = wait_streaming(editor, 6000)
        last, settled = settle(editor, 6000)
        if not (streamed and settled):
            failures.append("loading did not catch up in 6000 frames")
        print(f"living_gate: loaded: {world['loaded']} cells, {scatter['instances']} scatter instances")
        if tool("get_people", {"limit": 0})["total"] != len(ids):
            failures.append(f"opened, the world has {tool('get_people', {'limit': 0})['total']} people, not {len(ids)}")
        tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        tool("set_people_settings", {"most": MOST_EMBODIED, "made_a_frame": 4})
        tool("set_sense_settings", {"calm_seconds": CALM_SECONDS})
        # The day begins: simulating, the clock running.
        started = tool("simulate", {"on": True, "clock": True})
        if not started.get("clock"):
            failures.append("the clock does not run while simulating")

        def hour():
            return tool("get_time")["shown"]["hours"]

        def wait_hour(until, seconds=900.0):
            """Hovers until the clock shows `until` (hours, later today). Returns the hour it stopped at."""
            end = time.monotonic() + seconds
            now = hour()
            while now < until and time.monotonic() < end:
                editor.hover(20)
                now = hour()
            return now

        def by_hour(what, hours, want, where):
            """The people's day checked (day_check) from hours[0] on, again until it holds or hours[1]
            has come (those late on their way arrive). Returns (the people as last read, the hour)."""
            now_hour = wait_hour(hours[0])
            while True:
                now = everyone(tool)
                found = day_check(now, laid, ids, want, where)
                if not found or now_hour >= hours[1] or now_hour < hours[0]:  # (or past midnight)
                    break
                editor.hover(60)
                now_hour = hour()
            activity = next(iter(want.values()))
            print(f"living_gate: {now_hour:.2f} h: {sum(1 for p in now.values() if doing(p) == activity)} of {len(ids)} "
                  f"{what}")
            failures.extend(f"{what} ({now_hour:.2f} h): {f}" for f in found)
            return now, now_hour

        # Before dawn: everyone asleep at home (once the routines have looked at everyone).
        by_hour("asleep at home", (START_HOUR, FLIGHT_HOUR - 0.3), {"farmer": "sleep", "townsfolk": "sleep"},
                {"farmer": "home", "townsfolk": "home"})

        # The flight, as the valley wakes: the hour from before 6 made to last as long as the flight will
        # at this machine's pace (the flight is so many frames, the day so many seconds), so that the
        # valley sets off, the townsfolk are out when the street is flown and the shot falls among them,
        # whatever the frame rate.
        if hour() >= 6.0:
            failures.append(f"it is {hour():.2f} h before the flight: the valley woke while it was loaded and read")
        began = time.monotonic()
        editor.hover(120)
        frame_seconds = (time.monotonic() - began) / 120.0
        day_speed = tool("get_time")["clock"]["speed"]
        flight_speed = flight_clock_speed(frame_seconds, stops)
        tool("set_time", {"hours": FLIGHT_HOUR, "speed": flight_speed})
        set_out = hour()
        print(f"living_gate: a frame takes {frame_seconds * 1000.0:.1f} ms: the flight's hour runs at {flight_speed:.0f} game "
              f"seconds a second (the day's {day_speed:.0f})")
        before = tool("set_people_settings")
        routine_before = tool("set_routine_settings")
        shot_at = low_end + int((street_end - low_end) * SHOT_AT)
        overflow = []
        seen = {"embodied": 0, "street_walking": 0, "unseen": 0, "walking": 0, "alarmed": 0, "trips": 0, "waiting": 0}
        shot = {"fired": False, "out": set(), "fled": None, "hour": 0.0}
        markets = laid["markets"]
        home_of = {id_: laid["homes"][home] for id_, (home, _) in zip(ids, laid["people"])}

        def each_stop(stop, frame):
            # (A read every so many stops, a command a frame: the editor keeps its last 240 frame times and
            # the flight reads them every SAMPLE_EVERY stops, so all of these must leave room.)
            if stop % READ_EVERY == 0:
                held = tool("set_people_settings")
                seen["embodied"] = max(seen["embodied"], held["embodied"])
                seen["unseen"] = max(seen["unseen"], held["unseen"])
                seen["walking"] = max(seen["walking"], held["walking"])
                seen["waiting"] = max(seen["waiting"], held["waiting"])
                if leg_of(stop, stops) == "street":
                    seen["street_walking"] = max(seen["street_walking"], held["walking"])
                seen["trips"] = max(seen["trips"], tool("get_travel")["trips"])
                seen["alarmed"] = max(seen["alarmed"], tool("get_people", {"alarmed": True, "limit": 0})["count"])
            if stop == shot_at:
                # Who is out, then a rifle shot in the street, into the ground from just over it, half
                # way between two of the market's places (where nobody stands).
                shot["out"] = out_of_doors(tool("get_people", {"routine": "townsfolk", "limit": 1000})["people"], markets)
                at = path[stop][0]
                z = min((z for _, _, z in markets), key=lambda z: abs(z - at[2])) + 5.0
                tool("fire", {"origin": [TOWN[0] + 1.2, town_h + 0.3, z], "direction": [0.0, -1.0, 0.0], "speed": 880.0})
                shot["fired"] = True
                shot["hour"] = hour()
            if stop == shot_at + FLED_AFTER:
                # Those who were out and now run home: alarmed, told to flee, and on their way or there.
                alarmed = tool("get_people", {"alarmed": True, "routine": "townsfolk", "limit": 1000})["people"]
                shot["fled"] = sum(1 for p in alarmed if p["id"] in shot["out"] and runs_home(p, home_of[p["id"]]))
            if frame["overflow"] or frame["voxels"]["overflow"] or frame["decal_overflow"] or frame["light_overflow"]:
                overflow.append(stop)

        flown = editor.fly(path, each_stop, marks=(low_end, street_end, *range(SAMPLE_EVERY - 1, stops, SAMPLE_EVERY)))
        landed = hour()
        tool("set_time", {"hours": landed, "speed": day_speed})  # (the day at its own pace again)
        times = frame_stats(flown)
        slow = slow_frames(flown, editor.slow, editor.typicals, editor.slow_over_ms,
                           leg=lambda at: leg_of(at, stops))
        if slow:
            print(f"living_gate: {slow}")
        print(f"living_gate: frame time over the flight ({times['frames']} frames sampled, {set_out:.2f} h to "
              f"{landed:.2f} h): {describe(times)}")
        cuts = [0, editor.marked[low_end], editor.marked[street_end], len(flown)]
        leg_times = {}
        for i, name in enumerate(("low", "street", "climb")):
            leg_times[name] = frame_stats(flown[cuts[i]:cuts[i + 1]])
            print(f"living_gate: flying {name}: {describe(leg_times[name])}")
            told = leg_slow_frames(flown[cuts[i]:cuts[i + 1]], editor.slow, editor.typicals, editor.slow_over_ms,
                                   lambda at, name=name: leg_of(at, stops) == name) if real_gpu else None
            if told:
                print(f"living_gate: flying {name}: {told}")
        after = tool("set_people_settings")
        routine_after = tool("set_routine_settings")
        appeared = after["appeared_in_view"] - before["appeared_in_view"]
        vanished = after["vanished_in_view"] - before["vanished_in_view"]
        sent = routine_after["sent"] - routine_before["sent"]
        print(f"living_gate: in the flight: {sent} people sent by their routines, at most {seen['trips']} on their way at once; "
              f"at most {seen['embodied']} with an entity, {seen['walking']} of them walking ({seen['street_walking']} while the street was flown), "
              f"{after['made'] - before['made']} made, {after['released'] - before['released']} released, "
              f"{after['swapped'] - before['swapped']} swapped; {appeared} appeared and {vanished} went close by in view; "
              f"at most {seen['unseen']} waited to be out of sight, {seen['waiting']} for room")
        told, over = people_cost(editor.typicals, PEOPLE_BUDGET_MS, real_gpu)
        if told:
            print(f"living_gate: {told}")
        failures += over
        if appeared or vanished:
            failures.append(f"in the flight {appeared} people appeared and {vanished} went close by in view")
        if seen["street_walking"] < townsfolk // 8:
            failures.append(f"at most {seen['street_walking']} people were walking as characters while the street was flown "
                            f"(of {townsfolk} townsfolk)")
        if after["swapped"] - before["swapped"] or seen["waiting"]:
            failures.append(f"in the flight {after['swapped'] - before['swapped']} people far off gave their entities up for "
                            f"others and {seen['waiting']} waited for room: more were near than may be held ({MOST_EMBODIED})")
        if sent < len(ids) // 2:
            failures.append(f"only {sent} of {len(ids)} people were sent by their routines in the flight "
                            f"({set_out:.2f} h to {landed:.2f} h)")
        if overflow:
            failures.append(f"a list or pool overflowed at stops {overflow[:5]}")

        # The shot: the street's crowd ran home, and calm again came back to work.
        print(f"living_gate: the shot ({shot['hour']:.2f} h): {len(shot['out'])} of {townsfolk} townsfolk were out, "
              f"{shot['fled']} of them running home {FLED_AFTER} stops after it; at most {seen['alarmed']} people alarmed at once")
        if not shot["fired"] or shot["fled"] is None:
            failures.append("no shot was fired in the street")
        elif len(shot["out"]) < townsfolk // 4:
            failures.append(f"only {len(shot['out'])} of {townsfolk} townsfolk were out when the shot was fired ({shot['hour']:.2f} h)")
        elif shot["fled"] < len(shot["out"]) * 3 // 4:
            failures.append(f"the shot in the street sent {shot['fled']} of the {len(shot['out'])} townsfolk out running home")
        until = time.monotonic() + 120.0
        while tool("get_people", {"alarmed": True, "limit": 0})["count"] and time.monotonic() < until:
            editor.hover(30)
        still = tool("get_people", {"alarmed": True, "limit": 0})["count"]
        if still:
            failures.append(f"{still} people are still alarmed two minutes after the shot")

        # Mid morning: everyone at work, those who fled too. The game saved.
        by_hour("at work", WORK_HOUR, {"farmer": "work", "townsfolk": "work"}, {"farmer": "fields", "townsfolk": "markets"})
        tool("save_game", {"slot": "gate", "wait": True})
        saved_hour = hour()
        saved_people = everyone(tool)

        # The midday meal: elsewhere, doing something else. Then the save loaded: as it was.
        now_hour = by_hour("at their meal", MEAL_HOUR, {"farmer": "eat", "townsfolk": "eat"},
                           {"farmer": "wells", "townsfolk": "home"})[1]
        routine_now = tool("set_routine_settings")
        if routine_now["failed"]:
            failures.append(f"{routine_now['failed']} people's routines could not send them")
        travel = tool("get_travel")
        print(f"living_gate: the day so far: {routine_now['sent']} sent by their routines, {routine_now['failed']} failed; "
              f"{travel['arrived']} trips arrived, {travel['failed']} failed, {travel['searches']} ways looked for, "
              f"{travel['reused']} reused")
        tool("load_game", {"slot": "gate", "wait": True})
        until = time.monotonic() + 120.0
        loaded = tool("get_save")
        while (loaded["doing"] or loaded["people_waiting"]) and time.monotonic() < until:
            editor.hover(10)
            loaded = tool("get_save")
        loaded_hour = hour()
        loaded_people = everyone(tool)
        if loaded["error"] or loaded["doing"] or loaded["people_waiting"]:
            failures.append(f"the game was not loaded: {loaded['error'] or loaded['doing']}, "
                            f"{loaded['people_waiting']} people still waiting")
        back = loaded_failures(saved_people, loaded_people)
        print(f"living_gate: saved at {saved_hour:.2f} h, loaded at {now_hour:.2f} h: it is {loaded_hour:.2f} h again; "
              f"{len(back)} failures of the people's places")
        failures += back
        if not saved_hour - 0.05 <= loaded_hour <= saved_hour + 0.75:
            failures.append(f"loaded, it is {loaded_hour:.2f} h, not about the {saved_hour:.2f} h it was saved at")
        editor.hover(60)
        if not tool("get_physics")["simulating"] or not hour() > loaded_hour:
            failures.append(f"loaded, the world or its day does not run on (it was {loaded_hour:.3f} h, it is {hour():.3f} h)")
        if real_gpu:
            failures += speed_failures(times)
            failures += [f"{name} leg: {f}" for name, t in leg_times.items() for f in speed_failures(t)]
        tool("simulate", {"on": False})
        # Close without saving: the world's files are the gate's, thrown away.
        tool("close_world")
        editor.close()
    except Exception as error:
        failures.append(str(error))
        if editor is not None:
            editor.kill()
    finally:
        try:
            folder.cleanup()
        except OSError as error:  # Windows: a file the killed editor still holds
            print(f"living_gate: warning: could not remove {folder.name}: {error}")
    for failure in failures:
        print(f"living_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("living_gate: PASS" if real_gpu else "living_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
