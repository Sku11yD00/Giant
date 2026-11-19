#!/usr/bin/env python3
"""The crowd gate (crowds plan CR6): three thousand people in a market
square beside the N3 gate's town, flown to, through and round.

    python3 tools/crowd_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames), and leave the window be. It builds the N3
gate's edited valley and fills a square by the town's houses: 2,000 people
standing in blocks, and 1,000 walking the lanes between them from one end
to the other and back, each in its own time and each way on its own side
(a routine a lane and a way, that sends them across every other hour of a
clock run fast). They come in three looks of five colours each, fifteen
in all. It saves that as a world and opens it again, simulating, turns
the figures on (set_figure_settings: those with no entity drawn as baked
poses) and, once the lanes are full:
- looks over the square from 30 m up and, the sun held still meanwhile,
  measures what the figures add on the GPU: them drawn and not, with the
  sun's shadows and without, in turn;
- flies in from 460 m up the valley, down a lane at head height through
  the crowd, and once round the square rising to 80 m, at 2560x1440 where
  the screen allows.
It passes when:
- the flight holds 60 fps without micro stutter (as the other gates:
  average under 16.7 ms, 99th percentile under 20.8 ms, at most one
  hitch), over the whole flight and over each leg on its own;
- the crowd is there: from the lane on, all but a twentieth of the people
  are drawn (a figure or an entity each) and no figure waits for room;
  every look's poses were baked, its walk's too, and the lanes' people
  walk (a quarter of them or more at once, as figures or entities);
- nobody appeared or went close by in the camera's view in the flight
  (set_people_settings: `appeared_in_view`, `vanished_in_view`);
- the figures add under 2 ms on the GPU where the whole square is in view
  (every timed pass with them over without them; what of it is their
  shadows is told apart: that with the shadows on over that with them
  off), and the shadows keep up with them (no more pages left behind
  than without them);
- the people's own work takes under 1 ms of the main thread a frame (the
  `travel`, `senses`, `routines` and `people` phases, picking and placing
  the figures among the last: the worst sample of the flight; drawing
  them is in `render`, and shows in the frame times);
- nothing overflowed.

On CI (the null device, which draws nothing and times nothing) a smaller
crowd is put through the same on a shorter flight, and it checks all but
the speed and the costs: tools are tested, not timings.
CROWD_GATE_PEOPLE=<standing>,<walking> fills the square with that many
whatever the device.
"""
import math
import os
import sys
import tempfile
import time

from gate_common import Editor, describe, frame_stats, leg_slow_frames, median, slow_frames, speed_failures
from gate_person import write_person
from living_gate import ground_heights, keep_look, people_cost
from n2_gate import ALL_ON, set_resolution, settle
from n3_gate import TOWN, build_valley, wait_streaming

