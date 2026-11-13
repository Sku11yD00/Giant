#!/usr/bin/env python3
"""The night gate (lights part L5): the N3 gate's valley at night, its
towns lit by over a thousand lamps, flown through from the riverbank,
down the town's lit street and up over the valley.

    python3 tools/night_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames). It builds the N3 gate's edited valley (the
river, the ridges, the town, the hamlet, the forest), then lights it:
every house has a lit window front and back, the town's lanes have street
lights (spots shining down, casting shadows), the street's swinging
lanterns are lamps (every fourth casting shadows, moving as they swing),
and a bigger town down the valley, 1.2 km past the first, has a lit
house on every plot and a street light at every crossing; about 1250
lamps in all. It saves that as a world, opens it again at night (the
clock at half past ten, the moon and stars out, fair weather, the ground
damp so the lamps shine in it) with every N2 and N3 feature on, and
flies the N3 gate's flight (about 25 seconds at 60 fps) at 2560x1440
where the screen allows. It passes when:
- the flight holds 60 fps without micro stutter (as the other gates:
  average under 16.7 ms, 99th percentile under 20.8 ms, at most one
  hitch), over the whole flight and over each leg on its own;
- the lamps were there: over 1000 lamps in view at once some time in the
  flight, 64 shadowed at once in the town, lamps lighting the voxel
  world (its bounce and reflections, the far town glowing), and the
  lamp lists never overflowed (far lamps are merged a cell at a time,
  src/lights.h: the far town's thousand lamps would otherwise fill its
  few clusters past their 256);
- the lamps take under 2 ms of GPU time (their shadows' pass, "lamp
  shadows", plus what shading and the voxel world's light ("gi") take
  over themselves with the lamps off) by the river, in the street and up
  high where the most lamps were in view, each caught up first, each part
  printed;
- it is night (the sun under the horizon) and nothing overflowed and
  every stop takes the same draws.

On CI (the null device) it checks all but the speed: tools are tested,
not timings.
"""
import os
import sys
import tempfile

from gate_common import Editor, describe, frame_stats, slow_frames, speed_failures
from n2_gate import ALL_ON, behind_text, set_resolution, settle
from n3_gate import (HOUSES_X, HOUSES_Z, SPACING, STOPS, TOWN, build_valley, flight_path, leg_ends, leg_of,
                     wait_streaming)

RESOLUTION = (2560, 1440)
# Half past ten at night at the spring equinox, 50 degrees north; the clock stands still.
NIGHT = {"on": True, "hours": 22.5, "day": 80, "latitude": 50, "speed": 0}
FAR_TOWN = (250.0, -1500.0)  # the bigger town's centre (x, z), down the valley
FAR_SIDE = 18                # houses a side
FAR_SPACING = 14.0
LAMP_BUDGET_MS = 2.0         # the lamps' GPU time (shadows, shading, the voxel world's light)
MIN_IN_VIEW = 1000           # lamps in view at once, some time in the flight
SHADOWED = 64                # lamps shaded with shadows at once, in the town (the renderer's most)
LAMPS_OFF = {"local_lights": False}
LAMPS_ON = {"local_lights": True, "local_shadows": True}


def lantern_lamp(i):
    """The street's lanterns: every fourth casts shadows (six faces each)."""
    return {"on": True, "intensity": 2.0, "range": 8.0, "color": [1.0, 0.7, 0.4], "source_radius": 0.08,
            "shadows": i % 4 == 0, "shadow_near": 0.25}


def window(name, z, sign):
    """A lit window pane on a house's front (sign 1) or back (-1), its lamp
    a little out from the wall (the offset is in the pane's own, thin, space)."""
    return {"name": name, "shape": "cube", "position": [1.2 * sign, 1.6, z], "scale": [0.8, 0.8, 0.04],
            "base_color": [0.1, 0.1, 0.1], "emissive": [1.6, 1.1, 0.6],
            "lamp": {"on": True, "intensity": 1.0, "range": 6.0, "color": [1.0, 0.75, 0.45],
                     "offset": [0, 0, 5.0 * sign], "source_radius": 0.1}}


