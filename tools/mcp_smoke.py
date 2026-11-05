#!/usr/bin/env python3
"""Drives the editor over MCP the way Claude Code does, and checks the answers.

    python3 tools/mcp_smoke.py <path to giant_editor> [editor options...]

Starts the editor with --mcp, then: initialize, tools/list, change the
background colour, undo it, read the log, and quit. Then starts it again and
closes the connection, which must close the editor too, and once more as a
client that crashes with requests in flight. Fails (exit 1) on any
wrong answer, or if the editor does not exit cleanly.
"""
import base64
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading

from n1_gate import ENTITIES, STREET_X, build_gate_scene, flight_path

TIMEOUT = 240  # seconds for the whole run (the N1 gate scene is large on the software renderer)


def decode_png(data):
    """Decodes the editor's PNGs (8-bit RGBA, filter 0) to (width, height, bytes)."""
    import zlib
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("not a PNG")
    at, idat, width, height = 8, b"", 0, 0
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if zlib.crc32(kind + body) != struct.unpack(">I", data[at + 8 + length:at + 12 + length])[0]:
            raise RuntimeError(f"bad CRC in {kind}")
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    raw = zlib.decompress(idat)
    row = width * 4
    if len(raw) != (row + 1) * height:
        raise RuntimeError("PNG data has the wrong size")
    pixels = b"".join(raw[y * (row + 1) + 1:(y + 1) * (row + 1)] for y in range(height))
    return width, height, pixels


