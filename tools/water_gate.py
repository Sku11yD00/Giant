#!/usr/bin/env python3
"""The water gate (water plan W6): the N3 gate's valley with water in it,
a lake up the valley, a river down its channel past the town and out to
a coast, flown low over the lake, under its surface and up, down the
river past the town, and out over the sea.

    python3 tools/water_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames). It builds the N3 gate's edited valley (the
river's channel, the ridges, the town, the hamlet, the forest), then
puts water in it: a basin dug west of the river holds a lake; a river's
bed is cut down the channel to a line that never rises downstream (a
gorge where the valley's floor rises, along the town's west edge among
them) and the river runs down it, falling to a coast dug out across the
valley's foot, where the sea comes in. Crates are dropped into the lake
and the river (splashes, floating, drifting, wet a while after). It
saves that as a world, opens it again with every N2 and N3 feature on in
fair weather, the simulation running, and flies (about 50 seconds at 60
fps) at 2560x1440 where the screen allows:
- over the lake, low, then down under its surface and up again;
- over to the river, then down it low over the water, up over the
  gorge's rim beside the town (the houses and the river both in view);
- out over the coast and the sea, then up, looking back at the shore.
It passes when:
- the flight holds 60 fps without micro stutter (as the other gates:
  average under 16.7 ms, 99th percentile under 20.8 ms, at most one
  hitch), over the whole flight and over each leg on its own;
- the water was there: the lake holding water in its middle, the river
  along its length and across its width, each leg flown over its own
  water, the camera under the surface on the lake leg (and nowhere else),
  nothing dropped or overflowed, no lake broken, the crates in the water
  and wet, streaming never failed;
- nothing of the valley but the coast lies under the sea (no flooding);
- the water takes under 1.5 ms of GPU time (docs/architecture.md's
  budget: its own pass, "water", with the reflections, refraction,
  caustics and the view under the surface, plus what shading, the
  voxel world and its light ("voxels", "gi") take over themselves with
  the water off: voxel cones meeting it, its bricks) over the lake,
  under it, by the river at the town and over the sea, each caught up
  after every change first, each part printed.

On CI (the null device) it checks all but the speed: tools are tested,
not timings.
"""
import math
import os
import sys
import tempfile

from gate_common import Editor, describe, frame_stats, slow_frames, speed_failures
from n2_gate import ALL_ON, set_resolution, settle
from n3_gate import HALF, HOUSES_X, SPACING, TOWN, build_valley, wait_streaming

RESOLUTION = (2560, 1440)
STOPS = 1500              # two frames each: about 50 s at 60 fps, the river flown at about 85 m/s
WATER_BUDGET_MS = 1.5
LAKE = (-300.0, 450.0)      # the lake's centre (x, z), west of the river above the town
LAKE_RADIUS = 110.0
LAKE_DIG = 10.0             # its bed this far under its shore's lowest (before digging)
COAST_Z = -1500.0           # the coast: the valley's foot south of here dug out to the sea floor
SEA_FLOOR = -40.0
SEA_LEVEL = -30.0
RIVER_TOP = 760             # the river's source (z), past the lake
RIVER_DEPTH = 1.6           # the river's water over its bed
RIVER_WIDTH = 14.0
RIVER_CUT = 2.0             # its bed under the lowest of the channel's ground so far
RIVER_FALL = 0.05           # its last stretch falls this much a metre to the sea
TOWN_EDGE_X = -(HOUSES_X * SPACING / 2 + 18.0)  # the river bends round the town's houses, 18 m west of them
LAKE_END, RIVER_END = 0.2, 0.8  # the legs' ends, as fractions of the flight
RIVER_FROM = LAKE[1] - 50.0  # the river leg flies down it from here
CROSS = 0.2                 # the share of the river leg spent crossing over to it from the lake
RIVER_DAB = 3.5             # metres between the bed's dabs (a quarter of its width: no sills left)
WATER_OFF = {"water": False}
WATER_ON = {"water": True}
CRATES = 24
MOST_ASTRAY = 4             # crates that may miss the water (land on a bank)


def river_x(z):
    """The river's line: the N3 channel's, bent round the town's houses."""
    x = 40.0 * math.sin(z / 300.0)
    bend = max(0.0, 1.0 - abs(z - TOWN[1]) / 160.0)  # 1 at the town, 0 160 m either side
    bend = bend * bend * (3.0 - 2.0 * bend)
    return x + (TOWN_EDGE_X - x) * bend


