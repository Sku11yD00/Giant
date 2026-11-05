#!/usr/bin/env python3
"""The N1 gate: a kit-built town and 10,000 trees, flown through over MCP.

    python3 tools/n1_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames): it builds the gate scene, flies the
camera down a street of the town and up over the forest (about 20 seconds
at 60 fps: each stop is two commands, a frame each), then reports frame
times over the whole flight. It passes when the flight holds 60 fps
(average frame under 16.7 ms; with vsync on a 60 Hz screen that is every
frame on time), every stop takes the same fixed number of draws, nothing
overflows, and the detail the view asks for has loaded by the end (a group
may wait a frame while flying; none may be left waiting).

tools/mcp_smoke.py runs the same scene on CI, where only the software
renderer exists: there it checks the draw counts and the image, not speed.
"""
import json
import random
import subprocess
import sys
import threading

TREES = 10_000
HOUSES_X, HOUSES_Z = 20, 15
SPACING = 9.0
FRAME_BUDGET_MS = 1000.0 / 60.0


def build_gate_scene(tool):
    """Builds the gate scene. Returns the number of distinct materials in it."""
    tool("new_scene")
    tool("create_entity", {"name": "Ground", "shape": "plane", "scale": [2000, 1, 2000],
                           "base_color": [0.25, 0.3, 0.2], "roughness": 0.95})
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
    half_x = HOUSES_X * SPACING / 2
    town = [{"kit": "house", "position": [x * SPACING - half_x, 0, -z * SPACING - 10]}
            for x in range(HOUSES_X) for z in range(HOUSES_Z)]
    tool("create_entities", {"entities": town})
    # The forest: a jittered grid around the town, the same every run.
    rng = random.Random(1)
    trees = []
    side = 120  # 14,400 cells, of which the town takes about 600
    for cell in range(side * side):
        x = (cell % side - side / 2) * 7.0 + rng.uniform(-2.5, 2.5)
        z = 200 - (cell // side) * 7.0 + rng.uniform(-2.5, 2.5)
        if -half_x - 8 < x < half_x + 8 and -HOUSES_Z * SPACING - 18 < z < 0:
            continue  # leave the town clear
        size = rng.uniform(0.8, 1.3)
        trees.append({"kit": "tree", "position": [round(x, 2), 0, round(z, 2)],
                      "rotation": {"yaw": round(rng.uniform(-180, 180), 1)},
                      "scale": [size, size, size]})
        if len(trees) == TREES:
            break
    assert len(trees) == TREES, "the forest grid is too small for the trees"
    tool("create_entities", {"entities": trees})
    return 1 + 3 + 2  # ground, house, tree


ENTITIES = 1 + HOUSES_X * HOUSES_Z + TREES  # ground, houses, trees
STREET_X = SPACING / 2  # between the house column at x = 0 and the next


def flight_path(steps):
    """Camera stops from a street in the town up over the forest."""
    path = []
    for i in range(steps):
        t = i / max(steps - 1, 1)
        if t < 0.5:  # down the main street at head height
            u = t / 0.5
            position = [STREET_X, 1.8, 5 - u * 120]
            look_at = [STREET_X, 1.6, position[2] - 30]
        else:  # climb and look out over the trees
            u = (t - 0.5) / 0.5
            position = [STREET_X + u * 150, 1.8 + u * 120, -115 - u * 200]
            look_at = [position[0] + 60, 0.0, position[2] - 150]
        path.append((position, look_at))
    return path


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    editor = subprocess.Popen([sys.argv[1], "--mcp", *sys.argv[2:]], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=None, text=True, encoding="utf-8", bufsize=1)
    timer = threading.Timer(300, editor.kill)
    timer.start()
    next_id = 0

    def call(method, params=None):
        nonlocal next_id
        next_id += 1
        message = {"jsonrpc": "2.0", "id": next_id, "method": method}
        if params is not None:
            message["params"] = params
        editor.stdin.write(json.dumps(message) + "\n")
        editor.stdin.flush()
        reply = json.loads(editor.stdout.readline())
        if "error" in reply:
            raise RuntimeError(f"{method} failed: {reply['error']}")
        return reply["result"]

    def tool(name, arguments=None):
        result = call("tools/call", {"name": name, "arguments": arguments or {}})
        if result.get("isError"):
            raise RuntimeError(f"tool {name} failed: {result['content'][0]['text']}")
        return result.get("structuredContent", {})

    failures = []
    real_gpu = False
    try:
        call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                            "clientInfo": {"name": "n1_gate", "version": "1"}})
        info = tool("get_engine_info")
        print(f"n1_gate: {info['adapter']} ({info['backend']})")
        if info["software_adapter"] or info["backend"] == "null":
            print("n1_gate: warning: not a real GPU, the frame times mean nothing")
        materials = build_gate_scene(tool)
        tool("set_render_settings", {"gpu_culling": True, "occlusion_culling": True,
                                     "antialiasing": True, "specular_antialiasing": True})
        waiting = 0
        overflow = False
        bad_draws = []
        path = flight_path(600)
        for position, look_at in path[:30]:  # warm up: streaming fills the pool
            tool("set_camera", {"position": position, "look_at": look_at})
        # The editor keeps the last 240 frame times: read them every 100
        # stops (200 frames), so the whole flight is timed.
        windows = []
        for stop, (position, look_at) in enumerate(path):
            tool("set_camera", {"position": position, "look_at": look_at})
            frame = tool("get_render_settings")["last_frame"]
            waiting = max(waiting, frame["streaming"]["waiting"])
            overflow = overflow or frame["overflow"]
            # GPU culling with occlusion: two raster passes and shading.
            if frame["draws"] != 3 or frame["post_draws"] != 3:
                bad_draws.append((stop, frame["draws"], frame["post_draws"]))
            if stop % 100 == 99:
                windows.append(tool("get_engine_info")["frame_times"])
        times = {"frames": sum(w["frames"] for w in windows),
                 "average_ms": sum(w["average_ms"] for w in windows) / len(windows),
                 "worst_ms": max(w["worst_ms"] for w in windows)}
        for _ in range(30):  # hovering at the end: what the view wants loads
            call("ping")
        left_waiting = tool("get_render_settings")["last_frame"]["streaming"]["waiting"]
        tool("set_render_settings", {"gpu_culling": False})
        for _ in range(3):
            call("ping")
        frame = tool("get_render_settings")["last_frame"]
        print(f"n1_gate: {frame['instances']} instances in {frame['draws']} draws "
              f"(+{frame['post_draws']} full-screen), {frame['materials']} materials, "
              f"{frame['triangles']} triangles in view at the end")
        print(f"n1_gate: frame time over the flight ({times['frames']} frames sampled): "
              f"average {times['average_ms']:.2f} ms, worst {times['worst_ms']:.2f} ms")
        if bad_draws:
            failures.append(f"the draw count changed during the flight (stop, draws, post): {bad_draws[:5]}")
        if frame["draws"] != 2 or frame["post_draws"] != 3:
            failures.append(f"on the CPU path the scene should take 2 draws and 3 full-screen passes, got {frame}")
        if left_waiting > 0:
            failures.append(f"{left_waiting} detail groups still waiting to load after the flight")
        if frame["materials"] != materials:
            failures.append(f"expected {materials} shared materials, got {frame['materials']}")
        if overflow:
            failures.append("a culling list overflowed: some geometry was dropped")
        if waiting > 0:
            # Waiting a frame is normal while flying fast; a backlog is not.
            print(f"n1_gate: at most {waiting} detail groups waited a frame to load")
        real_gpu = not info["software_adapter"] and info["backend"] != "null"
        if real_gpu and times["average_ms"] > FRAME_BUDGET_MS + 0.3:
            failures.append(f"average frame {times['average_ms']:.2f} ms is over the 60 fps budget")
        tool("quit_editor")
        editor.wait(timeout=60)
    except Exception as error:
        failures.append(str(error))
        editor.kill()
    finally:
        timer.cancel()
    for failure in failures:
        print(f"n1_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("n1_gate: PASS" if real_gpu else "n1_gate: PASS (draws and loading only: speed needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