def build_scene(tool, call, check):
    """N0's gate: build and light a scene through MCP alone, then save, load and undo."""
    tool("new_scene")
    check(tool("list_entities")["count"] == 0, "new scene is empty")
    made = tool("create_entities", {"entities": [
        {"name": "Ground", "shape": "plane", "scale": [200, 1, 200],
         "base_color": [0.3, 0.45, 0.25], "roughness": 0.95},
        {"name": "Tower", "shape": "cube", "position": [0, 2, 0], "scale": [2, 4, 2],
         "base_color": [0.8, 0.3, 0.2]},
        {"name": "Ball", "shape": "sphere", "position": [3, 1, 0], "scale": [2, 2, 2],
         "base_color": [0.9, 0.8, 0.5], "metallic": 1, "roughness": 0.2},
    ]})
    ids = made["ids"]
    check(len(ids) == 3 and len(set(ids)) == 3, "three entities with their own ids")
    listed = tool("list_entities")
    check([e["name"] for e in listed["entities"]] == ["Ground", "Tower", "Ball"], "outliner order")

    lighting = tool("set_lighting", {"sun_elevation_degrees": 35, "sun_azimuth_degrees": 220,
                                     "sun_intensity": 4})
    check(lighting["sun_intensity"] == 4 and lighting["exposure"] == 1, "lighting set, rest kept")
    camera = tool("set_camera", {"position": [8, 5, 10], "look_at": [0, 1.5, 0]})
    check(abs(camera["yaw"] - 38.66) < 0.1 and camera["pitch"] < -10, f"camera aimed, got {camera}")

    tower = tool("set_entity", {"id": ids[1], "base_color": [0.2, 0.3, 0.8],
                                "rotation": {"yaw": 30}})
    check(tower["base_color"] == [0.2, 0.30000001192092896, 0.800000011920929]
          or abs(tower["base_color"][2] - 0.8) < 1e-6, "colour changed")
    check(abs(tower["rotation"]["yaw"] - 30) < 1e-3, "rotated")
    check(tower["position"] == [0, 2, 0], "untouched fields stay")
    tool("undo")
    check(abs(tool("get_entity", {"id": ids[1]})["base_color"][0] - 0.8) < 1e-6, "undo set_entity")
    tool("redo")
    before_camera = tool("get_camera")
    refused = call("tools/call", {"name": "set_camera", "arguments": {
        "position": [1, 1, 1], "look_at": [1, 1, 1], "vertical_fov_degrees": 20}})
    check(refused.get("isError") is True and tool("get_camera") == before_camera,
          "a refused set_camera leaves the camera alone")
    missing = call("tools/call", {"name": "set_entity", "arguments": {"id": 999999}})
    check(missing.get("isError") is True, "unknown id is a tool error")

    # CPU culling gives exact counts on the frame itself (GPU counts arrive
    # late, and the null backend runs no shaders).
    tool("set_render_settings", {"gpu_culling": False})
    for _ in range(3):
        call("ping")  # let a frame render the scene
    info = tool("get_engine_info")
    check(info["last_frame"]["scene_instances"] == 3, f"3 instances drawn, got {info['last_frame']}")
    check(info["last_frame"]["scene_draws"] == 2, "one raster pass and one shading pass")
    gpu_passes = [p["name"] for p in info["gpu"]["passes"]]
    check(all(name in gpu_passes for name in ("raster", "shading", "UI")),
          f"GPU passes timed, got {info['gpu']}")
    check(info["gpu"]["frame_ms"] >= 0 and all(p["ms"] >= 0 for p in info["gpu"]["passes"]),
          "GPU times are not negative")
    tool("set_render_settings", {"gpu_culling": True})
    shot = call("tools/call", {"name": "capture_screenshot",
                               "arguments": {"max_width": 800, "file_name": "mcp_scene"}})
    check(not shot.get("isError"), "scene screenshot taken")
    if info["backend"] != "null":
        image = [c for c in shot["content"] if c["type"] == "image"][0]
        width, height, pixels = decode_png(base64.b64decode(image["data"]))
        x, y = width // 2, height // 2  # the tower, in the middle of the view
        r, g, b, _ = pixels[(y * width + x) * 4:(y * width + x) * 4 + 4]
        check(b > r and (r, g, b) != (108, 157, 219), f"the tower is drawn, got {(r, g, b)}")

    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "town.scene.json")
        saved = tool("save_scene", {"path": path})
        check(saved["entities"] == 3 and os.path.isfile(path), "scene saved")
        tool("delete_entities", {"ids": [ids[0], ids[0]]})  # listed twice is fine
        check(tool("list_entities")["count"] == 2, "deleted")
        tool("undo")
        check(tool("list_entities")["entities"][0]["id"] == ids[0], "undo puts it back in place")
        tool("new_scene")
        check(tool("list_entities")["count"] == 0, "cleared")
        check(tool("load_scene", {"path": path})["entities"] == 3, "scene loaded")
        check(tool("get_lighting")["sun_intensity"] == 4, "lighting loaded")
        tool("undo")
        check(tool("list_entities")["count"] == 0, "undo the load")
        tool("undo")
        check(tool("list_entities")["count"] == 3, "undo the new scene")
        bad_path = os.path.join(folder, "bad.scene.json")
        with open(bad_path, "w", encoding="utf-8") as bad:
            bad.write('{"format": "giant-scene", "version": 1, "entities": [{"id": 1, "shap": 2}]}')
        bad = call("tools/call", {"name": "load_scene", "arguments": {"path": bad_path}})
        check(bad.get("isError") is True and "shap is not a known key" in bad["content"][0]["text"],
              "a broken scene file is refused with the reason")
        check(tool("list_entities")["count"] == 3, "a failed load changes nothing")
    tool("new_scene")  # leave the default empty scene for the checks after this


