#!/usr/bin/env python3
"""The effects gate (effects plan FX5): the water gate's valley in a storm,
its town smoking and burning, a boathouse collapsing into the river and
shots fired down the street, flown through.

    python3 tools/effects_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames). It builds the N3 gate's edited valley with the
water gate's lake, river and sea (and its crates), then gives it effects:
smoke from a chimney on every house in the town, fires burning along the
street and down by the river (each glowing, flickering), and a breakable
boathouse on the riverbank by the town. It saves that as a world and opens
it again in a storm (rain splashing on what it falls on, a gale blowing
leaves and grit along), simulating, with every N2 and N3 feature on, and
flies the N3 gate's flight (up the river, down the street at head height,
spiralling up and out) at 2560x1440 where the screen allows. On the way
the boathouse is knocked down into the river (its pieces' dust and
splashes) and shots are fired into the houses from the street (their
chips, dust and sparks). It passes when:
- the flight holds 60 fps without micro stutter (as the other gates:
  average under 16.7 ms, 99th percentile under 20.8 ms, at most one
  hitch), over the whole flight and over each leg on its own;
- the effects were there: effects running (giving birth) and the fires'
  lights given, rain splashing, leaves blowing (grit only off dry ground:
  none in the rain), the shots' hits,
  the collapse's dust and splashes all set effects off, and particles were
  alive and drawn;
- the particles take under 1.0 ms of GPU time (the "particles" pass: their
  lighting, drawing and putting them over the scene) in the street, with
  the most of them about, caught up first;
- nothing overflowed and every stop takes the same draws.

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
from water_gate import put_water, river_x

RESOLUTION = (2560, 1440)
PARTICLES_BUDGET_MS = 1.0
FIRES_STREET = 8           # along the street's sides
FIRES_RIVER = 4            # on the riverbank by the town
SHOT_EVERY = 12            # stops between shots down the street
BOATHOUSE_AT = 0.3         # the street leg's share gone when the boathouse is knocked down
EFFECTS_ON = {"particles": True, "local_lights": True}


def house_spots(town_h):
    return [(round(TOWN[0] + (x - (HOUSES_X - 1) / 2) * SPACING, 2), town_h,
             round(TOWN[1] + (z - (HOUSES_Z - 1) / 2) * SPACING, 2))
            for x in range(HOUSES_X) for z in range(HOUSES_Z)]


def boathouse_spot(tool):
    """On the riverbank by the town, between the river and the houses."""
    z = TOWN[1] + 20.0
    x = river_x(z) + 11.0
    h = tool("get_terrain", {"points": [[x, z]]})["heights"][0]
    return x, h, z


def add_effects(tool, town_h):
    """Chimney smoke, fires, the boathouse. Returns (chimneys, fires)."""
    chimneys = [{"name": f"Chimney {i}", "shape": "cube", "position": [x + 1.2, y + 4.0, z - 1.5],
                 "scale": [0.5, 1.2, 0.5], "base_color": [0.35, 0.3, 0.28], "roughness": 0.9,
                 "effect": {"on": True, "effect": "smoke"}}
                for i, (x, y, z) in enumerate(house_spots(town_h))]
    tool("create_entities", {"entities": chimneys})
    street_half = HOUSES_Z * SPACING / 2
    fires = [{"name": f"Street fire {i}", "shape": "cube",
              "position": [TOWN[0] + (1.7 if i % 2 else -1.7), town_h + 0.15,
                           round(TOWN[1] - street_half + (i + 0.5) * (2 * street_half / FIRES_STREET), 2)],
              "scale": [0.8, 0.3, 0.8], "base_color": [0.1, 0.08, 0.07], "roughness": 0.95,
              "effect": {"on": True, "effect": "fire"}}
             for i in range(FIRES_STREET)]
    bank = []
    for i in range(FIRES_RIVER):
        z = TOWN[1] - 30.0 + i * 20.0
        bank.append([river_x(z) + 9.0, z])
    heights = tool("get_terrain", {"points": bank})["heights"]
    fires += [{"name": f"Bank fire {i}", "shape": "cube", "position": [round(x, 2), round(h + 0.15, 2), z],
               "scale": [0.8, 0.3, 0.8], "base_color": [0.1, 0.08, 0.07], "roughness": 0.95,
               "effect": {"on": True, "effect": "fire"}}
              for i, ((x, z), h) in enumerate(zip(bank, heights))]
    tool("create_entities", {"entities": fires})
    # The boathouse: wooden walls and a roof that come apart, leaning out over the water.
    pieces = []
    for level in range(3):
        for side, (px, pz, sx, sz) in enumerate(((0, 2.4, 6, 0.3), (0, -2.4, 6, 0.3), (2.9, 0, 0.3, 4.5),
                                                  (-2.9, 0, 0.3, 4.5))):
            pieces.append({"name": f"wall {level} {side}", "shape": "cube", "position": [px, 0.6 + 1.2 * level, pz],
                           "scale": [sx, 1.2, sz], "base_color": [0.42, 0.3, 0.18], "roughness": 0.85,
                           "surface": "wood"})
    pieces.append({"name": "roof", "shape": "cube", "position": [0, 3.8, 0], "scale": [6.6, 0.4, 5.4],
                   "base_color": [0.3, 0.2, 0.12], "roughness": 0.9, "surface": "wood"})
    tool("create_kit", {"name": "boathouse", "pieces": pieces})
    tool("set_kit_breakable", {"name": "boathouse", "breakable": True, "strength": 200})
    x, h, z = boathouse_spot(tool)
    tool("create_entity", {"name": "Boathouse", "kit": "boathouse", "position": [round(x, 2), round(h, 2), z]})
    return len(chimneys), len(fires)


def particles_cost(editor, at, rounds=3):
    """GPU ms of the "particles" pass at a stop, the median over `rounds`.
    The stop is caught up first. Returns (ms or None when the GPU did not
    time it, whether it caught up, the most particles alive meanwhile)."""
    editor.tool("set_camera", {"position": at[0], "look_at": at[1]})
    editor.tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
    streamed = wait_streaming(editor, 6000)[2]
    last, settled = settle(editor, 6000)
    caught_up = settled and streamed
    if not caught_up:  # what was behind, for the report
        print(f"effects_gate: behind: streaming {'caught up' if streamed else 'not caught up'}, {behind_text(last)}")
    editor.tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
    costs = []
    alive = 0
    for _ in range(rounds):
        editor.hover(10)
        passes = editor.gpu_passes(20)
        alive = max(alive, editor.last_frame()["particles"]["alive"])
        if "particles" not in passes:
            return None, caught_up, alive
        costs.append(passes["particles"])
    return sorted(costs)[len(costs) // 2], caught_up, alive


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    failures = []
    real_gpu = False
    editor = None
    folder = tempfile.TemporaryDirectory()
    try:
        editor = Editor(sys.argv[1], sys.argv[2:], "effects_gate", timeout=1800)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"effects_gate: {info['adapter']} ({info['backend']})")
        real_gpu = editor.real_gpu()
        if not real_gpu:
            print("effects_gate: warning: not a real GPU, the timings mean nothing")
        width, height = set_resolution(editor, *RESOLUTION)
        print(f"effects_gate: {width}x{height} pixels")
        if real_gpu and (width, height) != RESOLUTION:
            print(f"effects_gate: warning: the budgets are for {RESOLUTION[0]}x{RESOLUTION[1]}")
        town_h = build_valley(tool)
        put_water(tool)
        chimneys, fires = add_effects(tool, town_h)

        def ground(points):
            heights = []
            for i in range(0, len(points), 4096):
                heights += tool("get_terrain", {"points": points[i:i + 4096]})["heights"]
            return heights

        path = flight_path(STOPS, ground)
        tool("set_weather", {"preset": "storm"})
        world_folder = os.path.join(folder.name, "valley")
        saved = tool("save_world", {"folder": world_folder})
        print(f"effects_gate: saved {saved['cells']} cells with {chimneys} chimneys and {fires} fires; "
              f"town at {town_h:.1f} m")
        tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        tool("open_world", {"folder": world_folder})
        tool("set_render_settings", {**ALL_ON, **EFFECTS_ON})
        tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        world, scatter, streamed = wait_streaming(editor, 6000)
        last, settled = settle(editor, 6000)
        tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        if not (streamed and settled):
            failures.append("loading did not catch up in 6000 frames")
        print(f"effects_gate: loaded: {world['loaded']} cells, {scatter['instances']} scatter instances")
        tool("simulate", {"on": True})  # (the crates fall into the water: splashes)
        boathouse = boathouse_spot(tool)
        low_end, street_end = leg_ends(STOPS)
        knock_at = low_end + int((street_end - low_end) * BOATHOUSE_AT)
        overflow = []
        bad_draws = []
        draws = None
        most = {"alive": 0, "drawn": False, "born": 0, "running": 0, "lights": 0, "splashes": 0, "leaves": 0,
                "grit": 0, "weather_cut": 0, "cut": 0}
        knocked = {"loosened": 0}
        shots = [0]

        def each_stop(stop, frame):
            nonlocal draws
            particles = frame["particles"]
            most["alive"] = max(most["alive"], particles["alive"])
            most["drawn"] = most["drawn"] or particles["drawn"]
            most["born"] = max(most["born"], particles["born"])
            if stop % 10 == 0:  # (a read of the effects every few stops: room in the frame times kept)
                emitting = tool("get_effects")["emitting"]
                most["running"] = max(most["running"], emitting["running"])
                most["lights"] = max(most["lights"], emitting["lights"])
                weather = emitting["weather"]
                most["splashes"] = max(most["splashes"], weather.get("splashes", 0))
                most["leaves"] = max(most["leaves"], weather.get("leaves", 0))
                most["grit"] = max(most["grit"], weather.get("grit", 0))
                most["weather_cut"] += weather["cut"]
                most["cut"] += emitting["cut"]
            if stop == knock_at:  # the boathouse knocked into the river
                hit = tool("impact", {"point": [boathouse[0] + 3.0, boathouse[1] + 1.5, boathouse[2]],
                                      "radius": 8.0, "impulse": 5000.0})
                knocked["loosened"] = hit["loosened"]
            if leg_of(stop, STOPS) == "street" and stop % SHOT_EVERY == 0:  # into the houses either side
                at = frame_camera[stop]
                side = 1.0 if shots[0] % 2 else -1.0
                tool("fire", {"origin": at, "direction": [side, -0.15, -0.3]})
                shots[0] += 1
            if frame["overflow"] or frame["voxels"]["overflow"] or frame["decal_overflow"] or frame["light_overflow"]:
                overflow.append(stop)
            # (Less the particles' two passes, which run while anything is alive.)
            got = (frame["draws"], frame["post_draws"] - (2 if frame["particles"]["drawn"] else 0))
            if draws is None:
                draws = got
            elif got != draws:
                bad_draws.append((stop, *got))

        frame_camera = [p[0] for p in path]
        flown = editor.fly(path, each_stop, marks=(low_end, street_end))
        times = frame_stats(flown)
        slow = slow_frames(flown, editor.slow, editor.typicals, editor.slow_over_ms)
        if slow:
            print(f"effects_gate: {slow}")
        print(f"effects_gate: frame time over the flight ({times['frames']} frames sampled): {describe(times)}")
        cuts = [0, editor.marked[low_end], editor.marked[street_end], len(flown)]
        leg_times = {}
        for i, name in enumerate(("low", "street", "climb")):
            leg_times[name] = frame_stats(flown[cuts[i]:cuts[i + 1]])
            print(f"effects_gate: flying {name}: {describe(leg_times[name])}")
        effects = tool("get_effects")["emitting"]
        events = effects["events"]
        print(f"effects_gate: at most {most['alive']} particles alive ({'drawn' if most['drawn'] else 'never drawn'}), "
              f"{most['born']} born in a frame; at most {most['running']} effects giving birth in a frame, {most['lights']} effect lights; the weather at most "
              f"{most['splashes']} splashes, {most['leaves']} leaves, {most['grit']} grit a frame "
              f"({most['weather_cut']} particles cut); effects cut {most['cut']}")
        print(f"effects_gate: events: {events['hits']} hits, {events['splashes']} splashes, {events['dust']} dust, "
              f"{events['refused']} refused; the knock loosened {knocked['loosened']} boathouse pieces; {shots[0]} shots fired")
        # (Running counts those that gave birth that frame: smoke, a few a second, skips frames.)
        if most["running"] == 0:
            failures.append("no effect ever gave birth")
        if most["lights"] == 0:
            failures.append("no fire gave light")
        if most["splashes"] == 0:
            failures.append("the rain never splashed")
        # (Grit blows only off dry ground: the storm's rain has wet it, so none is expected here.)
        if most["leaves"] == 0:
            failures.append(f"the storm blew {most['leaves']} leaves at most a frame")
        if events["hits"] == 0:
            failures.append("no shot's hit set an effect off")
        if events["splashes"] == 0:
            failures.append("nothing going into the water splashed")
        if events["dust"] == 0 or knocked["loosened"] == 0:
            failures.append(f"the boathouse's collapse ({knocked['loosened']} pieces) threw up no dust")
        if most["alive"] == 0 or not most["drawn"]:
            failures.append(f"particles: at most {most['alive']} alive, {'drawn' if most['drawn'] else 'never drawn'}")
        if effects["missing"]:
            failures.append(f"{effects['missing']} effects or looks named nothing")
        if overflow:
            failures.append(f"a list or pool overflowed at stops {overflow[:5]}")
        if bad_draws:
            failures.append(f"the draw count changed during the flight (stop, draws, post): {bad_draws[:5]}")
        # The particles' cost in the street, smoke, fires, rain and shots about.
        cost, caught_up, alive = particles_cost(editor, path[(low_end + street_end) // 2])
        if not caught_up:
            failures.append("streaming, the scatter or the voxel world did not catch up in the street")
        if cost is None:
            print("effects_gate: the particles in the street: not timed")
            if real_gpu:
                failures.append("the GPU did not time the particles pass in the street")
        else:
            print(f"effects_gate: the particles in the street: {cost:.2f} ms ({alive} particles alive)")
            if real_gpu and cost > PARTICLES_BUDGET_MS:
                failures.append(f"the particles take {cost:.2f} ms in the street, over {PARTICLES_BUDGET_MS} ms")
        gpu = tool("get_engine_info").get("gpu", {})
        if gpu.get("passes"):
            passes = ", ".join(f"{p['name']} {p['ms']:.2f}" for p in gpu["passes"])
            print(f"effects_gate: GPU time at the end {gpu['frame_ms']:.2f} ms ({passes} ms)")
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
            print(f"effects_gate: warning: could not remove {folder.name}: {error}")
    for failure in failures:
        print(f"effects_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("effects_gate: PASS" if real_gpu else "effects_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
