#!/usr/bin/env python3
"""The N2 gate: the N1 town and forest in a 40 km landscape, flown through
from a wet street up to 1.5 km over it, looking out 20 km.

    python3 tools/n2_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames). Besides the N1 gate's town and 10,000 trees
it has wet streets and a still lake (glossy: reflections), hills out to
19 km (big meshes) and villages and woods 4 to 18 km off (small: the
voxel far field stands in for them). With every N2 feature on (voxel
world, far field, sun shadows, global illumination, reflections) it
builds the voxel world around the start, then flies down the street, out
over the lake and up to 1.5 km, turning to look round the horizon (about
25 seconds at 60 fps). It passes when:
- the flight holds 60 fps without micro stutter (as the N1 gate: average
  under 16.7 ms, 99th percentile under 20.8 ms, at most one hitch), with
  the voxel world, its light and shadows rebuilt as the camera moves;
- global illumination and reflections take under 4 ms of GPU time (the
  "gi" pass plus what shading takes over shading without them, at the
  street and up high);
- the voxel world reaches 20 km (it holds a village 18 km off) and the
  far field stands in for the far villages; nothing overflowed;
- every stop takes the same draws.
The image stays as sharp moving as standing still by construction: no
pass reads an earlier frame (AGENTS.md rule 2).

On CI (the null device) it checks all but the speed: tools are tested,
not timings.
"""
import math
import random
import sys

from gate_common import Editor, describe, frame_stats, speed_failures
from n1_gate import HOUSES_Z, SPACING, STREET_X, build_gate_scene

VIEW_M = 20_000.0      # the view the gate asks for
LIGHT_BUDGET_MS = 4.0  # global illumination and reflections, GPU
STOPS = 750
HILLS = 40
VILLAGES = 24
VILLAGE_SIZE = 60      # instances each: houses and trees
LAKE = (260.0, -360.0)  # centre (x, z) of the lake, beside the forest
CLIMB_TO = 1500.0       # metres


def build_landscape(tool):
    """The N1 scene plus the N2 additions. Returns (materials, far instances (kit pieces),
    a point inside a house of the village at the view's far end)."""
    materials = build_gate_scene(tool)
    # The land around the N1 scene's 2 km ground, a little under it.
    tool("create_entity", {"name": "Land", "shape": "plane", "position": [0, -0.03, 0],
                           "scale": [40_000, 1, 40_000], "base_color": [0.28, 0.3, 0.2], "roughness": 0.95})
    # Wet streets between the house columns, and a still lake.
    half_x = 20 * SPACING / 2
    streets = [{"name": f"Street {i}", "shape": "cube",
                "position": [i * SPACING - half_x + STREET_X, 0.01, -HOUSES_Z * SPACING / 2 - 10],
                "scale": [3.2, 0.02, HOUSES_Z * SPACING + 16],
                "base_color": [0.06, 0.06, 0.07], "roughness": 0.08} for i in range(20)]
    tool("create_entities", {"entities": streets})
    tool("create_entity", {"name": "Lake", "shape": "plane", "position": [LAKE[0], 0.03, LAKE[1]],
                           "scale": [240, 1, 180], "base_color": [0.04, 0.06, 0.07], "roughness": 0.03})
    rng = random.Random(2)
    # Hills: big meshes, out to the edge of the view.
    hills = []
    for i in range(HILLS):
        angle = rng.uniform(0.0, 2.0 * math.pi)
        distance = rng.uniform(4_000.0, VIEW_M - 1_000.0)
        width = rng.uniform(1_200.0, 3_000.0)
        hills.append({"name": f"Hill {i}", "shape": "sphere",
                      "position": [round(math.cos(angle) * distance), 0, round(math.sin(angle) * distance)],
                      "scale": [round(width), round(width * rng.uniform(0.15, 0.3)), round(width * 0.8)],
                      "base_color": [0.3, 0.33, 0.22], "roughness": 0.9})
    tool("create_entities", {"entities": hills})
    # Villages and woods: small, so the voxel far field shows them.
    far = []
    probe = None  # a house wall in the village at the view's far end
    for v in range(VILLAGES):
        angle = 2.0 * math.pi * v / VILLAGES + rng.uniform(-0.1, 0.1)
        distance = rng.uniform(4_000.0, 18_000.0) if v else 18_000.0  # one at the view's far end
        cx, cz = math.cos(angle) * distance, math.sin(angle) * distance
        for _ in range(VILLAGE_SIZE):
            kit = "house" if rng.random() < 0.4 else "tree"
            x, z = round(cx + rng.uniform(-150, 150), 1), round(cz + rng.uniform(-150, 150), 1)
            far.append({"kit": kit, "position": [x, 0, z], "rotation": {"yaw": round(rng.uniform(-180, 180), 1)}})
            if v == 0 and kit == "house" and probe is None:
                probe = [x, 1.5, z]
    tool("create_entities", {"entities": far})
    pieces = sum(3 if f["kit"] == "house" else 2 for f in far)  # instances drawn: a kit's pieces
    return materials + 4, pieces, probe  # land, wet street, lake, hills


