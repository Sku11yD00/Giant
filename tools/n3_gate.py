#!/usr/bin/env python3
"""The N3 gate: a storm rolling over an edited valley, flown through from a
riverbank up into the clouds, streamed from disk.

    python3 tools/n3_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames). It builds a 4 km valley of rolling hills,
then edits it with the terrain brushes (a river cut down its middle,
ridges raised either side, a flat site for a town), puts a kit-built town
on the site and strews a forest on the slopes and stones by the river by
scatter rules. It saves all that as a world and opens it again, so the
flight streams cells (entities, ground tiles, the far ground) from disk
and the scatter is placed as the cells load. With every N2 feature on and
fair weather, it flies low up the river, through the town at head height,
then climbs 1 km into the cloud deck turning round the valley (about 25
seconds at 60 fps) at 2560x1440 where the screen allows, while a storm
blends in over 20 seconds: clouds thicken, rain falls, the ground wets.
It passes when:
- the flight holds 60 fps without micro stutter (as the N1 and N2 gates:
  average under 16.7 ms, 99th percentile under 20.8 ms, at most one
  hitch), with cells, ground and scatter streaming in and the weather
  changing as it flies;
- the weather (sky, clouds, their shadows, the cover map; and what rain
  and wet ground add to shading) takes under 3 ms of GPU time in the full
  storm, low in the valley and up in the clouds;
- streaming never failed, every cell near the end has loaded, the scatter
  has caught up with it and no cell was cut short;
- the storm arrived (the view shows it, rain falls, the sky drew clouds,
  their shadows, rain, wet surfaces and the cover map) and the ground has
  begun to wet;
- nothing overflowed and every stop takes the same draws.
How far behind streaming and scatter fall while flying is printed for each
leg. No pass reads an earlier frame (AGENTS.md rule 2), so the image stays
as sharp moving as standing still.

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
CLIMB_TO = 1000.0        # metres over the town: up into the storm's deck (base 700 m)
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
    town = [{"kit": "house", "position": [round(TOWN[0] + (x - (HOUSES_X - 1) / 2) * SPACING - SPACING / 2, 2),
                                          town_h,
                                          round(TOWN[1] + (z - (HOUSES_Z - 1) / 2) * SPACING, 2)]}
            for x in range(HOUSES_X) for z in range(HOUSES_Z)]
    tool("create_entities", {"entities": town})
    # A street down the town's middle (x = TOWN[0]), wet in the rain.
    tool("create_entity", {"name": "Street", "shape": "cube", "position": [TOWN[0], town_h + 0.01, TOWN[1]],
                           "scale": [4, 0.02, HOUSES_Z * SPACING + 20], "base_color": [0.08, 0.08, 0.09],
                           "roughness": 0.5})
    # The forest on the slopes, above the valley floor; stones by the river.
    tool("add_scatter_layer", {"name": "forest", "items": [{"kit": "tree", "scale": [0.8, 1.3]}],
                               "spacing": 9, "density": 0.7, "clump_size": 60, "clumping": 0.6,
                               "height_min": town_h + 4, "slope_max": 35, "seed": 7})
    tool("add_scatter_layer", {"name": "stones", "items": [{"shape": "sphere", "scale": [0.4, 1.2], "sink": 0.3,
                                                            "base_color": [0.45, 0.44, 0.42], "roughness": 0.7}],
                               "spacing": 12, "density": 0.5, "height_max": town_h + 2, "slope_max": 20, "seed": 8})
    return town_h


def flight_path(steps, ground):
    """Up the river low, through the town, then up into the clouds turning
    round. `ground(points)` gives the ground's height at [x, z] points."""
    plan = []
    street_z0 = TOWN[1] + HOUSES_Z * SPACING / 2 + 15
    for i in range(steps):
        t = i / max(steps - 1, 1)
        if t < 0.35:  # low up the river towards the town, 6 m over the bank
            u = t / 0.35
            z = 1400.0 + u * (street_z0 - 1400.0)
            x = RIVER_X + 40.0 * math.sin(z / 300.0) * (1.0 - u) + TOWN[0] * u
            plan.append(("low", x, z, 6.0, None))
        elif t < 0.5:  # down the town's street at head height
            u = (t - 0.35) / 0.15
            z = street_z0 - u * (HOUSES_Z * SPACING + 30)
            plan.append(("street", TOWN[0], z, 1.8, None))
        else:  # climb over the town into the storm, turning round the valley
            u = (t - 0.5) / 0.5
            z = street_z0 - (HOUSES_Z * SPACING + 30)
            plan.append(("climb", TOWN[0], z, 1.8 + u * CLIMB_TO, u * 2.0 * math.pi))
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