RESOLUTION = (2560, 1440)
STANDING = 2000
WALKING = 1000
SMALL_STANDING, SMALL_WALKING = 240, 120  # off a real GPU (CI): the same square, fewer people
STOPS = 750
SMALL_STOPS = 300
SQUARE = (TOWN[0] + 62.0, TOWN[1])  # its middle (x, z): on the town's flat site, east of the houses
HALF_X, HALF_Z = 20.0, 35.0
LANES = 10                 # along z, 4 m apart: the walkers'
LANE_EVERY = 4.0
BLOCK_COLUMNS = (-0.8, 0.0, 0.8)  # a block of the standing, between two lanes: three files
ROW_EVERY = 0.9            # metres between its rows
KEEP_SIDE = 0.4            # metres from a lane's middle each way's people walk, on their own side
END_RADIUS = 0.35          # of the places at a lane's ends: each has a spot of its own within it
WALK_SPEED = 1.4           # m/s
HOUR_SECONDS = 55.0        # an hour of the square's clock: a lane walked (50 s) and a moment over
START_HOUR = 8.0           # (even: everyone crosses first, the way it faces)
FULL_HOURS = 2.0           # the lanes are full: everyone has set off in its own time
SMALL_FULL_HOURS = 0.4     # off a real GPU: some walk, which is all that is checked there
SPREAD_MINUTES = 120       # each keeps its hours late by its own share of this: a steady stream
APPROACH_END, LANE_END = 0.3, 0.6  # the legs' ends, as fractions of the flight
APPROACH_FROM = 460.0      # metres up the valley the flight sets out: beyond where figures are drawn
FIGURE_DISTANCE = 400.0    # metres figures are drawn out to (the default, set all the same)
FIGURE_KEEP = 40.0         # and farther one drawn stays so
TURN_SHARE = 0.35          # of the rise: the camera turns from looking down the lane to the square's middle
RISE_TO = 80.0             # metres over the square the flight ends
OVERLOOK = (0.0, 30.0, 80.0)  # from the square's middle (x, up, z): where the figures' cost is measured
READ_EVERY = 25            # stops between reads of the crowd in the flight
SAMPLE_EVERY = 50          # stops between the flight's reads of the frame times (the editor keeps 240 frames)
FIGURES_BUDGET_MS = 2.0    # on the GPU, the whole square in view
PEOPLE_BUDGET_MS = 1.0     # of the main thread a frame
# Each look and the colours its people come in (a person has one of them in place of the look's own).
LOOKS = (("crowd red", ([0.5, 0.12, 0.1], [0.42, 0.2, 0.12], [0.6, 0.3, 0.25], [0.3, 0.08, 0.1], [0.55, 0.45, 0.2])),
         ("crowd blue", ([0.12, 0.2, 0.45], [0.1, 0.3, 0.4], [0.3, 0.35, 0.55], [0.08, 0.1, 0.25], [0.35, 0.3, 0.28])),
         ("crowd green", ([0.15, 0.36, 0.14], [0.3, 0.4, 0.15], [0.1, 0.22, 0.12], [0.4, 0.45, 0.35], [0.6, 0.58, 0.5])))
# What is drawn at a sample, in the order they are taken: {state: (figures, the sun's shadows)}. Those
# with the shadows on follow one another, the shadows caught up first.
STATES = {"no_shadows": (True, False), "neither": (False, False), "no_figures": (False, True), "both": (True, True)}


def lane_x(lane):
    return round(SQUARE[0] + (lane - (LANES - 1) / 2) * LANE_EVERY, 2)