def light_the_valley(tool, town_h):
    """The lamps over the N3 valley. Returns how many lamps there are."""
    # Every house lit (the town's, the hamlet's, the far town's): the kit replaced.
    house = tool("get_kit", {"name": "house"})["pieces"]
    tool("create_kit", {"name": "house", "replace": True,
                        "pieces": house + [window("window front", 2.53, 1.0), window("window back", -2.53, -1.0)]})
    tool("create_kit", {"name": "street light", "pieces": [
        {"name": "pole", "shape": "cube", "position": [0, 2.5, 0], "scale": [0.15, 5, 0.15],
         "base_color": [0.2, 0.2, 0.22], "metallic": 1.0, "roughness": 0.6},
        {"name": "head", "shape": "cube", "position": [0, 5.05, 0], "scale": [0.6, 0.12, 0.3],
         "base_color": [0.2, 0.2, 0.22], "emissive": [3.0, 2.6, 2.0],
         "lamp": {"on": True, "kind": "spot", "intensity": 12.0, "range": 16.0, "color": [1.0, 0.85, 0.65],
                  "offset": [0, -1.0, 0], "inner_degrees": 35, "outer_degrees": 60, "source_radius": 0.1,
                  "shadows": True, "shadow_near": 0.3}},
    ]})
    # The town's lanes: a street light at each crossing (not in the main
    # street, where the lanterns hang and the flight goes).
    town = [{"kit": "street light",
             "position": [TOWN[0] + (x - HOUSES_X / 2) * SPACING, town_h, TOWN[1] + (z - HOUSES_Z / 2) * SPACING]}
            for x in range(HOUSES_X + 1) if x != HOUSES_X // 2 for z in range(HOUSES_Z + 1)]
    tool("create_entities", {"entities": town})
    # The far town: flattened, a house on every plot, a street light at every crossing.
    centre_h = tool("get_terrain", {"points": [list(FAR_TOWN)]})["heights"][0]
    half = FAR_SIDE * FAR_SPACING / 2
    tool("sculpt_terrain", {"op": "flatten", "points": [list(FAR_TOWN)], "radius": half * 1.6, "strength": 1,
                            "height": centre_h, "falloff": 0.3})
    plots = [[FAR_TOWN[0] + (x - (FAR_SIDE - 1) / 2) * FAR_SPACING, FAR_TOWN[1] + (z - (FAR_SIDE - 1) / 2) * FAR_SPACING]
             for x in range(FAR_SIDE) for z in range(FAR_SIDE)]
    crossings = [[FAR_TOWN[0] + (x - FAR_SIDE / 2) * FAR_SPACING, FAR_TOWN[1] + (z - FAR_SIDE / 2) * FAR_SPACING]
                 for x in range(FAR_SIDE + 1) for z in range(FAR_SIDE + 1)]
    heights = tool("get_terrain", {"points": plots + crossings})["heights"]
    far = [{"kit": "house" if i < len(plots) else "street light", "position": [round(x, 2), round(h, 2), round(z, 2)]}
           for i, ((x, z), h) in enumerate(zip(plots + crossings, heights))]
    tool("create_entities", {"entities": far})
    houses = HOUSES_X * HOUSES_Z + 12 + len(plots)  # the town, the hamlet, the far town
    return 2 * houses + len(town) + len(crossings) + 64  # windows, street lights, lanterns


def lamp_cost(editor, at, rounds=3):
    """GPU ms of the lamps at a stop: the "lamp shadows" pass plus what
    "shading" and "gi" (the voxel world's light, lamps lit into it) take
    over themselves with the lamps off, the median over `rounds`, on and
    off in turn. The stop is caught up first. Returns (ms or None when the
    GPU did not time those passes, its parts, whether it caught up, whether
    the lamps went on and off as asked)."""
    editor.tool("set_camera", {"position": at[0], "look_at": at[1]})
    editor.tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
    streamed = wait_streaming(editor, 6000)[2]
    last, settled = settle(editor, 6000)
    caught_up = settled and streamed
    if not caught_up:  # what was behind, for the report
        print(f"night_gate: behind: streaming {'caught up' if streamed else 'not caught up'}, {behind_text(last)}")
    editor.tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
    costs = []
    toggled = True
    for _ in range(rounds):
        editor.hover(10)
        on = editor.gpu_passes(20)
        toggled = toggled and editor.last_frame()["lamps_in_view"] > 0
        editor.tool("set_render_settings", LAMPS_OFF)
        editor.hover(10)
        off = editor.gpu_passes(20)
        toggled = toggled and editor.last_frame()["lamps_in_view"] == 0
        editor.tool("set_render_settings", LAMPS_ON)
        if "shading" not in on or "shading" not in off:
            return None, None, caught_up, toggled
        parts = {"shadows": on.get("lamp shadows", 0.0), "shading": on["shading"] - off["shading"],
                 "voxel light": on.get("gi", 0.0) - off.get("gi", 0.0)}
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
        editor = Editor(sys.argv[1], sys.argv[2:], "night_gate", timeout=1800)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"night_gate: {info['adapter']} ({info['backend']})")
        real_gpu = editor.real_gpu()
        if not real_gpu:
            print("night_gate: warning: not a real GPU, the timings mean nothing")
        width, height = set_resolution(editor, *RESOLUTION)
        print(f"night_gate: {width}x{height} pixels")
        if real_gpu and (width, height) != RESOLUTION:
            print(f"night_gate: warning: the budgets are for {RESOLUTION[0]}x{RESOLUTION[1]}")
        town_h = build_valley(tool, lantern_lamp)
        lamps = light_the_valley(tool, town_h)

        def ground(points):
            heights = []
            for i in range(0, len(points), 4096):
                heights += tool("get_terrain", {"points": points[i:i + 4096]})["heights"]
            return heights

        path = flight_path(STOPS, ground)
        # Night, fair weather, the ground damp; saved with the world.
        tool("set_time", NIGHT)
        tool("set_weather", {"preset": "fair", "wetness": 0.5, "snow_cover": 0})
        world_folder = os.path.join(folder.name, "valley")
        saved = tool("save_world", {"folder": world_folder})
        print(f"night_gate: saved {saved['cells']} cells with {lamps} lamps; town at {town_h:.1f} m")
        tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        tool("open_world", {"folder": world_folder})
        tool("set_render_settings", {**ALL_ON, **LAMPS_ON})
        time = tool("get_time")
        print(f"night_gate: the sun at {time['sun']['elevation_degrees']:.1f} degrees, "
              f"the light: {time['light']}, stars {time['stars']:.2f}")
        if not time["sun"]["elevation_degrees"] < -6.0:
            failures.append(f"it is not night: the sun at {time['sun']['elevation_degrees']} degrees")
        tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        world, scatter, streamed = wait_streaming(editor, 6000)
        last, settled = settle(editor, 6000)
        tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        if not (streamed and settled):
            failures.append("loading did not catch up in 6000 frames")
        print(f"night_gate: loaded: {world['loaded']} cells, {scatter['instances']} scatter instances, "
              f"{last['voxels']['bricks']} voxel bricks, {last['lamps_in_view']} lamps in view")
        overflow = []
        bad_draws = []
        draws = None
        most = {"in_view": 0, "shadowed": 0, "voxel": 0, "waiting": 0, "lights": 0, "merged": 0, "drawn": 0,
                "culls": 0}
        widest = [0, STOPS - 1]  # the climb's stop with the most lamps in view (lamps, stop)
        legs = {name: {"in_view": 0, "shadowed": 0} for name in ("low", "street", "climb")}
        low_end, street_end = leg_ends(STOPS)

        def each_stop(stop, frame):
            nonlocal draws
            leg = legs[leg_of(stop, STOPS)]
            shadows = frame["lamp_shadows"]
            in_view = frame["lamps_in_view"]
            leg["in_view"] = max(leg["in_view"], in_view)
            leg["shadowed"] = max(leg["shadowed"], shadows["lights"])
            if leg_of(stop, STOPS) == "climb" and in_view > widest[0]:
                widest[:] = [in_view, stop]
            most["in_view"] = max(most["in_view"], in_view)
            most["lights"] = max(most["lights"], frame["lights"])
            most["merged"] = max(most["merged"], frame["lights_merged"])
            most["shadowed"] = max(most["shadowed"], shadows["lights"])
            most["voxel"] = max(most["voxel"], frame["gi"]["lamps"])
            most["waiting"] = max(most["waiting"], shadows["waiting"])
            most["drawn"] = max(most["drawn"], shadows["drawn"])
            most["culls"] = max(most["culls"], shadows["culls"])
            if frame["overflow"] or frame["voxels"]["overflow"] or frame["decal_overflow"] or frame["light_overflow"]:
                overflow.append(stop)
            got = (frame["draws"], frame["post_draws"])
            if draws is None:
                draws = got
            elif got != draws:
                bad_draws.append((stop, *got))

        flown = editor.fly(path, each_stop, marks=(low_end, street_end))
        times = frame_stats(flown)
        slow = slow_frames(flown, editor.slow, editor.typicals)
        if slow:
            print(f"night_gate: {slow}")
        print(f"night_gate: frame time over the flight ({times['frames']} frames sampled): {describe(times)}")
        cuts = [0, editor.marked[low_end], editor.marked[street_end], len(flown)]
        leg_times = {}
        for i, name in enumerate(("low", "street", "climb")):
            leg_times[name] = frame_stats(flown[cuts[i]:cuts[i + 1]])
            print(f"night_gate: flying {name}: {describe(leg_times[name])}; at most {legs[name]['in_view']} lamps "
                  f"in view, {legs[name]['shadowed']} shadowed")
        print(f"night_gate: over the flight at most {most['in_view']} lamps in view (shaded as at most "
              f"{most['lights']} lights, at most {most['merged']} of them far lamps merged), {most['shadowed']} shadowed, "
              f"{most['voxel']} lighting the voxel world's bricks lit in a frame, "
              f"{most['waiting']} shadow faces waiting, at most {most['drawn']} drawn in a frame "
              f"with {most['culls']} cullings (one a lamp)")
        if most["in_view"] < MIN_IN_VIEW:
            failures.append(f"at most {most['in_view']} lamps in view at once, not {MIN_IN_VIEW}")
        if legs["street"]["shadowed"] < SHADOWED:
            failures.append(f"at most {legs['street']['shadowed']} lamps shadowed at once in the street, "
                            f"not {SHADOWED}")
        if most["voxel"] == 0:
            failures.append("no lamps lit the voxel world")
        if overflow:
            failures.append(f"a list or pool overflowed at stops {overflow[:5]}")
        if bad_draws:
            failures.append(f"the draw count changed during the flight (stop, draws, post): {bad_draws[:5]}")
        # The lamps' cost: low by the river, in the street, up high looking out over the most.
        for name, at in (("river", path[low_end // 2]), ("street", path[(low_end + street_end) // 2]),
                         ("heights", path[widest[1]])):
            cost, parts, caught_up, toggled = lamp_cost(editor, at)
            if not caught_up:
                failures.append(f"streaming, the scatter or the voxel world did not catch up by the {name}")
            if not toggled:
                failures.append(f"the lamps did not go on and off as asked by the {name}")
            if cost is None:
                print(f"night_gate: the lamps by the {name}: not timed")
                if real_gpu:
                    failures.append(f"the GPU did not time the shading pass by the {name}")
                continue
            shown = ", ".join(f"{k} {v:.2f}" for k, v in parts.items())
            print(f"night_gate: the lamps by the {name}: {cost:.2f} ms ({shown})")
            if real_gpu and cost > LAMP_BUDGET_MS:
                failures.append(f"the lamps take {cost:.2f} ms by the {name}, over {LAMP_BUDGET_MS} ms")
        gpu = tool("get_engine_info").get("gpu", {})
        if gpu.get("passes"):
            passes = ", ".join(f"{p['name']} {p['ms']:.2f}" for p in gpu["passes"])
            print(f"night_gate: GPU time at the end {gpu['frame_ms']:.2f} ms ({passes} ms)")
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
            print(f"night_gate: warning: could not remove {folder.name}: {error}")
    for failure in failures:
        print(f"night_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("night_gate: PASS" if real_gpu else "night_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