def put_water(tool):
    """Digs the lake's basin, the river's bed and the coast, and fills them.
    Returns (lake level, the river's path)."""
    edge = HALF - 64
    # The coast: the valley's foot dug down to the sea floor (flattened along a line across it).
    coast = [[x, z] for z in (COAST_Z - 250.0, -edge) for x in range(-int(edge), int(edge) + 1, 200)]
    for i in range(0, len(coast), 4):  # (a few dabs a call: a stroke's reach is capped)
        tool("sculpt_terrain", {"op": "flatten", "points": coast[i:i + 4], "radius": 320, "strength": 1,
                                "height": SEA_FLOOR, "falloff": 0.6})
    # The lake's basin: flattened low; the lake at the height of the lowest of its shore once dug
    # (the dig's edge lowers the shore a little too).
    shore = [[LAKE[0] + math.cos(a) * LAKE_RADIUS, LAKE[1] + math.sin(a) * LAKE_RADIUS]
             for a in (i * 2.0 * math.pi / 48 for i in range(48))]
    rim = min(tool("get_terrain", {"points": shore})["heights"])
    tool("sculpt_terrain", {"op": "flatten", "points": [list(LAKE)], "radius": LAKE_RADIUS * 1.15, "strength": 1,
                            "height": rim - LAKE_DIG, "falloff": 0.35})
    lake_level = min(tool("get_terrain", {"points": shore})["heights"]) - 0.3
    tool("create_water", {"name": "Lake", "kind": "lake", "level": round(lake_level, 3),
                          "outline": [[round(x, 2), round(z, 2)] for x, z in shore], "waves": 0.6})
    # The river: from up the valley (past the lake) down the channel, round the town, to the sea. Its
    # bed is cut down the channel to a line that never rises downstream: the lowest of the ground so
    # far, less a gorge's depth, and in its last stretch falling to under the sea (the valley's floor
    # rises and falls: the river cuts a gorge through the rises, the town's site among them).
    zs = list(range(RIVER_TOP, -int(edge) - 1, -20))
    line = [[round(river_x(z), 2), float(z)] for z in zs]
    ground = tool("get_terrain", {"points": line})["heights"]
    sea_at = next((z for (x, z), g in zip(line, ground) if g < SEA_LEVEL), line[-1][1])
    path = []
    lowest = None
    for (x, z), g in zip(line, ground):
        lowest = g - RIVER_CUT if lowest is None else min(lowest, g - RIVER_CUT)
        bed = min(lowest, SEA_LEVEL - RIVER_DEPTH + (z - sea_at) * RIVER_FALL)
        level = max(bed + RIVER_DEPTH, SEA_LEVEL)  # (into the sea at its level)
        path.append({"x": x, "z": z, "width": RIVER_WIDTH, "level": round(level, 3), "bed": bed})
    # The bed dug in dabs a quarter of its width apart (their heights between the path's points), those
    # of a height (to 5 cm) a stroke each, down to where the ground is under the sea already.
    dabs = []
    z = float(RIVER_TOP)
    while z >= sea_at:
        i = min(int((RIVER_TOP - z) // 20), len(path) - 2)
        u = (path[i]["z"] - z) / 20.0
        bed = path[i]["bed"] + (path[i + 1]["bed"] - path[i]["bed"]) * u
        dabs.append(([round(river_x(z), 2), round(z, 2)], round(bed / 0.05) * 0.05))
        z -= RIVER_DAB
    run = []
    for k, (point, bed) in enumerate(dabs):
        run.append(point)
        if k + 1 == len(dabs) or dabs[k + 1][1] != bed or len(run) == 64:
            tool("sculpt_terrain", {"op": "flatten", "points": run, "radius": RIVER_WIDTH, "strength": 1,
                                    "height": round(bed, 3), "falloff": 0.5})
            run = []
    for p in path:
        del p["bed"]
    tool("create_water", {"name": "River", "kind": "river", "path": path, "flow": 1.5, "waves": 0.8})
    tool("create_water", {"name": "Sea", "kind": "sea", "level": SEA_LEVEL, "waves": 1.0})
    # Crates: dropped into the lake and the river (by the town), floating.
    crates = []
    for i in range(CRATES):
        if i < CRATES // 2:
            a = i * 2.0 * math.pi / (CRATES // 2)
            x, z, y = LAKE[0] + math.cos(a) * 40.0, LAKE[1] + math.sin(a) * 40.0, lake_level + 3.0
        else:
            z = TOWN[1] + 200.0 - (i - CRATES // 2) * 30.0
            x = river_x(z)
            y = next(p["level"] for p in path if p["z"] <= z) + 3.0
        crates.append({"name": f"Crate {i}", "shape": "cube", "position": [round(x, 2), round(y, 2), round(z, 2)],
                       "scale": [1.0, 0.8, 1.2], "base_color": [0.45, 0.32, 0.18], "roughness": 0.85,
                       "surface": "wood", "body": "dynamic", "mass": 300})
    tool("create_entities", {"entities": crates})
    return lake_level, path


def leg_of(stop, steps):
    t = stop / max(steps - 1, 1)
    return "lake" if t < LAKE_END else "river" if t < RIVER_END else "coast"


def leg_ends(steps):
    """The last stop of the lake and river legs."""
    return [max(i for i in range(steps) if leg_of(i, steps) == leg) for leg in ("lake", "river")]


def ease(v, a=0.05):
    """0 to 1 as v goes 0 to 1, from still up to speed over the first `a`
    and down to still again over the last."""
    v = min(max(v, 0.0), 1.0)
    if v < a:
        d = v * v / (2.0 * a)
    elif v > 1.0 - a:
        d = 1.0 - a - (1.0 - v) * (1.0 - v) / (2.0 * a)
    else:
        d = v - a / 2.0
    return d / (1.0 - a)


def flight_path(steps, lake_level, path, town_h, ground):
    """Over the lake and under it, over to the river and down it (over the
    gorge's rim beside the town), out over the sea and up. Heights over the
    water (the river's level from its path), and never under the ground
    (`ground(points)`: its height at [x, z]) but in the dive. Returns the
    stops ((position, look_at) each), which of them dive (the camera may be
    under the surface) and which of those are meant to be well under it."""
    def river_level(z):
        for a, b in zip(path, path[1:]):
            if b["z"] <= z <= a["z"]:
                u = (a["z"] - z) / max(a["z"] - b["z"], 1e-6)
                return a["level"] + (b["level"] - a["level"]) * u
        return path[-1]["level"] if z < path[-1]["z"] else path[0]["level"]

    plan = []
    river_bottom = COAST_Z - 150.0
    for i in range(steps):
        t = i / max(steps - 1, 1)
        leg = leg_of(i, steps)
        diving = False
        if leg == "lake":  # across the lake, down under at its middle, up, on to its east side
            u = t / LAKE_END
            x = LAKE[0] - LAKE_RADIUS * 0.6 + u * LAKE_RADIUS * 1.2
            z = LAKE[1] - 10.0 * math.sin(u * math.pi)
            dive = math.sin(min(max((u - 0.3) / 0.4, 0.0), 1.0) * math.pi)  # under from 0.3 to 0.7 of the way
            diving = 0.3 < u < 0.7
            y = lake_level + 3.0 - dive * 7.0
            look = [x + 60.0, y - 1.5 if dive < 0.5 else y - 0.5, z]
        elif leg == "river":  # over the ground to the river, then down it low over the water
            u = (t - LAKE_END) / (RIVER_END - LAKE_END)
            cross = CROSS
            v = ease(max(0.0, (u - cross) / (1.0 - cross)))
            z = RIVER_FROM + v * (river_bottom - RIVER_FROM)
            x = river_x(z)
            # Over the water, rising over the gorge's rim beside the town (the houses in view).
            w = max(0.0, 1.0 - abs(z - TOWN[1]) / 250.0)
            w = w * w * (3.0 - 2.0 * w)
            y = (river_level(z) + 6.0) * (1.0 - w) + (town_h + 6.0) * w
            if u < cross:  # up over the ground between, and down to the river
                k = u / cross
                k = k * k * (3.0 - 2.0 * k)
                lake_x = LAKE[0] + LAKE_RADIUS * 0.6
                start = lake_level + 3.0
                x = lake_x + (x - lake_x) * k
                z = LAKE[1] + (z - LAKE[1]) * k
                y = start + (y - start) * k + 40.0 * math.sin(math.pi * k)  # up over the ground between
            look = [river_x(z - 60.0), y - 2.0, z - 60.0]
        else:  # out over the sea low, then up, turning to look back at the shore
            u = (t - RIVER_END) / (1.0 - RIVER_END)
            z = river_bottom - u * 300.0
            x = river_x(river_bottom) + 200.0 * u
            y = SEA_LEVEL + 6.0 + 250.0 * u * u
            heading = math.pi * min(1.0, u * 1.5)  # from looking down the coast to looking back up the valley
            look = [x + math.sin(heading) * 1000.0, y * 0.5, z - math.cos(heading) * 1000.0]
        plan.append((x, y, z, look, diving))
    heights = ground([[p[0], p[2]] for p in plan])
    out = []
    dives = []
    deep = []
    for i, (x, y, z, look, diving) in enumerate(plan):
        if diving:  # (over the lake's bed)
            if heights[i] is not None:
                y = max(y, heights[i] + 1.0)
            dives.append(i)
            if y < lake_level - 1.0:  # (clear of the waves)
                deep.append(i)
        else:  # over the highest ground a few stops either side (no dipping into a bank)
            near = [h for h in heights[max(0, i - 3):i + 4] if h is not None]
            if near:
                y = max(y, max(near) + 2.0)
        out.append([[x, y, z], look])
    # The camera never drops or climbs more than 2 m a stop to clear the ground (only ever raised:
    # the clearances hold), so it goes smoothly over a bank.
    for i in range(len(out) - 2, -1, -1):
        out[i][0][1] = max(out[i][0][1], out[i + 1][0][1] - 2.0)
    for i in range(1, len(out)):
        out[i][0][1] = max(out[i][0][1], out[i - 1][0][1] - 2.0)
    deep = [i for i in deep if out[i][0][1] < lake_level - 1.0]
    for (x, y, z, look, diving), stop in zip(plan, out):  # (looking as it would have, raised with it)
        stop[1] = [look[0], look[1] + stop[0][1] - y, look[2]]
    out = [([round(c, 3) for c in p], [round(c, 3) for c in look]) for p, look in out]
    return out, set(dives), set(deep)


def water_cost(editor, at, rounds=3):
    """GPU ms of the water at a stop: its own pass ("water": reflections,
    refraction, caustics, the view under the surface) plus what shading,
    the voxel world and its light ("voxels", "gi") take over themselves
    with the water off (voxel cones meeting it, its bricks), the median
    over `rounds`, on and off in turn. The stop is caught up first, and
    after each change (the water's bricks leave and come back to the voxel
    world). Returns (ms or None when the GPU did not time those passes, its
    parts, whether it caught up, whether the water went off and on as asked)."""
    def catch_up():
        editor.tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        streamed = wait_streaming(editor, 6000)[2]
        settled = settle(editor, 6000)[1]
        editor.tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        editor.hover(10)
        return streamed and settled

    editor.tool("set_camera", {"position": at[0], "look_at": at[1]})
    caught_up = catch_up()
    costs = []
    toggled = True
    for _ in range(rounds):
        on = editor.gpu_passes(20)
        toggled = toggled and editor.last_frame()["water"]["triangles"] > 0
        editor.tool("set_render_settings", WATER_OFF)
        caught_up = catch_up() and caught_up
        off = editor.gpu_passes(20)
        toggled = toggled and editor.last_frame()["water"]["triangles"] == 0
        editor.tool("set_render_settings", WATER_ON)
        caught_up = catch_up() and caught_up
        if "shading" not in on or "shading" not in off:
            return None, None, caught_up, toggled
        parts = {"water": on.get("water", 0.0), "shading": on["shading"] - off["shading"],
                 "voxel world": on.get("voxels", 0.0) - off.get("voxels", 0.0) + on.get("gi", 0.0) - off.get("gi", 0.0)}
        costs.append((sum(parts.values()), parts))
    costs.sort(key=lambda c: c[0])
    cost, parts = costs[len(costs) // 2]
    return cost, parts, caught_up, toggled


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    failures = []
    real_gpu = False
    editor = None
    folder = tempfile.TemporaryDirectory()
    try:
        editor = Editor(sys.argv[1], sys.argv[2:], "water_gate", timeout=1700)  # (under ctest's 1800)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"water_gate: {info['adapter']} ({info['backend']})")
        real_gpu = editor.real_gpu()
        if not real_gpu:
            print("water_gate: warning: not a real GPU, the timings mean nothing")
        width, height = set_resolution(editor, *RESOLUTION)
        print(f"water_gate: {width}x{height} pixels")
        if real_gpu and (width, height) != RESOLUTION:
            print(f"water_gate: warning: the budgets are for {RESOLUTION[0]}x{RESOLUTION[1]}")
        town_h = build_valley(tool)
        lake_level, river = put_water(tool)
        # Nothing but the coast under the sea: the valley north of it sampled every 64 m.
        edge = HALF - 64
        land = [[x, z] for x in range(-int(edge), int(edge) + 1, 64)
                for z in range(int(COAST_Z) + 400, int(edge) + 1, 64)]
        heights = []
        for i in range(0, len(land), 4096):
            heights += tool("get_terrain", {"points": land[i:i + 4096]})["heights"]
        flooded = [p for p, h in zip(land, heights) if h is not None and h < SEA_LEVEL + 0.5]
        if flooded:
            failures.append(f"{len(flooded)} places north of the coast lie under the sea, e.g. {flooded[:3]}")
        # The water is in what was dug: the lake's middle, the river along its length and across it
        # (every 10 m down its middle and 0.4 of its width either side, down to where it meets the sea).
        probes = [list(LAKE)]
        for a, b in zip(river, river[1:]):
            if b["level"] <= SEA_LEVEL:
                break
            for u in (0.0, 0.5):
                z = a["z"] + (b["z"] - a["z"]) * u
                probes += [[river_x(z) + dx * RIVER_WIDTH, z] for dx in (-0.4, 0.0, 0.4)]
        found = tool("get_water", {"points": probes})["at"]
        if not (found[0] and found[0]["name"] == "Lake" and (found[0]["depth"] or 0) > 5.0):
            failures.append(f"the lake's middle holds no lake 5 m deep: {found[0]}")
        wet = sum(1 for w in found[1:] if w and w["name"] == "River" and (w["depth"] or 0) > 0.3)
        if wet < 0.95 * (len(found) - 1):
            failures.append(f"the river holds water at only {wet} of {len(found) - 1} places along and across it")
        print(f"water_gate: the river has water at {wet} of {len(found) - 1} places along and across it")
        print(f"water_gate: the lake at {lake_level:.1f} m, the river from {river[0]['level']:.1f} m down to "
              f"{river[-1]['level']:.1f} m, the sea at {SEA_LEVEL} m; the town at {town_h:.1f} m")
        def ground(points):
            heights = []
            for i in range(0, len(points), 4096):
                heights += tool("get_terrain", {"points": points[i:i + 4096]})["heights"]
            return heights

        path, dives, deep = flight_path(STOPS, lake_level, river, town_h, ground)
        # Each leg flies over its own water: the lake's, the river's (once crossed over to it), the sea's.
        river_from = min(i for i in range(STOPS) if leg_of(i, STOPS) == "river")
        river_cross = river_from + CROSS * (max(i for i in range(STOPS) if leg_of(i, STOPS) == "river") - river_from)
        for leg_name, body, share in (("lake", "Lake", 0.95), ("river", "River", 0.9), ("coast", "Sea", 0.9)):
            stops = [i for i in range(STOPS) if leg_of(i, STOPS) == leg_name and not (leg_name == "river" and i < river_cross)]
            under = tool("get_water", {"points": [[path[i][0][0], path[i][0][2]] for i in stops]})["at"]
            over = sum(1 for w in under if w and w["name"] == body)
            print(f"water_gate: the {leg_name} leg over the {body.lower()} at {over} of {len(stops)} stops")
            if over < share * len(stops):
                failures.append(f"the {leg_name} leg is over the {body.lower()} at only {over} of {len(stops)} stops")
        tool("set_weather", {"preset": "fair", "wetness": 0, "snow_cover": 0})
        world_folder = os.path.join(folder.name, "valley")
        saved = tool("save_world", {"folder": world_folder})
        print(f"water_gate: saved {saved['cells']} cells")
        tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        tool("open_world", {"folder": world_folder})
        tool("set_render_settings", {**ALL_ON, **WATER_ON})
        tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        world, scatter, streamed = wait_streaming(editor, 6000)
        last, settled = settle(editor, 6000)
        tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        if not (streamed and settled):
            failures.append("loading did not catch up in 6000 frames")
        if last["water"]["triangles"] == 0:
            failures.append("no water drawn")
        if last["water"]["broken_lakes"]:
            failures.append(f"broken lakes: {last['water']['broken_lakes']}")
        # The crates dropped in: a few seconds of the simulation first (splashes, floating).
        tool("simulate", {"on": True})
        tool("step_physics", {"seconds": 4})
        physics = tool("get_physics")
        print(f"water_gate: loaded: {world['loaded']} cells, {scatter['instances']} scatter instances, "
              f"{last['voxels']['bricks']} voxel bricks; {physics['wet']['count']} things wet, "
              f"{physics['wet']['in_water']} in the water")
        if physics["wet"]["in_water"] < CRATES - MOST_ASTRAY:
            failures.append(f"only {physics['wet']['in_water']} of {CRATES} crates are in the water")
        overflow = []
        legs = {name: {"under": 0} for name in ("lake", "river", "coast")}
        astray = []  # stops under the water out of the dive
        lake_end, river_end = leg_ends(STOPS)

        def each_stop(stop, frame):
            leg = legs[leg_of(stop, STOPS)]
            water = frame["water"]
            leg["under"] += 1 if water["underwater"] and stop in deep else 0
            if water["underwater"] and stop not in dives:
                astray.append(stop)
            if frame["overflow"] or frame["voxels"]["overflow"] or frame["decal_overflow"] or \
                    water["overflow"] or water["dropped"]:
                overflow.append(stop)

        flown = editor.fly(path, each_stop, marks=(lake_end, river_end))
        times = frame_stats(flown)
        slow = slow_frames(flown, editor.slow, editor.typicals)
        if slow:
            print(f"water_gate: {slow}")
        print(f"water_gate: frame time over the flight ({times['frames']} frames sampled): {describe(times)}")
        cuts = [0, editor.marked[lake_end], editor.marked[river_end], len(flown)]
        leg_times = {}
        for i, name in enumerate(("lake", "river", "coast")):
            leg_times[name] = frame_stats(flown[cuts[i]:cuts[i + 1]])
            print(f"water_gate: flying the {name}: {describe(leg_times[name])}"
                  + (f"; under the surface at {legs[name]['under']} stops" if name == "lake" else ""))
        if not deep or legs["lake"]["under"] < len(deep):
            failures.append(f"the camera was under the lake's surface at only {legs['lake']['under']} of the "
                            f"dive's {len(deep)} stops a metre or more under it")
        if astray:
            failures.append(f"the camera was under the water out of the dive at stops {astray[:5]}")
        world = tool("get_world")
        if world["failed"] or world["far_ground"]["failed"]:
            failures.append(f"streaming failed: {world['failed']} cells, {world['far_ground']['failed']} far chunks")
        if overflow:
            failures.append(f"a list or pool overflowed or water was dropped at stops {overflow[:5]}")
        physics = tool("get_physics")
        if physics["wet"]["count"] == 0:
            failures.append("nothing went in the water wet")
        # The simulation stopped (the crates back where they were put, still: the voxel world settles).
        tool("simulate", {"on": False})
        # The water's cost: over the lake, under it (the dive's deepest), by the river at the town, over the sea.
        under = min(deep or dives, key=lambda i: path[i][0][1])
        town_stop = min(range(lake_end + (river_end - lake_end) // 5, river_end), key=lambda i: abs(path[i][0][2] - TOWN[1]))
        for name, at in (("over the lake", path[lake_end // 5]), ("under the lake", path[under]),
                         ("by the river at the town", path[town_stop]), ("over the sea", path[(river_end + STOPS) // 2])):
            # (On the null device nothing is timed: one round checks it catches up and toggles.)
            cost, parts, caught_up, toggled = water_cost(editor, at, rounds=3 if real_gpu else 1)
            if not caught_up:
                failures.append(f"streaming, the scatter or the voxel world did not catch up {name}")
            if not toggled:
                failures.append(f"the water did not go off and on as asked {name}")
            if cost is None:
                print(f"water_gate: the water {name}: not timed")
                if real_gpu:
                    failures.append(f"the GPU did not time the shading pass {name}")
                continue
            shown = ", ".join(f"{k} {v:.2f}" for k, v in parts.items())
            print(f"water_gate: the water {name}: {cost:.2f} ms ({shown})")
            if real_gpu and cost > WATER_BUDGET_MS:
                failures.append(f"the water takes {cost:.2f} ms {name}, over {WATER_BUDGET_MS} ms")
        gpu = tool("get_engine_info").get("gpu", {})
        if gpu.get("passes"):
            passes = ", ".join(f"{p['name']} {p['ms']:.2f}" for p in gpu["passes"])
            print(f"water_gate: GPU time at the end {gpu['frame_ms']:.2f} ms ({passes} ms)")
        if real_gpu:
            failures += speed_failures(times)
            failures += [f"{name} leg: {f}" for name, t in leg_times.items() for f in speed_failures(t)]
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
            print(f"water_gate: warning: could not remove {folder.name}: {error}")
    for failure in failures:
        print(f"water_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("water_gate: PASS" if real_gpu else "water_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
