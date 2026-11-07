#!/usr/bin/env python3
"""The N3 gate: a storm rolling over an edited valley, flown through from a
riverbank up into the clouds, streamed from disk.

    python3 tools/n3_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames). It builds a 4 km valley of rolling hills,
then edits it with the terrain brushes (a river cut down its middle,
ridges raised either side, a flat site for a town), puts a kit-built town
on the site and a hamlet up the valley, and strews a forest on the slopes
and stones by the river by scatter rules. It saves all that as a world and
opens it again, so the flight streams cells (entities, ground tiles, the
far ground) from disk and the scatter is placed as the cells load. With
every N2 feature on and fair weather, it flies up the river over the trees
(about 75 m/s), down the town's street at head height between the houses,
then spirals 900 m out and 1 km up into the cloud deck looking out over
the valley (about 25 seconds at 60 fps), at 2560x1440 where the screen
allows, while a storm blends in over 20 seconds: clouds thicken, rain
falls, the ground wets. It passes when:
- the flight holds 60 fps without micro stutter (as the N1 and N2 gates:
  average under 16.7 ms, 99th percentile under 20.8 ms, at most one
  hitch), over the whole flight and over each leg on its own, with cells,
  ground and scatter streaming in and out and the weather changing;
- the flight streamed: ground tiles loaded, and tiles and the hamlet's
  entities unloaded behind it; streaming never failed; the scatter caught
  up with every cell loaded some time in the street, and no cell was cut
  short;
- the weather (the sky passes: atmosphere, clouds, their shadows, the
  cover map; and what rain, wet ground and cover add to shading) takes
  under 3 ms of GPU time in the full storm, by the river, in the street
  and up in the clouds, each caught up first;
- the storm arrived (the view shows it, rain falls, the sky drew clouds,
  their shadows, rain, wet surfaces and the cover map) and the ground has
  begun to wet;
- nothing overflowed and every stop takes the same draws.
How far behind streaming, scatter and the voxel world fall is printed for
each leg. No pass reads an earlier frame (AGENTS.md rule 2), so the image
stays as sharp moving as standing still.

On CI (the null device) it checks all but the speed: tools are tested,
not timings.
"""
import math
import os
import sys
import tempfile

from gate_common import Editor, describe, frame_stats, speed_failures
from n2_gate import ALL_ON, set_resolution, settle

RESOLUTION = (2560, 1440)
STOPS = 750
HALF = 2048.0            # the valley: 64 x 64 cells of 64 m round the origin
SAMPLES = 32             # a tile's samples a side: 2 m apart
RIVER_X = 0.0            # the river runs along z down the valley's middle
RIDGE_X = 700.0          # ridges raised either side
TOWN = (0.0, -300.0)     # the flattened site's centre (x, z)
TOWN_RADIUS = 110.0
HOUSES_X, HOUSES_Z, SPACING = 8, 6, 10.0
HAMLET = (200.0, 1300.0)  # houses up the valley, left behind (unloaded) as the flight goes on
START_Z = 600.0          # the low leg starts here, up the river
LOW_OVER = 12.0          # metres over the ground on the low leg (over the trees)
CLIMB_TO = 1000.0        # metres over the town: up into the storm's deck (base 700 m)
CLIMB_OUT = 900.0        # metres the climb spirals out from the town
LOW_END, STREET_END = 0.45, 0.6  # the legs' ends, as fractions of the flight
STORM_SECONDS = 20.0     # how long the storm takes to blend in
WEATHER_BUDGET_MS = 3.0  # GPU: the weather in the full storm
WEATHER_OFF = {"clouds": False, "cloud_shadows": False, "precipitation": False, "wet_surfaces": False,
               "weather_cover": False}
WEATHER_ON = {k: True for k in WEATHER_OFF}