def write_bumpy_sphere_glb(path, segments=256, rings=128):
    """A 1 m wide sphere with bumps, about 65 000 triangles: dense enough for LOD."""
    import math
    positions, normals, indices = [], [], []
    for r in range(rings + 1):
        theta = math.pi * r / rings
        for s in range(segments + 1):
            phi = 2 * math.pi * s / segments
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), -math.sin(theta) * math.sin(phi))
            bump = 0.5 + 0.02 * math.sin(phi * 12) * math.sin(theta * 9)
            positions.append(tuple(c * bump for c in n))
            normals.append(n)
    row = segments + 1
    for r in range(rings):
        for s in range(segments):
            a = r * row + s
            b, c, d = a + row, a + row + 1, a + 1
            if r != 0:
                indices += [a, b, d]
            if r != rings - 1:
                indices += [d, b, c]
    vertex_bytes = b"".join(struct.pack("<6f", *p, *n) for p, n in zip(positions, normals))
    index_bytes = struct.pack(f"<{len(indices)}I", *indices)
    gltf = {
        "asset": {"version": "2.0"}, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}],
        "buffers": [{"byteLength": len(vertex_bytes) + len(index_bytes)}],
        "bufferViews": [{"buffer": 0, "byteLength": len(vertex_bytes), "byteStride": 24},
                        {"buffer": 0, "byteOffset": len(vertex_bytes), "byteLength": len(index_bytes)}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
             "min": [-0.52, -0.52, -0.52], "max": [0.52, 0.52, 0.52]},
            {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": len(positions),
             "type": "VEC3"},
            {"bufferView": 1, "componentType": 5125, "count": len(indices), "type": "SCALAR"}],
    }
    text = json.dumps(gltf).encode()
    text += b" " * (-len(text) % 4)
    binary = vertex_bytes + index_bytes
    binary += b"\0" * (-len(binary) % 4)
    with open(path, "wb") as out:
        out.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(binary)))
        out.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        out.write(struct.pack("<II", len(binary), 0x004E4942) + binary)
    return len(indices) // 3