def flight_path(steps):
    """Down a wet street, out over the lake, then up to 1.5 km turning round the horizon."""
    path = []
    for i in range(steps):
        t = i / max(steps - 1, 1)
        if t < 0.3:  # down the street at head height
            u = t / 0.3
            position = [STREET_X, 1.8, 5 - u * 120]
            look_at = [STREET_X, 1.4, position[2] - 30]
        elif t < 0.55:  # low over the lake
            u = (t - 0.3) / 0.25
            position = [STREET_X + u * (LAKE[0] - 150), 1.8 + u * 30, -115 + u * (LAKE[1] + 200)]
            look_at = [LAKE[0], 0.0, LAKE[1] - 60]
        else:  # climb, turning to look out across the land
            u = (t - 0.55) / 0.45
            height = 31.8 + u * (CLIMB_TO - 31.8)
            heading = u * 2.0 * math.pi
            position = [LAKE[0], height, LAKE[1] + 200]
            look_at = [position[0] + math.sin(heading) * VIEW_M, 0.0, position[2] - math.cos(heading) * VIEW_M]
        path.append(([round(c, 3) for c in position], [round(c, 3) for c in look_at]))
    return path


ALL_ON = {"voxel_world": True, "voxel_far_field": True, "shadows": True, "gi": True,
          "reflections": True, "screen_space_reflections": True, "gpu_culling": True,
          "occlusion_culling": True, "antialiasing": True, "specular_antialiasing": True}
LIGHT_OFF = {"gi": False, "reflections": False, "screen_space_reflections": False}


def light_cost(editor, at):
    """GPU ms of global illumination and reflections at a camera stop."""
    editor.tool("set_camera", {"position": at[0], "look_at": at[1]})
    editor.tool("set_render_settings", ALL_ON)
    editor.hover(30)
    on = editor.gpu_passes(30)
    editor.tool("set_render_settings", LIGHT_OFF)
    editor.hover(10)
    off = editor.gpu_passes(30)
    editor.tool("set_render_settings", ALL_ON)
    editor.hover(30)
    return on.get("gi", 0.0) + on.get("shading", 0.0) - off.get("shading", 0.0), on, off