def standing_spots(count):
    """Where `count` people stand ((x, z, yaw)): in the blocks between the lanes, three files each,
    spread evenly over all of them however few there are."""
    rows = int(2 * HALF_Z / ROW_EVERY)
    spots = [(SQUARE[0] + (block - (LANES - 2) / 2) * LANE_EVERY + column, SQUARE[1] + (row - (rows - 1) / 2) * ROW_EVERY)
             for row in range(rows) for block in range(LANES - 1) for column in BLOCK_COLUMNS]
    if count > len(spots):
        raise ValueError(f"{count} people do not fit the square's {len(spots)} places to stand")
    chosen = []
    for k in range(count):
        x, z = spots[k * len(spots) // count]
        # (A little out of line each, the same every run, and facing any way.)
        chosen.append((round(x + 0.12 * math.sin(k * 12.9898), 2), round(z + 0.12 * math.sin(k * 78.233), 2), (k * 47) % 360))
    return chosen


def lane_ends():
    """The places at the lanes' ends, [(name, x, z)]: for each lane and each way a home at one end and
    where its people cross to at the other, on that way's own side of the lane's middle."""
    ends = []
    for lane in range(LANES):
        for way, side, from_z, to_z in (("b", KEEP_SIDE, -HALF_Z, HALF_Z), ("a", -KEEP_SIDE, HALF_Z, -HALF_Z)):
            x = round(lane_x(lane) + side, 2)
            ends.append((f"Lane {lane} home to {way}", x, SQUARE[1] + from_z))
            ends.append((f"Lane {lane} end {way}", x, SQUARE[1] + to_z))
    return ends


def walkers(count):
    """`count` walkers: [(home's name, routine, x, z, yaw)], shared among the lanes, half of each
    lane's at home at either end, set out along their side of it to begin with, facing their way."""
    made = []
    per_end = (count + 2 * LANES - 1) // (2 * LANES)  # of a lane's, at home at one end
    for k in range(count):
        lane, to_a, along = k % LANES, (k // LANES) % 2 == 1, k // (2 * LANES)
        share = (along + 0.5) / per_end
        z = SQUARE[1] + (HALF_Z - 2.0) * (2.0 * share - 1.0) * (-1.0 if to_a else 1.0)
        way = "a" if to_a else "b"
        made.append((f"Lane {lane} home to {way}", f"lane_{lane}_to_{way}",
                     round(lane_x(lane) + (-KEEP_SIDE if to_a else KEEP_SIDE), 2), round(z, 2), 180 if to_a else 0))
    return made


def routines():
    """The walkers' days, one a lane and a way: across to its end on the even hours, home on the odd."""
    def day(place):
        return [({"from": hour, "activity": "stroll", "at": "place", "name": place, "speed": WALK_SPEED} if hour % 2 == 0 else
                 {"from": hour, "activity": "stroll", "at": "home", "speed": WALK_SPEED}) for hour in range(24)]
    return {f"lane_{lane}_to_{way}": day(f"Lane {lane} end {way}") for lane in range(LANES) for way in ("a", "b")}


def leg_of(stop, steps):
    t = stop / max(steps - 1, 1)
    return "approach" if t < APPROACH_END else "lane" if t < LANE_END else "rise"


def leg_ends(steps):
    """The last stop of the approach and of the lane."""
    return [max(i for i in range(steps) if leg_of(i, steps) == leg) for leg in ("approach", "lane")]


def flight_path(steps, ground):
    """In from up the valley towards the square, down a lane at head height, then once round the
    square out and up, turning from the way the lane was flown to look at the square's middle.
    `ground(points)` gives the ground's height at [x, z]."""
    lane = lane_x(LANES // 2)
    z_in, z_out = SQUARE[1] + HALF_Z + 6.0, SQUARE[1] - HALF_Z - 6.0
    # (The rise begins where the lane ended: at this angle round the middle, this far from it.)
    angle0, r0 = math.atan2(lane - SQUARE[0], HALF_Z + 6.0), math.hypot(lane - SQUARE[0], HALF_Z + 6.0)
    plan = []
    for i in range(steps):
        t = i / max(steps - 1, 1)
        leg = leg_of(i, steps)
        if leg == "approach":  # down towards the square's end, looking at its middle
            u = t / APPROACH_END
            plan.append((lane, z_in + (1.0 - u) * APPROACH_FROM, 25.0 + (1.8 - 25.0) * u, None))
        elif leg == "lane":  # down the lane, between the two streams
            u = (t - APPROACH_END) / (LANE_END - APPROACH_END)
            plan.append((lane, z_in + u * (z_out - z_in), 1.8, (0.0, 60.0, 0.0)))
        else:
            u = (t - LANE_END) / (1.0 - LANE_END)
            angle, r = angle0 + u * 2.0 * math.pi, r0 + u * 50.0
            turned = min(u / TURN_SHARE, 1.0)
            turned = turned * turned * (3.0 - 2.0 * turned)
            # (Looking along -z as the lane ended, then round to the middle: there, `r` off, at the end.)
            heading = angle - angle0 * (1.0 - turned) + math.pi * turned
            plan.append((SQUARE[0] + math.sin(angle) * r, SQUARE[1] - math.cos(angle) * r, 1.8 + u * (RISE_TO - 1.8),
                         (heading, 60.0 + turned * (r - 60.0), turned)))
    heights = ground([[x, z] for x, z, _, _ in plan])
    middle = ground([[SQUARE[0], SQUARE[1]]])[0]
    path = []
    for i, (x, z, over, look) in enumerate(plan):
        near = [h for h in heights[max(0, i - 3):i + 4] if h is not None]
        y = (max(near) if near else 0.0) + over
        if look is None:
            look_at = [SQUARE[0], middle + 1.0, SQUARE[1]]
        else:
            heading, off, turned = look
            level = y - 0.2
            look_at = [x + math.sin(heading) * off, level + turned * (middle + 1.0 - level), z - math.cos(heading) * off]
        path.append(([round(x, 3), round(y, 3), round(z, 3)], [round(c, 3) for c in look_at]))
    return path


def added_cost(rounds):
    """What the figures add on the GPU, from `rounds` of the timed passes ({state: {pass: ms}}, a state
    each of STATES): (the ms they add with the shadows on, over every pass all four timed; what of it
    is their shadows: that less what they add with the shadows off; the passes that change by 0.05 ms
    or more with the shadows on, most first), each the median over the rounds. None when nothing was timed."""
    names = set.intersection(*(set(r[state]) for r in rounds for state in STATES)) if rounds else set()
    if not names:
        return None

    def over(r, with_, without, which=names):
        return sum(r[with_][name] - r[without][name] for name in which)

    total = median([over(r, "both", "no_figures") for r in rounds])
    shadows = median([over(r, "both", "no_figures") - over(r, "no_shadows", "neither") for r in rounds])
    by_pass = {name: median([over(r, "both", "no_figures", (name,)) for r in rounds]) for name in names}
    return total, shadows, {name: ms for name, ms in sorted(by_pass.items(), key=lambda p: -abs(p[1])) if abs(ms) >= 0.05}


def crowd_failures(total, walking, seen, figures, looks):
    """What the crowd fails: `seen` from the flight's reads from the lane on (`drawn_least`: the fewest
    drawn, figures and entities together; `figures_waiting`: the most figures waiting for room;
    `walking_most`: the most walking, figures and entities together), `figures` the figures' counts as last read
    (set_figure_settings), of `looks` looks. Lines."""
    failures = []
    if seen["drawn_least"] < total - total // 20:
        failures.append(f"at fewest {seen['drawn_least']} of the {total} people were drawn from the lane on")
    if seen["figures_waiting"]:
        failures.append(f"{seen['figures_waiting']} people waited for room to be drawn as figures")
    if figures["looks_without"] or figures["looks_baking"]:
        failures.append(f"{figures['looks_without']} looks have no pose to draw their people with, "
                        f"{figures['looks_baking']} are still being baked")
    # (A look's standing pose and its walk's: without the walk's, those travelling slide as they stand.)
    wanted = looks * (1 + figures["walk_poses"])
    if walking and figures["poses"] < wanted:
        failures.append(f"{figures['poses']} poses are kept for the figures, not the {wanted} of {looks} looks standing "
                        f"and walking")
    if walking and seen["walking_most"] < max(walking // 4, 1):
        failures.append(f"at most {seen['walking_most']} people walked, as figures or entities (of {walking} walkers)")
    return failures


def shadow_failures(pages):
    """What the sun's shadows over the square fail, from `pages` ({state: {"drawn", "waiting", "behind"}},
    the medians a frame with the figures and without, the shadows on): the figures' shadows are
    kept up with (no more pages left behind than without them). Lines."""
    with_, without = pages["both"], pages["no_figures"]
    if with_["behind"] > without["behind"]:
        return [f"the sun's shadows fall behind with the figures: {with_['behind']} pages behind a frame "
                f"({without['behind']} without them), {with_['waiting']} waiting, {with_['drawn']} drawn"]
    return []


def sample(editor, frames):
    """Over `frames` frames: (the median GPU ms of each timed pass, the median of the sun's shadow
    pages drawn, waiting and behind a frame)."""
    passes, pages = {}, {"drawn": [], "waiting": [], "behind": []}
    for _ in range(frames):
        editor.call("ping")
        for p in editor.tool("get_engine_info").get("gpu", {}).get("passes", []):
            passes.setdefault(p["name"], []).append(p["ms"])
        shadows = editor.last_frame()["shadows"]
        for key in pages:
            pages[key].append(shadows[key])
    return {name: median(ms) for name, ms in passes.items()}, {key: median(values) for key, values in pages.items()}


def fill_square(tool, whole, standing, walking):
    """Fills the square in the valley build_valley made: the lanes' ends, the looks, the routines and
    the people, and sets the clock. Returns the people's ids."""
    def ground(points):
        return [round(h, 3) for h in ground_heights(tool, points)]

    ends = lane_ends()
    for (name, x, z), h in zip(ends, ground([(x, z) for _, x, z in ends])):
        tool("create_place", {"name": name, "position": [x, h, z], "radius": END_RADIUS})
    town_h = ground([SQUARE])[0]
    for i, (name, tints) in enumerate(LOOKS):
        keep_look(tool, name, whole, tints[0], [SQUARE[0] + 40.0 + i, town_h, SQUARE[1]], list(tints))
    for name, entries in routines().items():
        tool("set_routine", {"name": name, "entries": entries})
    people = []
    stood = standing_spots(standing)
    for k, ((x, z, yaw), h) in enumerate(zip(stood, ground([(x, z) for x, z, _ in stood]))):
        people.append({"look": LOOKS[k % len(LOOKS)][0], "position": [x, h, z], "yaw": yaw, "seed": k + 1})
    walk = walkers(walking)
    for k, ((home, routine, x, z, yaw), h) in enumerate(zip(walk, ground([(x, z) for _, _, x, z, _ in walk]))):
        people.append({"look": LOOKS[k % len(LOOKS)][0], "home": home, "routine": routine, "position": [x, h, z],
                       "yaw": yaw, "seed": standing + k + 1})
    ids = []
    for i in range(0, len(people), 4096):
        ids += tool("create_people", {"people": people[i:i + 4096]})["ids"]
    tool("set_time", {"on": True, "hours": START_HOUR, "day": 172, "latitude": 48, "speed": 3600.0 / HOUR_SECONDS})
    return ids


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
        editor = Editor(sys.argv[1], sys.argv[2:], "crowd_gate", timeout=1700)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"crowd_gate: {info['adapter']} ({info['backend']})")
        real_gpu = editor.real_gpu()
        if not real_gpu:
            print("crowd_gate: warning: not a real GPU, the timings mean nothing")
        width, height = set_resolution(editor, *RESOLUTION)
        print(f"crowd_gate: {width}x{height} pixels")
        if real_gpu and (width, height) != RESOLUTION:
            print(f"crowd_gate: warning: the budgets are for {RESOLUTION[0]}x{RESOLUTION[1]}")
        build_valley(tool)
        tool("import_mesh", {"path": whole})
        standing, walking = (STANDING, WALKING) if real_gpu else (SMALL_STANDING, SMALL_WALKING)
        if os.environ.get("CROWD_GATE_PEOPLE"):
            standing, walking = (int(n) for n in os.environ["CROWD_GATE_PEOPLE"].split(","))
        stops = STOPS if real_gpu else SMALL_STOPS
        ids = fill_square(tool, whole, standing, walking)
        total = len(ids)

        path = flight_path(stops, lambda points: ground_heights(tool, points))
        middle = ground_heights(tool, [SQUARE])[0]
        overlook = ([SQUARE[0] + OVERLOOK[0], round(middle + OVERLOOK[1], 3), SQUARE[1] + OVERLOOK[2]],
                    [SQUARE[0], round(middle + 1.0, 3), SQUARE[1]])
        world_folder = os.path.join(folder.name, "valley")
        saved = tool("save_world", {"folder": world_folder})
        print(f"crowd_gate: saved {saved['cells']} cells with {total} people in the square ({standing} standing, "
              f"{walking} walking {LANES} lanes; {person['triangles']} triangles a person, {len(LOOKS)} looks, "
              f"{sum(len(tints) for _, tints in LOOKS)} colours)")
        approach_end, lane_end = leg_ends(stops)
        tool("set_camera", {"position": overlook[0], "look_at": overlook[1]})
        tool("open_world", {"folder": world_folder})
        tool("set_render_settings", ALL_ON)
        tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        world, scatter, streamed = wait_streaming(editor, 6000)
        last, settled = settle(editor, 6000)
        if not (streamed and settled):
            failures.append("loading did not catch up in 6000 frames")
        print(f"crowd_gate: loaded: {world['loaded']} cells, {scatter['instances']} scatter instances")
        if tool("get_people", {"limit": 0})["total"] != total:
            failures.append(f"opened, the world has {tool('get_people', {'limit': 0})['total']} people, not {total}")
        tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        tool("set_routine_settings", {"spread_minutes": SPREAD_MINUTES})
        tool("set_figure_settings", {"on": True, "distance": FIGURE_DISTANCE, "keep": FIGURE_KEEP})
        started = tool("simulate", {"on": True, "clock": True})
        if not started.get("clock"):
            failures.append("the clock does not run while simulating")

        def hour():
            return tool("get_time")["shown"]["hours"]

        def drawn():
            """(The figures' counts, the people's, how many are drawn: a figure or an entity each.)"""
            figures, held = tool("set_figure_settings"), tool("set_people_settings")
            return figures, held, figures["figures"] + held["embodied"]

        def wait_drawn(seconds=120.0):
            """Hovers until all but a fiftieth of the people are drawn (figures are made a few a frame)."""
            end = time.monotonic() + seconds
            while drawn()[2] < total - total // 50 and time.monotonic() < end:
                editor.hover(20)
            return drawn()

        # The lanes fill: everyone sets off in its own time over two hours of the square's clock.
        full = START_HOUR + (FULL_HOURS if real_gpu else SMALL_FULL_HOURS)
        end = time.monotonic() + 600.0
        while hour() < full and time.monotonic() < end:
            editor.hover(30)
        figures, held, there = wait_drawn()
        settings = {k: held[k] for k in ("embody_distance", "release_distance", "view_distance", "most")}
        print(f"crowd_gate: {hour():.2f} h, over the square: {there} of {total} drawn ({figures['figures']} figures, "
              f"{figures['walking']} of them walking; {held['embodied']} entities, {held['walking']} walking); "
              f"entities as set: {settings}; figures out to {figures['distance']:.0f} m, at most {figures['most']}")

        # What the figures add on the GPU, the whole square in view: drawn and not, with the sun's
        # shadows and without, in turn. The sun is held meanwhile (it turns fast with this clock, and
        # every shadow page is drawn again each quarter degree: that would be what is measured). The
        # walkers walk on: trips go by the simulation's seconds.
        clock_speed = tool("get_time")["clock"]["speed"]
        tool("set_time", {"speed": 0.0})
        editor.hover(120)
        rounds, pages = [], {state: [] for state in STATES}
        remade = True
        for _ in range(3):
            timed = {}
            for state, (with_figures, with_shadows) in STATES.items():
                tool("set_figure_settings", {"on": with_figures})
                turned_on = with_shadows and not tool("get_render_settings")["shadows"]
                tool("set_render_settings", {"shadows": with_shadows})
                if with_figures:
                    remade = wait_drawn()[2] >= total - total // 50 and remade
                if turned_on:
                    settle(editor, 300)  # (every page drawn again first: not what the figures cost)
                editor.hover(30)
                timed[state], shadows = sample(editor, 20)
                pages[state].append(shadows)
            rounds.append(timed)
        tool("set_figure_settings", {"on": True})
        tool("set_render_settings", {"shadows": True})
        tool("set_time", {"speed": clock_speed})
        if not remade:
            failures.append("turned on again, the figures were not all made again in two minutes")
        pages = {state: {key: median([p[key] for p in found]) for key in ("drawn", "waiting", "behind")}
                 for state, found in pages.items()}
        print(f"crowd_gate: the sun's shadow pages a frame over the square: with the figures {pages['both']['drawn']} drawn, "
              f"{pages['both']['waiting']} waiting, {pages['both']['behind']} behind; without them "
              f"{pages['no_figures']['drawn']} drawn, {pages['no_figures']['waiting']} waiting, {pages['no_figures']['behind']} behind")
        failures += shadow_failures(pages)
        added = added_cost(rounds)
        if added is None:
            if real_gpu:
                failures.append("the GPU timed no passes: what the figures add is not known")
        else:
            parts = ", ".join(f"{name} {ms:+.2f}" for name, ms in added[2].items())
            print(f"crowd_gate: the figures add {added[0]:.2f} ms on the GPU over the square, {added[1]:.2f} ms of it "
                  f"their shadows ({parts or 'no pass changes by 0.05 ms'})")
            if real_gpu and added[0] > FIGURES_BUDGET_MS:
                failures.append(f"the figures add {added[0]:.2f} ms on the GPU over the square, over {FIGURES_BUDGET_MS} ms "
                                f"({added[1]:.2f} ms of it their shadows; {parts})")

        # The flight.
        tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        editor.hover(60)
        before, figures_before = tool("set_people_settings"), tool("set_figure_settings")
        overflow = []
        seen = {"drawn_least": total, "walking_most": 0, "embodied": 0, "unseen": 0, "waiting": 0, "figures": 0,
                "figures_waiting": 0, "figures_walking": 0}

        def each_stop(stop, frame):
            if stop % READ_EVERY == 0:
                figures, held, there = drawn()
                if stop > approach_end:
                    seen["drawn_least"] = min(seen["drawn_least"], there)
                    seen["figures_waiting"] = max(seen["figures_waiting"], figures["waiting"])
                seen["walking_most"] = max(seen["walking_most"], figures["walking"] + held["walking"])
                seen["figures_walking"] = max(seen["figures_walking"], figures["walking"])
                seen["figures"] = max(seen["figures"], figures["figures"])
                for key in ("embodied", "unseen", "waiting"):
                    seen[key] = max(seen[key], held[key])
            if frame["overflow"] or frame["voxels"]["overflow"] or frame["decal_overflow"] or frame["light_overflow"]:
                overflow.append(stop)

        flown = editor.fly(path, each_stop, marks=(approach_end, lane_end, *range(SAMPLE_EVERY - 1, stops, SAMPLE_EVERY)))
        times = frame_stats(flown)
        slow = slow_frames(flown, editor.slow, editor.typicals, editor.slow_over_ms, leg=lambda at: leg_of(at, stops))
        if slow:
            print(f"crowd_gate: {slow}")
        print(f"crowd_gate: frame time over the flight ({times['frames']} frames sampled): {describe(times)}")
        cuts = [0, editor.marked[approach_end], editor.marked[lane_end], len(flown)]
        leg_times = {}
        for i, name in enumerate(("approach", "lane", "rise")):
            leg_times[name] = frame_stats(flown[cuts[i]:cuts[i + 1]])
            print(f"crowd_gate: flying {name}: {describe(leg_times[name])}")
            told = leg_slow_frames(flown[cuts[i]:cuts[i + 1]], editor.slow, editor.typicals, editor.slow_over_ms,
                                   lambda at, name=name: leg_of(at, stops) == name) if real_gpu else None
            if told:
                print(f"crowd_gate: flying {name}: {told}")
        after, figures_after = tool("set_people_settings"), tool("set_figure_settings")
        appeared = after["appeared_in_view"] - before["appeared_in_view"]
        vanished = after["vanished_in_view"] - before["vanished_in_view"]
        print(f"crowd_gate: in the flight: at fewest {seen['drawn_least']} of {total} drawn from the lane on; at most "
              f"{seen['figures']} figures, {seen['figures_walking']} of them walking ({seen['walking_most']} with the entities walking); at most {seen['embodied']} entities "
              f"({after['made'] - before['made']} made, {after['released'] - before['released']} released, "
              f"{after['swapped'] - before['swapped']} swapped; {figures_after['made'] - figures_before['made']} figures "
              f"made, {figures_after['taken_over'] - figures_before['taken_over']} of them where an entity stood); "
              f"{appeared} appeared and {vanished} went close by in view; at most {seen['unseen']} waited to be out "
              f"of sight, {seen['waiting']} for room")
        failures += crowd_failures(total, walking, seen, figures_after, len(LOOKS))
        if appeared or vanished:
            failures.append(f"in the flight {appeared} people appeared and {vanished} went close by in view")
        told, over = people_cost(editor.typicals, PEOPLE_BUDGET_MS, real_gpu)
        if told:
            print(f"crowd_gate: {told}")
        failures += over
        routine = tool("set_routine_settings")
        if routine["failed"]:
            failures.append(f"{routine['failed']} people's routines could not send them")
        if overflow:
            failures.append(f"a list or pool overflowed at stops {overflow[:5]}")
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
            print(f"crowd_gate: warning: could not remove {folder.name}: {error}")
    for failure in failures:
        print(f"crowd_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("crowd_gate: PASS" if real_gpu else "crowd_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