def lod_gate(tool, call, check):
    """N1 part 2: import a dense mesh and see its detail follow distance."""
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "bumpy.glb").replace("\\", "/")
        triangles = write_bumpy_sphere_glb(path)
        mesh = tool("import_mesh", {"path": path})
        check(mesh["triangles"] == triangles and mesh["levels"] >= 4,
              f"mesh imported with a LOD hierarchy, got {mesh}")
        check(mesh["coarsest_triangles"] < triangles // 50, "the coarsest level is small")
        tool("new_scene")
        rock = tool("create_entity", {"mesh": path, "scale": [10, 10, 10]})
        check(rock["shape"] == "mesh", "entity draws the mesh")

        def triangles_from(distance, frames=6):  # streaming loads detail over frames
            tool("set_camera", {"position": [0, 0, distance], "look_at": [0, 0, 0]})
            for _ in range(frames):
                call("ping")  # let a frame render
            return tool("get_render_settings")["last_frame"]

        backend = tool("get_engine_info")["backend"]
        if backend != "null":
            # GPU culling (the default): the same cut, counted on the GPU a
            # few frames late. Occlusion off, so it matches the CPU's cut.
            tool("set_render_settings", {"occlusion_culling": False})
            gpu = triangles_from(9, frames=16)
            tool("set_render_settings", {"gpu_culling": False})
            cpu = triangles_from(9)
            check(gpu["from_gpu"] and not gpu["overflow"], f"GPU counts arrive: {gpu}")
            check(abs(gpu["triangles"] - cpu["triangles"]) <= cpu["triangles"] // 20,
                  f"the GPU cut matches the CPU's: {gpu} vs {cpu}")
            tool("set_render_settings", {"gpu_culling": True, "occlusion_culling": True})
            occluded = triangles_from(9, frames=16)
            check(0 < occluded["triangles"] <= gpu["triangles"],
                  f"occlusion culling only removes: {occluded} vs {gpu}")
        tool("set_render_settings", {"gpu_culling": False})  # exact counts, every backend
        near = triangles_from(9)
        mid = triangles_from(60)
        far = triangles_from(3000)
        check(near["triangles"] > mid["triangles"] > far["triangles"] > 0,
              f"detail drops with distance: {near}, {mid}, {far}")
        check(near["triangles"] <= triangles, "never more than the file has")
        tool("set_camera", {"position": [0, 0, 60], "look_at": [0, 0, 0]})
        tool("set_render_settings", {"debug_view": "lod_levels", "gpu_culling": True})
        shot = call("tools/call", {"name": "capture_screenshot",
                                   "arguments": {"max_width": 800, "file_name": "mcp_lod_levels"}})
        check(not shot.get("isError"), "LOD view screenshot taken")
        tool("set_render_settings", {"debug_view": "lit", "lod_error_pixels": 1,
                                     "gpu_culling": True})
        scene_path = os.path.join(folder, "rock.scene.json")
        tool("save_scene", {"path": scene_path})
        tool("new_scene")
        check(tool("load_scene", {"path": scene_path})["entities"] == 1, "mesh scene loads")
        check(tool("list_entities", {"full": True})["entities"][0]["mesh"] == path, "path kept")
        missing = call("tools/call", {"name": "create_entity",
                                      "arguments": {"mesh": os.path.join(folder, "nope.glb")}})
        check(missing.get("isError") is True, "a missing file is a tool error")
    tool("new_scene")


def kit_gate(tool, call, check):
    """N1 part 5: a kit placed hundreds of times, and a decal, in a fixed number of draws."""
    tool("new_scene")
    tool("create_entity", {"name": "Ground", "shape": "plane", "scale": [400, 1, 400],
                           "base_color": [0.3, 0.3, 0.3], "roughness": 0.9})
    kit = tool("create_kit", {"name": "house", "pieces": [
        {"name": "walls", "shape": "cube", "position": [0, 1.5, 0], "scale": [4, 3, 5],
         "base_color": [0.8, 0.75, 0.6]},
        {"name": "roof", "shape": "cube", "position": [0, 3.3, 0], "scale": [4.4, 0.6, 5.4],
         "base_color": [0.5, 0.15, 0.1]},
        {"name": "door", "shape": "cube", "position": [0, 1, 2.51], "scale": [1, 2, 0.05],
         "base_color": [0.3, 0.2, 0.1]},
        {"name": "dirt", "shape": "decal", "position": [0, 0, 3.5], "scale": [3, 1, 2],
         "base_color": [0.1, 0.08, 0.05], "decal": {"shape": "splat", "seed": 3}},
    ]})
    check(kit["pieces"] == 4 and kit["placements"] == 0, f"kit made, got {kit}")
    town = [{"kit": "house", "position": [x * 9.0 - 90, 0, -z * 9.0 - 10]} for x in range(20) for z in range(10)]
    placed = tool("create_entities", {"entities": town})
    check(len(placed["ids"]) == 200, "200 houses placed")
    check(tool("list_kits")["kits"][0]["placements"] == 200, "the kit knows its placements")
    tool("create_entity", {"name": "Glow", "shape": "decal", "position": [0, 0, -6], "scale": [8, 1, 8],
                           "base_color": [0, 0, 0], "emissive": [1, 0, 0], "decal": {"shape": "disc"}})
    tool("set_camera", {"position": [0, 6, 2], "look_at": [0, 0, -6]})
    tool("set_render_settings", {"gpu_culling": False})
    for _ in range(4):
        call("ping")
    frame = tool("get_render_settings")["last_frame"]
    check(frame["draws"] == 2, f"600 cubes still take one raster and one shading pass, got {frame}")
    check(frame["materials"] == 4, f"the town shares 4 materials, got {frame['materials']}")
    check(frame["decals"] >= 2 and not frame["decal_overflow"], f"decals in view, got {frame}")
    tool("set_render_settings", {"gpu_culling": True})
    shot = call("tools/call", {"name": "capture_screenshot",
                               "arguments": {"max_width": 800, "file_name": "mcp_kits"}})
    check(not shot.get("isError"), "town screenshot taken")
    if tool("get_engine_info")["backend"] != "null":
        image = [c for c in shot["content"] if c["type"] == "image"][0]
        width, height, pixels = decode_png(base64.b64decode(image["data"]))
        # The glowing decal is where the camera looks (as the tower check).
        x, y = width // 2, height // 2
        r, g, b, _ = pixels[(y * width + x) * 4:(y * width + x) * 4 + 4]
        # Red well above the rest (the tone mapper whitens bright highlights).
        check(r > 150 and r - g > 60 and r - b > 60, f"the decal glows red on the ground, got {(r, g, b)}")
    tool("undo")  # the decal
    check(tool("list_entities")["count"] == 201, "undo removes the decal")
    tool("new_scene")


def voxel_gate(tool, call, check):
    """N2 part 2: the voxel copy of the scene builds around the camera, follows edits and
    shows in the voxel view."""
    tool("new_scene")
    tool("create_entity", {"name": "Ground", "shape": "plane", "scale": [400, 1, 400],
                           "base_color": [0.3, 0.3, 0.3], "roughness": 0.9})
    block = tool("create_entity", {"name": "Block", "shape": "cube", "position": [0, 2, -10],
                                   "scale": [4, 4, 4], "base_color": [0.9, 0.05, 0.05]})["id"]
    tool("set_camera", {"position": [0, 3, 2], "look_at": [0, 2, -10]})
    # Sanitizer builds build bricks some 30x slower: a bigger budget settles them in time.
    tool("set_render_settings", {"debug_view": "voxels", "voxel_ms_per_frame": 50})

    def settle():
        for _ in range(60):
            for _ in range(10):
                call("ping")
            voxels = tool("get_render_settings")["last_frame"]["voxels"]
            if voxels["pending"] == 0:
                return voxels
        return voxels

    voxels = settle()
    check(voxels["pending"] == 0 and voxels["bricks"] > 0 and not voxels["overflow"],
          f"the voxel world is built, got {voxels}")
    check(voxels["cpu_ms"] < 100, f"building keeps near its budget, got {voxels}")
    built = voxels["bricks"]

    def centre():
        shot = call("tools/call", {"name": "capture_screenshot", "arguments": {"max_width": 480}})
        image = [c for c in shot["content"] if c["type"] == "image"][0]
        width, height, pixels = decode_png(base64.b64decode(image["data"]))
        x, y = width // 2, height // 2
        return pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]

    real = tool("get_engine_info")["backend"] != "null"
    if real:
        r, g, b = centre()
        check(r > 120 and r - g > 50 and r - b > 50, f"the voxel view shows the red block, got {(r, g, b)}")
    # Moving the block rebuilds only around it; the view follows.
    tool("set_entity", {"id": block, "position": [30, 2, -10]})
    voxels = settle()
    check(voxels["pending"] == 0 and abs(voxels["bricks"] - built) <= built // 4,
          f"the moved block is rebuilt where it went, got {voxels} (was {built} bricks)")
    if real:
        r, g, b = centre()
        check(r - g < 30 and r - b < 30, f"the block's old place is no longer red, got {(r, g, b)}")
    tool("set_render_settings", {"debug_view": "lit", "voxel_ms_per_frame": 1})
    tool("new_scene")