def settle(editor, frames):
    """Up to `frames` frames, until the voxel world, its light and the shadows have caught up."""
    last = editor.last_frame()
    for _ in range(frames // 10):
        if last["voxels"]["pending"] == 0 and last["gi"]["waiting"] == 0 and last["shadows"]["waiting"] == 0:
            break
        editor.hover(10)
        last = editor.last_frame()
    return last


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    failures = []
    real_gpu = False
    editor = None
    try:
        editor = Editor(sys.argv[1], sys.argv[2:], "n2_gate", timeout=900)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"n2_gate: {info['adapter']} ({info['backend']})")
        real_gpu = editor.real_gpu()
        if not real_gpu:
            print("n2_gate: warning: not a real GPU, the timings mean nothing")
        materials, far_count, far_village = build_landscape(tool)
        tool("set_render_settings", ALL_ON)
        path = flight_path(STOPS)
        # Loading: the voxel world, its light and the shadows around the
        # start, given more time a frame than the flight has (a loading
        # screen would); then the flight's own budgets.
        tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        last = settle(editor, 6000)
        tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        print(f"n2_gate: loaded: {last['voxels']['bricks']} voxel bricks, {last['shadows']['pages']} shadow pages")
        overflow = []
        bad_draws = []
        reflections_off = []

        def each_stop(stop, frame):
            if frame["overflow"] or frame["voxels"]["overflow"] or frame["decal_overflow"]:
                overflow.append(stop)
            # GPU culling with occlusion: two raster passes and shading, then
            # screen reflections and SMAA's three.
            if frame["draws"] != 3 or frame["post_draws"] != 4:
                bad_draws.append((stop, frame["draws"], frame["post_draws"]))
            if not (frame["gi"]["on"] and frame["reflections"]["voxel"] and frame["reflections"]["screen"]):
                reflections_off.append(stop)

        times = frame_stats(editor.fly(path, each_stop))
        print(f"n2_gate: frame time over the flight ({times['frames']} frames sampled): {describe(times)}")
        # Up high at the end: the far field and the 20 km view.
        last = settle(editor, 3000)
        print(f"n2_gate: at {CLIMB_TO:.0f} m: {last['far_field']['culled']} of {far_count} far instances left "
              f"to the voxel field, {last['voxels']['bricks']} voxel bricks ({last['voxels']['missing']} missing)")
        levels = tool("probe_voxels", {"position": far_village})["levels"]
        reach = next((l["state"] for l in levels if l["state"] != "outside"), "outside")
        if reach != "solid":
            failures.append(f"the voxel world does not hold the village {VIEW_M / 1000:.0f} km off ({reach})")
        if not last["far_field"]["on"] or last["far_field"]["culled"] < far_count // 2:
            failures.append(f"the far field stands in for too few far instances: {last['far_field']}")
        if last["voxels"]["missing"] > 0:
            failures.append(f"the voxel pool is full: {last['voxels']['missing']} bricks missing")
        if overflow:
            failures.append(f"a list or pool overflowed at stops {overflow[:5]}")
        if bad_draws:
            failures.append(f"the draw count changed during the flight (stop, draws, post): {bad_draws[:5]}")
        if reflections_off:
            failures.append(f"GI or reflections were off at stops {reflections_off[:5]}")
        # Global illumination and reflections, at the street and up high.
        for name, at in (("street", path[STOPS // 10]), ("high", path[-1])):
            cost, on, off = light_cost(editor, at)
            print(f"n2_gate: GI and reflections at the {name}: {cost:.2f} ms "
                  f"(gi {on.get('gi', 0.0):.2f}, shading {on.get('shading', 0.0):.2f} with, "
                  f"{off.get('shading', 0.0):.2f} without)")
            if real_gpu and cost > LIGHT_BUDGET_MS:
                failures.append(f"GI and reflections take {cost:.2f} ms at the {name}, over {LIGHT_BUDGET_MS} ms")
        gpu = tool("get_engine_info").get("gpu", {})
        if gpu.get("passes"):
            passes = ", ".join(f"{p['name']} {p['ms']:.2f}" for p in gpu["passes"])
            print(f"n2_gate: GPU time at the end {gpu['frame_ms']:.2f} ms ({passes} ms)")
        frame = editor.last_frame()
        if frame["materials"] != materials:
            failures.append(f"expected {materials} shared materials, got {frame['materials']}")
        if real_gpu:
            failures += speed_failures(times)
        editor.close()
    except Exception as error:
        failures.append(str(error))
        if editor is not None:
            editor.kill()
    for failure in failures:
        print(f"n2_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("n2_gate: PASS" if real_gpu else "n2_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