def weather_cost(editor, at, rounds=3):
    """GPU ms of the weather at a stop: the "sky" and "clouds" passes plus
    what shading takes over shading with the weather's parts off (the sky
    itself stays on, so the cost of rain, wet ground and cover in shading
    shows), the median over `rounds`, on and off in turn. None when the GPU
    did not time those passes."""
    editor.tool("set_camera", {"position": at[0], "look_at": at[1]})
    settle(editor, 3000)
    costs = []
    for _ in range(rounds):
        editor.hover(10)
        on = editor.gpu_passes(20)
        editor.tool("set_render_settings", WEATHER_OFF)
        editor.hover(10)
        off = editor.gpu_passes(20)
        editor.tool("set_render_settings", WEATHER_ON)
        if "sky" not in on or "shading" not in on or "shading" not in off:
            return None
        costs.append(on["sky"] + on.get("clouds", 0.0) + on["shading"] - off["shading"])
    return sorted(costs)[len(costs) // 2]


def wait_streaming(editor, frames):
    """Up to `frames` frames until the world's cells and the scatter have
    caught up. Returns (get_world, get_scatter, whether they caught up)."""
    world = scatter = None
    for _ in range(max(frames // 10, 1)):
        world = editor.tool("get_world")
        scatter = editor.tool("get_scatter")
        if world["reading"] == 0 and world["waiting"] == 0 and scatter["building"] == 0 \
                and world["far_ground"]["reading"] == 0:
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
        legs = {name: {"reading": 0, "building": 0} for name in ("low", "street", "climb")}
        storm = {"started": False}

        def each_stop(stop, frame):
            nonlocal draws
            if stop == 0:
                # The storm rolls in over the flight (a frame: the flight times it too).
                tool("set_weather", {"preset": "storm", "over_seconds": STORM_SECONDS})
                storm["started"] = True
            t = stop / (STOPS - 1)
            leg = legs["low" if t < 0.35 else "street" if t < 0.5 else "climb"]
            if stop % 25 == 0:  # (a command each: a frame, timed with the flight)
                w = tool("get_world")
                s = tool("get_scatter")
                leg["reading"] = max(leg["reading"], w["reading"] + w["waiting"])
                leg["building"] = max(leg["building"], s["building"])
            if frame["overflow"] or frame["voxels"]["overflow"] or frame["decal_overflow"]:
                overflow.append(stop)
            got = (frame["draws"], frame["post_draws"])
            if draws is None:
                draws = got
            elif got != draws:
                bad_draws.append((stop, *got))

        times = frame_stats(editor.fly(path, each_stop))
        print(f"n3_gate: frame time over the flight ({times['frames']} frames sampled): {describe(times)}")
        for name, leg in legs.items():
            print(f"n3_gate: flying {name}: at most {leg['reading']} cells reading or waiting, "
                  f"{leg['building']} scatter cells building")
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
        if world["loaded"] == 0 or world["tiles_loaded"] == 0:
            failures.append(f"no cells or tiles loaded at the end: {world}")
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
        for name, at in (("river", path[STOPS // 6]), ("street", path[int(0.42 * (STOPS - 1))]),
                         ("clouds", path[-1])):
            cost = weather_cost(editor, at)
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
        # Close without saving: the world's files are the gate's, thrown away.
        tool("close_world")
        editor.close()
    except Exception as error:
        failures.append(str(error))
        if editor is not None:
            editor.kill()
    finally:
        folder.cleanup()
    for failure in failures:
        print(f"n3_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("n3_gate: PASS" if real_gpu else "n3_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