def n1_gate(tool, call, check):
    """N1 part 6, the N1 gate scene (tools/n1_gate.py measures its speed on a real GPU): a
    kit-built town and 10,000 trees in a fixed number of draws, with every bit of detail the
    view asks for loaded, and anti-aliasing that only touches edges."""
    materials = build_gate_scene(tool)
    count = tool("list_entities")["count"]
    check(count == ENTITIES, f"the town and 10,000 trees are placed, got {count} entities")
    tool("set_render_settings", {"gpu_culling": False})

    def settle():  # CPU culling: what a frame asks for loads by the next
        for _ in range(40):
            call("ping")
            frame = tool("get_render_settings")["last_frame"]
            if frame["streaming"]["waiting"] == 0 and frame["streaming"]["streamed_in"] == 0:
                return frame
        return frame

    for position, look_at in flight_path(6):
        tool("set_camera", {"position": position, "look_at": look_at})
        frame = settle()
        check(frame["draws"] == 2 and frame["post_draws"] == 3,
              f"20,000 pieces still take one raster pass, one shading pass and 3 anti-aliasing passes, got {frame}")
        check(frame["materials"] == materials, f"{materials} shared materials, got {frame['materials']}")
        check(not frame["overflow"], f"nothing dropped, got {frame}")
        check(frame["streaming"]["waiting"] == 0, f"all detail in view loaded (no late sharpening), got {frame}")
    tool("set_render_settings", {"antialiasing": False})
    call("ping")
    check(tool("get_render_settings")["last_frame"]["post_draws"] == 0, "anti-aliasing turns off")
    tool("set_render_settings", {"antialiasing": True, "gpu_culling": True})
    if tool("get_engine_info")["backend"] != "null":
        # Popping: small steps down the street change the image a little each time, evenly.
        # This only catches large jumps (detail switching across much of the view, e.g. a
        # whole level or the pool being flushed); the error bound (under a pixel) and the
        # "nothing left waiting" checks above cover the fine scale.
        changes, previous = [], None
        for step in range(5):
            z = -20.0 - step * 0.2
            tool("set_camera", {"position": [STREET_X, 1.8, z], "look_at": [STREET_X, 1.6, z - 30]})
            for _ in range(6):
                call("ping")
            shot = call("tools/call", {"name": "capture_screenshot", "arguments": {"max_width": 480}})
            image = [c for c in shot["content"] if c["type"] == "image"][0]
            width, height, pixels = decode_png(base64.b64decode(image["data"]))
            # The middle of the viewport, away from the panels' changing numbers.
            rows = range(int(height * 0.2), int(height * 0.6))
            cols = range(int(width * 0.3), int(width * 0.65))
            region = [pixels[(y * width + x) * 4 + c] for y in rows for x in cols for c in range(3)]
            if previous is not None:
                changes.append(sum(abs(a - b) for a, b in zip(region, previous)) / len(region))
            previous = region
        check(max(changes) <= 3 * min(changes) + 1.5, f"the view changes evenly while moving, got {changes}")
    tool("new_scene")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    command = [sys.argv[1], "--mcp", *sys.argv[2:]]
    editor = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=None, text=True, encoding="utf-8", bufsize=1)
    timer = threading.Timer(TIMEOUT, editor.kill)
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
        line = editor.stdout.readline()
        if not line:
            raise RuntimeError(f"editor closed the connection during {method}")
        reply = json.loads(line)
        if reply.get("id") != next_id:
            raise RuntimeError(f"reply id {reply.get('id')} does not match {next_id}")
        if "error" in reply:
            raise RuntimeError(f"{method} failed: {reply['error']}")
        return reply["result"]

    def tool(name, arguments=None):
        result = call("tools/call", {"name": name, "arguments": arguments or {}})
        if result.get("isError"):
            raise RuntimeError(f"tool {name} failed: {result['content'][0]['text']}")
        return result.get("structuredContent", {})

    def check(condition, what):
        if not condition:
            raise RuntimeError("check failed: " + what)

    try:
        init = call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                   "clientInfo": {"name": "mcp_smoke", "version": "1"}})
        check(init["serverInfo"]["name"] == "giant-editor", "server name")
        editor.stdin.write(json.dumps({"jsonrpc": "2.0",
                                       "method": "notifications/initialized"}) + "\n")
        editor.stdin.flush()

        names = {t["name"] for t in call("tools/list")["tools"]}
        for expected in ("get_engine_info", "set_clear_color", "undo", "get_log", "quit_editor"):
            check(expected in names, f"tool {expected} is listed")

        info = tool("get_engine_info")
        check(info["frames_completed"] >= 0, "engine info")

        before = tool("get_clear_color")
        tool("set_clear_color", {"r": 0.5, "g": 0.25, "b": 1.0})
        check(tool("get_clear_color") == {"r": 0.5, "g": 0.25, "b": 1.0}, "colour changed")
        tool("undo")
        check(tool("get_clear_color") == before, "undo restored the colour")
        history = tool("get_history")
        check(len(history["redo"]) == 1, "redo history has the undone change")

        bad = call("tools/call", {"name": "set_clear_color", "arguments": {"r": 2}})
        check(bad.get("isError") is True, "bad arguments are a tool error")

        log = tool("get_log", {"count": 200})
        check(any("MCP server" in line["text"] for line in log["lines"]), "log readable")

        shot = call("tools/call", {"name": "capture_screenshot",
                                   "arguments": {"max_width": 1600,
                                                 "file_name": "mcp_smoke"}})
        check(not shot.get("isError"), "screenshot taken")
        images = [c for c in shot["content"] if c["type"] == "image"]
        check(len(images) == 1 and images[0]["mimeType"] == "image/png", "one PNG image")
        width, height, pixels = decode_png(base64.b64decode(images[0]["data"]))
        info = shot["structuredContent"]
        check((width, height) == (info["width"], info["height"]), "PNG size matches")
        # The viewport (centre of the default layout) shows the background
        # colour, sRGB-encoded: linear (0.15, 0.34, 0.71) is about (108, 157, 219).
        x, y = int(width * 0.47), int(height * 0.3)
        r, g, b, _ = pixels[(y * width + x) * 4:(y * width + x) * 4 + 4]
        check(abs(r - 108) <= 3 and abs(g - 157) <= 3 and abs(b - 219) <= 3,
              f"viewport pixel is the background colour, got {(r, g, b)}")
        check(os.path.isfile(info["saved_to"]), "screenshot saved")

        build_scene(tool, call, check)
        lod_gate(tool, call, check)
        kit_gate(tool, call, check)
        voxel_gate(tool, call, check)
        n1_gate(tool, call, check)

        sample = os.environ.get("GIANT_SAMPLE_SCRIPTS")
        if sample:
            # C# scripts: the sample fades the background through commands.
            history_before = len(tool("get_history")["undo"])
            tool("load_scripts", {"path": sample})
            for _ in range(200):
                if tool("get_scripts")["running"] == 1:
                    break
            check(tool("get_scripts")["running"] == 1, "the sample script runs")
            for _ in range(60):
                call("ping")  # let a few frames pass
            lines = tool("get_log", {"count": 200})["lines"]
            check(any("Hello from C#" in line["text"] for line in lines), "the script logged")
            check(tool("get_clear_color") != before, "the script changed the background")
            check(len(tool("get_history")["undo"]) == history_before,
                  "script changes stay out of the undo history")
            tool("unload_scripts")
            call("ping")
            check(tool("get_scripts")["loaded"] == "", "scripts unloaded")

            # Load then unload sent together (usually handled in one frame):
            # the latest request wins, so nothing stays loaded.
            ids = []
            for name, arguments in (("load_scripts", {"path": sample}), ("unload_scripts", {})):
                next_id += 1
                ids.append(next_id)
                editor.stdin.write(json.dumps({"jsonrpc": "2.0", "id": next_id,
                                               "method": "tools/call",
                                               "params": {"name": name, "arguments": arguments}}) + "\n")
            editor.stdin.flush()
            replies = [json.loads(editor.stdout.readline()) for _ in ids]
            check([r.get("id") for r in replies] == ids, "replies to both requests, in order")
            for _ in range(30):
                call("ping")  # let the requests apply
            state = tool("get_scripts")
            check(state["loaded"] == "" and state["running"] == 0,
                  f"load then unload leaves nothing loaded, got {state}")
        else:
            print("mcp_smoke: C# scripts skipped (GIANT_SAMPLE_SCRIPTS not set)")

        tool("quit_editor")
        code = editor.wait(timeout=TIMEOUT)
        check(code == 0, f"editor exit code {code}")
    except Exception as error:  # report and fail
        print(f"mcp_smoke: {error}", file=sys.stderr)
        editor.kill()
        editor.wait()
        return 1
    finally:
        timer.cancel()

    # A client that just goes away must not leave the editor running.
    editor = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                              stderr=None)
    editor.stdin.close()
    try:
        code = editor.wait(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        editor.kill()
        print("mcp_smoke: editor kept running after the client disconnected", file=sys.stderr)
        return 1
    if code != 0:
        print(f"mcp_smoke: editor exit code {code} after disconnect", file=sys.stderr)
        return 1

    # A client that crashes with requests in flight (both pipes close while
    # the editor still has replies to write) must not kill the editor.
    editor = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=None)
    ping = (json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"}) + "\n").encode()
    editor.stdout.close()
    try:
        editor.stdin.write(ping * 200)
        editor.stdin.close()
    except (BrokenPipeError, OSError):
        pass  # the editor may already have closed; its exit code is checked below
    try:
        code = editor.wait(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        editor.kill()
        print("mcp_smoke: editor kept running after the client crashed", file=sys.stderr)
        return 1
    if code != 0:
        print(f"mcp_smoke: editor exit code {code} after the client crashed", file=sys.stderr)
        return 1
    print("mcp_smoke: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