def build_valley(tool):
    """The edited valley: terrain, brush strokes, the town and the scatter
    layers. Returns the town's ground height."""
    tool("new_scene")
    made = tool("create_terrain", {"from": [-HALF, -HALF], "to": [HALF - 1, HALF - 1], "samples": SAMPLES,
                                   "hills": {"base": 20, "amplitude": 25, "size": 500, "octaves": 4, "seed": 3}})
    assert made["tiles"] == 4096, f"expected 4096 tiles, got {made}"
    tool("set_terrain_material", {"base_color": [0.3, 0.32, 0.2], "roughness": 0.9})
    edge = HALF - 64
    # The river: cut down the middle, then smoothed into a channel.
    river = [[RIVER_X + 40.0 * math.sin(z / 300.0), z] for z in range(int(edge), -int(edge), -100)]
    tool("sculpt_terrain", {"op": "lower", "points": river, "radius": 160, "strength": 3, "falloff": 0.8})
    tool("sculpt_terrain", {"op": "smooth", "points": river, "radius": 120, "strength": 0.6, "falloff": 0.5})
    # Ridges either side.
    for side in (-1.0, 1.0):
        ridge = [[side * RIDGE_X + 120.0 * math.sin(z / 450.0 + side), z]
                 for z in range(-int(edge), int(edge), 100)]
        tool("sculpt_terrain", {"op": "raise", "points": ridge, "radius": 320, "strength": 6, "falloff": 0.9})
    # The town's site: flattened at the height of the riverbank there.
    town_h = tool("get_terrain", {"points": [[TOWN[0] + 60, TOWN[1]]]})["heights"][0] + 1.0
    tool("sculpt_terrain", {"op": "flatten", "points": [[TOWN[0], TOWN[1]]], "radius": TOWN_RADIUS,
                            "strength": 1, "height": town_h, "falloff": 0.3})
    # The town: kit houses on the site (the N1 gate's kit).
    tool("create_kit", {"name": "house", "pieces": [
        {"name": "walls", "shape": "cube", "position": [0, 1.5, 0], "scale": [4, 3, 5],
         "base_color": [0.8, 0.75, 0.6]},
        {"name": "roof", "shape": "cube", "position": [0, 3.3, 0], "scale": [4.4, 0.6, 5.4],
         "base_color": [0.5, 0.15, 0.1]},
        {"name": "door", "shape": "cube", "position": [0, 1, 2.51], "scale": [1, 2, 0.05],
         "base_color": [0.3, 0.2, 0.1]},
    ]})
    tool("create_kit", {"name": "tree", "pieces": [
        {"name": "trunk", "shape": "cube", "position": [0, 1.5, 0], "scale": [0.35, 3, 0.35],
         "base_color": [0.35, 0.25, 0.15], "roughness": 0.9},
        {"name": "canopy", "shape": "sphere", "position": [0, 4.2, 0], "scale": [3, 3.4, 3],
         "base_color": [0.15, 0.4, 0.12], "roughness": 0.8},
    ]})
    town = [{"kit": "house", "position": [round(TOWN[0] + (x - (HOUSES_X - 1) / 2) * SPACING, 2),
                                          town_h,
                                          round(TOWN[1] + (z - (HOUSES_Z - 1) / 2) * SPACING, 2)]}
            for x in range(HOUSES_X) for z in range(HOUSES_Z)]
    tool("create_entities", {"entities": town})
    # A hamlet up the valley: loaded at the start, unloaded as the flight leaves it.
    spots = [[HAMLET[0] + (i % 4) * 14.0, HAMLET[1] + (i // 4) * 16.0] for i in range(12)]
    heights = tool("get_terrain", {"points": spots})["heights"]
    tool("create_entities", {"entities": [{"kit": "house", "position": [x, round(h, 2), z]}
                                          for (x, z), h in zip(spots, heights)]})
    # A street down the town's middle (x = TOWN[0], between two columns of houses), wet in the rain.
    tool("create_entity", {"name": "Street", "shape": "cube", "position": [TOWN[0], town_h + 0.01, TOWN[1]],
                           "scale": [4, 0.02, HOUSES_Z * SPACING + 20], "base_color": [0.08, 0.08, 0.09],
                           "roughness": 0.5})
    # The forest on the slopes, above the valley floor; stones by the river (below the town).
    tool("add_scatter_layer", {"name": "forest", "items": [{"kit": "tree", "scale": [0.8, 1.3]}],
                               "spacing": 9, "density": 0.7, "clump_size": 60, "clumping": 0.6,
                               "height_min": town_h + 4, "slope_max": 35, "seed": 7})
    tool("add_scatter_layer", {"name": "stones", "items": [{"shape": "sphere", "scale": [0.4, 1.2], "sink": 0.3,
                                                            "base_color": [0.45, 0.44, 0.42], "roughness": 0.7}],
                               "spacing": 12, "density": 0.5, "height_max": town_h - 1, "slope_max": 20, "seed": 8})
    return town_h


def leg_of(stop, steps):
    t = stop / max(steps - 1, 1)
    return "low" if t < LOW_END else "street" if t < STREET_END else "climb"


def leg_ends(steps):
    """The last stop of the low and street legs."""
    ends = []
    for leg in ("low", "street"):
        ends.append(max(i for i in range(steps) if leg_of(i, steps) == leg))
    return ends


def flight_path(steps, ground):
    """Up the river low, down the town's street, then spiralling out and up
    into the clouds. `ground(points)` gives the ground's height at [x, z]."""
    plan = []
    street_z0 = TOWN[1] + HOUSES_Z * SPACING / 2 + 15
    street_z1 = street_z0 - (HOUSES_Z * SPACING + 30)
    for i in range(steps):
        t = i / max(steps - 1, 1)
        leg = leg_of(i, steps)
        if leg == "low":  # up the river towards the town, over the trees
            u = t / LOW_END
            z = START_Z + u * (street_z0 - START_Z)
            x = RIVER_X + 40.0 * math.sin(z / 300.0) * (1.0 - u) + TOWN[0] * u
            plan.append((leg, x, z, LOW_OVER, None))
        elif leg == "street":  # down the town's street at head height
            u = (t - LOW_END) / (STREET_END - LOW_END)
            # Down from over the trees to head height before the first houses.
            over = LOW_OVER + (1.8 - LOW_OVER) * min(1.0, u / 0.15)
            plan.append((leg, TOWN[0], street_z0 + u * (street_z1 - street_z0), over, None))
        else:  # spiralling out over the valley and up into the storm, looking out
            u = (t - STREET_END) / (1.0 - STREET_END)
            heading = u * 2.0 * math.pi  # starts looking along -z, as the street leg ends
            r = CLIMB_OUT * u
            plan.append((leg, TOWN[0] + math.sin(heading) * r, street_z1 - math.cos(heading) * r,
                         1.8 + u * CLIMB_TO, heading))
    heights = ground([[p[1], p[2]] for p in plan])
    path = []
    for i, (leg, x, z, over, heading) in enumerate(plan):
        # Over the highest ground a few stops either side (no dipping into a bank).
        near = [h for h in heights[max(0, i - 3):i + 4] if h is not None]
        y = (max(near) if near else 0.0) + over
        if leg == "climb":
            look_at = [x + math.sin(heading) * 3000.0, y * 0.3, z - math.cos(heading) * 3000.0]
        else:
            look_at = [x, y - 1.0, z - 60.0]
        path.append(([round(x, 3), round(y, 3), round(z, 3)], [round(c, 3) for c in look_at]))
    return path


SKY_PARTS = ("clouds", "cloud_shadows", "falling", "wet_surfaces", "weather_cover")


def weather_cost(editor, at, rounds=3):
    """GPU ms of the weather at a stop: the "sky" and "clouds" passes (the
    atmosphere, clouds, their shadows, the cover map) plus what shading
    takes over shading with the weather's parts off (rain falling, wet
    ground, the cover; fog stays on, so its few instructions are not
    counted), the median over `rounds`, on and off in turn. The stop is
    caught up first (streaming, scatter, the voxel world, as the flight had
    them). Returns (ms or None when the GPU did not time those passes,
    whether it caught up, whether the parts went on and off as asked)."""
    editor.tool("set_camera", {"position": at[0], "look_at": at[1]})
    editor.tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
    streamed = wait_streaming(editor, 6000)[2]
    caught_up = settle(editor, 6000)[1] and streamed
    editor.tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
    costs = []
    toggled = True
    for _ in range(rounds):
        editor.hover(10)
        on = editor.gpu_passes(20)
        toggled = toggled and all(editor.last_frame()["sky"][k] for k in SKY_PARTS)
        editor.tool("set_render_settings", WEATHER_OFF)
        editor.hover(10)
        off = editor.gpu_passes(20)
        toggled = toggled and not any(editor.last_frame()["sky"][k] for k in SKY_PARTS)
        editor.tool("set_render_settings", WEATHER_ON)
        if "sky" not in on or "shading" not in on or "shading" not in off:
            return None, caught_up, toggled
        costs.append(on["sky"] + on.get("clouds", 0.0) + on["shading"] - off["shading"])
    return sorted(costs)[len(costs) // 2], caught_up, toggled


def wait_streaming(editor, frames):
    """Up to `frames` frames until the world's cells, the ground's meshes
    (tiles and far ground) and the scatter have caught up. Returns (get_world, get_scatter, whether they caught up)."""
    world = scatter = None
    for _ in range(max(frames // 10, 1)):
        world = editor.tool("get_world")
        scatter = editor.tool("get_scatter")
        terrain = editor.tool("get_terrain")["meshes"]
        far = world["far_ground"]
        if world["reading"] == 0 and world["waiting"] == 0 and scatter["building"] == 0 \
                and far["reading"] == 0 and terrain["building"] == 0 \
                and all(level["building"] == 0 for level in far["levels"]):
            return world, scatter, True
        editor.hover(10)
    return world, scatter, False


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    failures = []
    real_gpu = False
    editor = None
    folder = tempfile.TemporaryDirectory()
    try:
        editor = Editor(sys.argv[1], sys.argv[2:], "n3_gate", timeout=1500)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"n3_gate: {info['adapter']} ({info['backend']})")
        real_gpu = editor.real_gpu()
        if not real_gpu:
            print("n3_gate: warning: not a real GPU, the timings mean nothing")
        width, height = set_resolution(editor, *RESOLUTION)
        print(f"n3_gate: {width}x{height} pixels")
        if real_gpu and (width, height) != RESOLUTION:
            print(f"n3_gate: warning: the budgets are for {RESOLUTION[0]}x{RESOLUTION[1]}")
        town_h = build_valley(tool)

        def ground(points):
            heights = []
            for i in range(0, len(points), 4096):
                heights += tool("get_terrain", {"points": points[i:i + 4096]})["heights"]
            return heights

        path = flight_path(STOPS, ground)
        # Fair weather, the ground dry; saved with the world.
        tool("set_weather", {"preset": "fair", "wetness": 0, "snow_cover": 0})
        world_folder = os.path.join(folder.name, "valley")
        saved = tool("save_world", {"folder": world_folder})
        print(f"n3_gate: saved {saved['cells']} cells, {saved.get('chunks_written', 0)} far ground chunks; "
              f"town at {town_h:.1f} m")
        # Opened again: from here the flight streams it from disk.
        tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        tool("open_world", {"folder": world_folder})
        tool("set_render_settings", {**ALL_ON, **WEATHER_ON})
        # Loading: given more time a frame than the flight has (a loading
        # screen would), then the flight's own budgets.
        tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        world, scatter, streamed = wait_streaming(editor, 6000)
        last, settled = settle(editor, 6000)
        tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        if not (streamed and settled):
            failures.append("loading did not catch up in 6000 frames")
        print(f"n3_gate: loaded: {world['loaded']} cells, {world['tiles_loaded']} tiles, "
              f"{scatter['instances']} scatter instances, {last['voxels']['bricks']} voxel bricks")
        overflow = []
        bad_draws = []
        draws = None
        legs = {name: {"reading": 0, "building": 0, "pending": 0} for name in ("low", "street", "climb")}
        low_end, street_end = leg_ends(STOPS)
        before = tool("get_world")
        street_building = []  # scatter cells to place, through the street leg

        def each_stop(stop, frame):
            nonlocal draws
            if stop == 0:
                # The storm rolls in over the flight (a frame: the flight times it too).
                tool("set_weather", {"preset": "storm", "over_seconds": STORM_SECONDS})
            leg = legs[leg_of(stop, STOPS)]
            leg["pending"] = max(leg["pending"], frame["voxels"]["pending"])
            in_street = low_end < stop <= street_end
            if stop % 25 == 0 or (in_street and stop % 10 == 0):  # (a command each: a frame, timed too)
                w = tool("get_world")
                s = tool("get_scatter")
                leg["reading"] = max(leg["reading"], w["reading"] + w["waiting"])
                leg["building"] = max(leg["building"], s["building"])
                if in_street:
                    street_building.append(s["building"])
            if frame["overflow"] or frame["voxels"]["overflow"] or frame["decal_overflow"]:
                overflow.append(stop)
            got = (frame["draws"], frame["post_draws"])
            if draws is None:
                draws = got
            elif got != draws:
                bad_draws.append((stop, *got))

        flown = editor.fly(path, each_stop, marks=(low_end, street_end))
        after = tool("get_world")
        times = frame_stats(flown)
        print(f"n3_gate: frame time over the flight ({times['frames']} frames sampled): {describe(times)}")
        # Each leg on its own too: the low leg streams the most, and a whole
        # flight's 99th percentile could hide its slow frames.
        cuts = [0, editor.marked[low_end], editor.marked[street_end], len(flown)]
        leg_times = {}
        for i, name in enumerate(("low", "street", "climb")):
            leg = legs[name]
            leg_times[name] = frame_stats(flown[cuts[i]:cuts[i + 1]])
            print(f"n3_gate: flying {name}: {describe(leg_times[name])}; at most {leg['reading']} cells "
                  f"reading or waiting, {leg['building']} scatter cells building, {leg['pending']} voxel work")
        # The flight streamed from disk: ground loaded, and what it left behind went.
        streamed_in = after["tiles_loaded"] - before["tiles_loaded"]
        tiles_out = after["tiles_unloaded"] - before["tiles_unloaded"]
        entities_out = after["entities_unloaded"] - before["entities_unloaded"]
        print(f"n3_gate: over the flight: {streamed_in} tiles loaded, {tiles_out} tiles and "
              f"{entities_out} entities unloaded")
        if streamed_in <= 0 or tiles_out <= 0 or entities_out <= 0:
            failures.append(f"the flight did not stream: {streamed_in} tiles loaded, {tiles_out} tiles and "
                            f"{entities_out} entities unloaded")
        # The scatter keeps up: some time in the street (slow, after the low
        # leg's run) it has placed every cell loaded.
        if not street_building or min(street_building) > 0:
            failures.append(f"the scatter fell behind: never caught up in the street leg "
                            f"(cells to place: {street_building})")
        # The storm in full (the rest of its blend, if the flight was quicker).
        weather = tool("get_weather")
        for _ in range(int(STORM_SECONDS * 60 / 10) + 60):
            if weather["progress"] >= 1.0:
                break
            editor.hover(10)
            weather = tool("get_weather")
        world, scatter, streamed = wait_streaming(editor, 3000)
        # What the flight left behind, given the loading budget to catch up
        # (up in the clouds the whole valley is in view).
        behind = editor.last_frame()
        print(f"n3_gate: behind when the flight ended: {behind['voxels']['pending']} voxel work, "
              f"{behind['gi']['waiting']} bricks to light, {behind['shadows']['waiting']} shadow pages")
        tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        last, settled = settle(editor, 6000)
        tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        if not streamed:
            failures.append(f"streaming or scatter did not catch up at the end: {world}, "
                            f"building {scatter['building']}")
        if not settled:
            failures.append(f"the voxel world, its light or the shadows did not catch up at the end: "
                            f"{last['voxels']}, {last['gi']}, {last['shadows']}")
        if world["failed"] or world["far_ground"]["failed"]:
            failures.append(f"streaming failed: {world['failed']} cells, {world['far_ground']['failed']} far chunks")
        if world["loaded"] == 0:
            failures.append(f"no cells loaded at the end: {world}")
        if scatter["points"] == 0 or any(layer["points"] == 0 for layer in scatter["layers"]):
            failures.append(f"a scatter layer placed nothing: {[(x['name'], x['points']) for x in scatter['layers']]}")
        if scatter["cells_cut_short"]:
            failures.append(f"{scatter['cells_cut_short']} scatter cells cut short (the cell cap)")
        layers = ", ".join(f"{layer['name']} {layer['points']}" for layer in scatter["layers"])
        print(f"n3_gate: at the end: {world['loaded']} cells, {world['tiles_loaded']} tiles, "
              f"{scatter['instances']} scatter instances ({layers})")
        sky = last["sky"]
        shown = weather.get("shown") or {}
        print(f"n3_gate: the storm: progress {weather['progress']:.2f}, rain {weather['falling']['rain']:.2f}, "
              f"ground wetness {weather['ground']['wetness']:.3f}, sky {sky}")
        if weather["progress"] < 1.0 or shown.get("cloud_cover") != 1.0:
            failures.append(f"the storm did not arrive: progress {weather['progress']}, shown {shown}")
        if not (weather["falling"]["rain"] > 0.5):
            failures.append(f"no rain falls in the storm: {weather['falling']}")
        if not all(sky[k] for k in ("on", "clouds", "cloud_shadows", "falling", "wet_surfaces", "weather_cover")):
            failures.append(f"the sky did not draw all the storm: {sky}")
        if not weather["ground"]["wetness"] > 0.02:
            failures.append(f"the ground has not begun to wet: {weather['ground']}")
        if overflow:
            failures.append(f"a list or pool overflowed at stops {overflow[:5]}")
        if bad_draws:
            failures.append(f"the draw count changed during the flight (stop, draws, post): {bad_draws[:5]}")
        # The weather's cost in the full storm: low by the river, in the street, up in the clouds.
        for name, at in (("river", path[low_end // 2]), ("street", path[(low_end + street_end) // 2]),
                         ("clouds", path[-1])):
            cost, caught_up, toggled = weather_cost(editor, at)
            if not caught_up:
                failures.append(f"streaming, the scatter or the voxel world did not catch up by the {name}")
            if not toggled:
                failures.append(f"the weather's parts did not go on and off as asked by the {name}")
            if cost is None:
                print(f"n3_gate: the weather by the {name}: not timed")
                if real_gpu:
                    failures.append(f"the GPU did not time the sky and shading passes by the {name}")
                continue
            print(f"n3_gate: the weather by the {name}: {cost:.2f} ms")
            if real_gpu and cost > WEATHER_BUDGET_MS:
                failures.append(f"the weather takes {cost:.2f} ms by the {name}, over {WEATHER_BUDGET_MS} ms")
        gpu = tool("get_engine_info").get("gpu", {})
        if gpu.get("passes"):
            passes = ", ".join(f"{p['name']} {p['ms']:.2f}" for p in gpu["passes"])
            print(f"n3_gate: GPU time at the end {gpu['frame_ms']:.2f} ms ({passes} ms)")
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
            print(f"n3_gate: warning: could not remove {folder.name}: {error}")
    for failure in failures:
        print(f"n3_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("n3_gate: PASS" if real_gpu else "n3_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
