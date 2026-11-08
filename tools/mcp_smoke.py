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
import time

from n1_gate import ENTITIES, STREET_X, build_gate_scene, flight_path

TIMEOUT = 2400  # seconds for the whole run (the gates are heavy on the software renderer with GPU validation: 1300-1500 s on Windows CI runners by N3 part 4, whose speed varies 2x; each gate prints its time)


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
    # Each frame's main-thread time by phase: the last 10 frames in full.
    recorded = info["frame_times"]["recorded"]
    check("phases" not in info["frame_times"], "phases only when asked")
    timed = tool("get_engine_info", {"phases_since": recorded - 10, "phases_over_ms": 0})["frame_times"]
    phases = timed["phases"]
    check(phases["since"] == recorded - 10 and phases["frames"] >= 10, f"the frames asked for: {phases}")
    check(all(name in phases["typical"] for name in ("mcp", "ui", "physics", "scene", "render", "ui_draw",
                                                     "render/voxels", "render/shading", "end_frame",
                                                     "streaming", "tail")),
          f"the frame's phases, got {sorted(phases['typical'])}")
    check(len(phases["slow"]) == phases["frames"], "every frame over 0 ms is given in full")
    check(phases["typical"]["render"] > 0, "rendering a scene takes time")
    for frame in phases["slow"]:
        p = frame["phases"]
        check(all(t >= 0 for t in p.values()) and frame["cpu_ms"] >= 0, f"no negative times: {frame}")
        # The renderer's parts lie within render (rounding and parts under
        # 0.05 ms left out: a little slack).
        parts = sum(t for n, t in p.items() if n.startswith("render/"))
        check(parts <= p.get("render", 0) + 0.1, f"the renderer's parts fit within render: {frame}")
        # The phases cover the frame: together close to its wall-clock time.
        whole = sum(t for n, t in p.items() if "/" not in n)
        check(abs(whole - frame["ms"]) <= 1.0 + 0.02 * frame["ms"],
              f"the phases add up to the frame time: {whole} vs {frame}")
        check(frame["cpu_ms"] <= 1.1 * frame["ms"] + 1.0, f"no more CPU than wall time: {frame}")
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


def world_gate(tool, call, check):
    """A world streamed by cell: saved, opened, loaded around the camera, let go far off.
    Terrain tiles are saved in their cell's file and stream with it."""
    with tempfile.TemporaryDirectory() as folder:
        world = os.path.join(folder, "valley")
        tool("create_entities", {"entities": [{"name": "near", "position": [5, 0.5, 5]},
                                              {"name": "near", "position": [70, 0.5, 5]},
                                              {"name": "far", "position": [6000, 0.5, 5]}]})
        tool("create_terrain", {"from": [0, 0], "to": [10, 10], "samples": 8, "height": 1})
        tool("create_terrain", {"from": [6000, 0], "to": [6010, 10], "height": 2})
        made = tool("save_world", {"folder": world})
        check(made["cells_written"] == 3 and made["cells"] == 3, f"world saved, got {made}")
        # The far ground: a level-1 chunk over each tile, and the level-2 chunk at the origin
        # (the far tile is not at one of level 2's sample points, 128 m apart).
        check(made["chunks_written"] == 3, f"far ground chunks written, got {made}")
        check(tool("get_world")["ground"] == 2, "two cells' files hold ground")
        tool("set_camera", {"position": [10, 5, 30], "look_at": [10, 0, 0]})
        opened = tool("open_world", {"folder": world})
        check(opened["cells"] == 3 and tool("list_entities")["count"] == 0, "world opened, nothing loaded yet")
        info = {}
        for _ in range(200):
            info = tool("get_world")
            if info["loaded"] == 2 and info["reading"] == 0 and info["waiting"] == 0:
                break
        check(info["loaded"] == 2 and info["entities"] == 2, f"the near cells load, got {info}")
        ground = tool("get_terrain", {"points": [[5, 5], [6005, 5]]})
        check(ground["tiles"] == 1 and ground["samples"] == 8 and ground["heights"] == [1, None],
              f"the near tile loads with its cell, got {ground}")
        far = {}
        for _ in range(300):
            far = tool("get_world")["far_ground"]
            if far["levels"][0]["drawn"] == 1:
                break
        check(far["levels"][0]["files"] == 2 and far["levels"][0]["loaded"] == 2 and far["levels"][1]["files"] == 1
              and far["levels"][0]["drawn"] == 1 and far["failed"] == 0,
              f"the far tile's chunk draws as far ground, the near one as its full tile, got {far}")
        tool("set_camera", {"position": [6000, 5, 30], "look_at": [6000, 0, 0]})
        for _ in range(200):
            info = tool("get_world")
            if info["loaded"] == 1 and info["entities"] == 1:
                break
        check(info["loaded"] == 1 and info["entities"] == 1 and info["unsaved_cells"] == 0,
              f"far off, the far cell loads and the near ones go, got {info}")
        ground = tool("get_terrain", {"points": [[5, 5], [6005, 5]]})
        check(ground["tiles"] == 1 and ground["heights"] == [None, 2] and info["tiles_unloaded"] == 1,
              f"the far tile comes, the near one goes, got {ground} and {info}")
    tool("new_scene")


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
    # The voxel view is only shown for the screenshots: on the software GPU its ray march
    # takes long, and the voxel world builds whatever the view.
    tool("set_render_settings", {"voxel_ms_per_frame": 50})

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

    def probe(position):  # the finest level holding the point
        levels = tool("probe_voxels", {"position": position})["levels"]
        return next((l["state"] for l in levels if l["state"] != "outside"), "outside")

    # The block's +X face (x = 2) and the ground are solid voxels; inside the block is empty.
    check(probe([2.05, 2.1, -10.1]) == "solid", "the block's side is in the voxel world")
    check(probe([0.1, 0.05, 0.1]) == "solid", "the ground is in the voxel world")
    check(probe([0.1, 2.1, -10.1]) == "empty", "the block is hollow (a shell of voxels)")

    def centre():
        tool("set_render_settings", {"debug_view": "voxels"})
        call("ping")
        shot = call("tools/call", {"name": "capture_screenshot", "arguments": {"max_width": 480}})
        image = [c for c in shot["content"] if c["type"] == "image"][0]
        width, height, pixels = decode_png(base64.b64decode(image["data"]))
        x, y = width // 2, height // 2
        tool("set_render_settings", {"debug_view": "lit"})
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
    check(probe([2.05, 2.1, -10.1]) == "empty", "the block's old place is empty")
    check(probe([32.05, 2.1, -10.1]) == "solid", "the block is in the voxel world where it went")
    check(probe([0.1, 0.05, 0.1]) == "solid", "the ground stayed")
    if real:
        r, g, b = centre()
        check(r - g < 30 and r - b < 30, f"the block's old place is no longer red, got {(r, g, b)}")
    tool("set_render_settings", {"voxel_ms_per_frame": 1})
    tool("new_scene")


def far_field_gate(tool, call, check):
    """N2 part 3: past the far-field distance small instances are not drawn as meshes; the
    voxel world shows them instead, lit like the meshes."""
    tool("new_scene")
    tool("set_camera", {"position": [0, 10, 0], "look_at": [0, 10, -600]})
    tool("create_entity", {"name": "Far block", "shape": "cube", "position": [0, 10, -600],
                           "scale": [30, 30, 30], "base_color": [0.9, 0.05, 0.05], "roughness": 0.9})
    tool("set_render_settings", {"voxel_ms_per_frame": 50, "far_field_distance": 300})

    def frame():
        for _ in range(60):
            for _ in range(10):
                call("ping")
            last = tool("get_render_settings")["last_frame"]
            if last["voxels"]["pending"] == 0:
                return last
        return last

    def centre():
        call("ping")
        shot = call("tools/call", {"name": "capture_screenshot", "arguments": {"max_width": 480}})
        image = [c for c in shot["content"] if c["type"] == "image"][0]
        width, height, pixels = decode_png(base64.b64decode(image["data"]))
        x, y = width // 2, height // 2
        return pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]

    real = tool("get_engine_info")["backend"] != "null"
    last = frame()
    check(last["voxels"]["pending"] == 0, f"the voxel world is built, got {last['voxels']}")
    check(last["far_field"] == {"on": True, "culled": 1},
          f"the far block is left to the voxel world, got {last['far_field']}")
    if real:
        r, g, b = centre()
        check(r > 80 and r - g > 40 and r - b > 40, f"the far field shows the red block, got {(r, g, b)}")
    # Pushed out past the block, the distance gives it back to the meshes.
    tool("set_render_settings", {"far_field_distance": 3000})
    last = frame()
    check(last["far_field"] == {"on": True, "culled": 0}, f"the block is a mesh again, got {last['far_field']}")
    tool("set_render_settings", {"far_field_distance": 300, "voxel_far_field": False})
    last = frame()
    check(last["far_field"] == {"on": False, "culled": 0}, f"the far field turns off, got {last['far_field']}")
    if real:
        r, g, b = centre()
        check(r > 80 and r - g > 40 and r - b > 40, f"the mesh shows the red block, got {(r, g, b)}")
    tool("set_render_settings", {"voxel_ms_per_frame": 1, "far_field_distance": 3000, "voxel_far_field": True})
    tool("new_scene")


def terrain_gate(tool, call, check):
    """N3 part 4: terrain tiles are made by command, built off the main thread, drawn as
    meshes at every distance (never left to the far field) and undone in one step."""
    tool("new_scene")
    made = tool("create_terrain", {"from": [-64, -64], "to": [63, 63], "samples": 16,
                                   "hills": {"amplitude": 6, "size": 80, "seed": 2}})
    check(made["tiles"] == 4 and made["spacing"] == 4.0, f"four tiles made, got {made}")
    # A lone tile far out, past the far-field distance, beside a small block there.
    tool("create_terrain", {"from": [0, -1000], "to": [10, -990], "height": 0})
    tool("create_entity", {"name": "Far block", "shape": "cube", "position": [100, 5, -1000],
                           "scale": [10, 10, 10]})
    tool("set_terrain_material", {"base_color": [0.1, 0.6, 0.1], "roughness": 0.9})
    tool("set_render_settings", {"voxel_ms_per_frame": 50, "far_field_distance": 300})
    tool("set_camera", {"position": [0, 60, 40], "look_at": [0, 0, -40]})
    info = None
    for _ in range(200):
        call("ping")
        info = tool("get_terrain")
        if info["meshes"]["building"] == 0:
            break
    meshes = {k: v for k, v in info["meshes"].items() if k != "built"}  # built: ever, other gates too
    # Two triangles a square, and the edge walls: 4 edges of 16 quads, seen from both sides.
    check(meshes == {"tiles": 5, "building": 0, "triangles": 5 * (16 * 16 * 2 + 4 * 16 * 4)},
          f"every tile's mesh is built, got {info['meshes']}")
    height = tool("get_terrain", {"points": [[0, 0], [500, 500]]})["heights"]
    check(height[1] is None and abs(height[0]) <= 6, f"the ground's height is sampled, got {height}")
    # N3 part 5: a brush stroke raises the ground and is undone in one step.
    sculpted = tool("sculpt_terrain", {"op": "raise", "points": [[-20, 0], [20, 0]], "radius": 10,
                                       "strength": 3, "falloff": 0})
    raised = tool("get_terrain", {"points": [[0, 0]]})["heights"][0]
    # On the line between two rows of tiles: all four change; the middle gets every dab in reach.
    check(sculpted["tiles_changed"] == 4 and raised > height[0] + 3,
          f"the stroke raises the ground, got {sculpted} and {raised}")
    tool("undo")
    check(tool("get_terrain", {"points": [[0, 0]]})["heights"][0] == height[0], "undo puts the ground back")
    # A height map (16-bit RAW, 2 x 2: black then white rows) makes ground; undone too.
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "hills.r16")
        with open(path, "wb") as f:
            f.write(bytes([0, 0, 0, 0, 255, 255, 255, 255]))
        imported = tool("import_height_map", {"file": path, "from": [2000, 2000], "to": [2100, 2100],
                                              "low": 0, "high": 50})
    ramp = tool("get_terrain", {"points": [[2000, 2000], [2000, 2100]]})["heights"]
    check(imported["tiles"] == 4 and ramp[0] == 0 and abs(ramp[1] - 50) < 1e-3,
          f"the height map makes ground, got {imported} and {ramp}")
    tool("undo")
    check(tool("get_terrain")["tiles"] == 5, "undo takes the imported ground away")
    # N3 part 6: a scatter layer strews stones over the four tiles (a point every 8 m square),
    # placed on a worker; undone, they go.
    tool("add_scatter_layer", {"name": "stones", "items": [{"shape": "sphere", "scale": [0.5, 1]}],
                               "spacing": 8, "area": {"from": [-64, -64], "to": [64, 64]}})
    scatter = None
    for _ in range(200):
        call("ping")
        scatter = tool("get_scatter")
        if scatter["building"] == 0:
            break
    check(scatter["cells"] == 4 and scatter["points"] == 256 and scatter["layers"][0]["points"] == 256,
          f"the stones are placed, got cells {scatter['cells']}, points {scatter['points']}")
    tool("undo")
    for _ in range(200):
        call("ping")
        scatter = tool("get_scatter")
        if scatter["building"] == 0:
            break
    check(scatter["layers"] == [] and scatter["points"] == 0, f"undo takes the stones away, got {scatter}")
    last = None
    for _ in range(60):
        for _ in range(10):
            call("ping")
        last = tool("get_render_settings")["last_frame"]
        if last["voxels"]["pending"] == 0:
            break
    check(last["far_field"] == {"on": True, "culled": 1},
          f"the far block is left to the voxels, the far tile is not, got {last['far_field']}")
    if tool("get_engine_info")["backend"] != "null":
        call("ping")
        shot = call("tools/call", {"name": "capture_screenshot", "arguments": {"max_width": 480}})
        image = [c for c in shot["content"] if c["type"] == "image"][0]
        width, height_px, pixels = decode_png(base64.b64decode(image["data"]))
        x, y = width // 2, height_px // 2
        r, g, b = pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]
        check(g > r + 20 and g > b + 20, f"the green ground fills the view, got {(r, g, b)}")
    tool("undo")  # the material
    tool("undo")  # the block
    tool("undo")  # the far tile
    check(tool("get_terrain")["tiles"] == 4, "undo takes the far tile away")
    tool("set_render_settings", {"voxel_ms_per_frame": 1, "far_field_distance": 3000})
    tool("new_scene")


def shadow_gate(tool, call, check):
    """N2 part 4: sun shadows from virtual shadow pages. The pages shading asks for are drawn
    a few frames later, cast where the sun says, and stay cached until something moves."""
    tool("new_scene")
    tool("create_entity", {"name": "Ground", "shape": "plane", "scale": [200, 1, 200],
                           "base_color": [0.6, 0.6, 0.6], "roughness": 0.9})
    # A slab floating 2 m up, east of the origin (x 3 to 9, z -4 to 4).
    block = tool("create_entity", {"name": "Block", "shape": "cube", "position": [6, 3, 0],
                                   "scale": [6, 2, 8], "base_color": [0.6, 0.6, 0.6]})["id"]
    # The sun in the east, 30 degrees up: the shadow falls west, over the origin (x -3.9 to 5.5).
    tool("set_lighting", {"sun_elevation_degrees": 30, "sun_azimuth_degrees": 90})
    tool("set_camera", {"position": [0, 12, 6], "look_at": [0, 0, 0]})

    def settle():
        # Requests take a few frames to come back: quiet three samples in a row.
        last, quiet = None, 0
        for _ in range(60):
            for _ in range(10):
                call("ping")
            last = tool("get_render_settings")["last_frame"]
            shadows = last["shadows"]
            still = not shadows["on"] or (shadows["pages"] > 0 and shadows["waiting"] == 0 and shadows["drawn"] == 0)
            quiet = quiet + 1 if still else 0
            if not real or quiet >= 3:
                return last
        return last

    def centre():
        call("ping")
        shot = call("tools/call", {"name": "capture_screenshot", "arguments": {"max_width": 480}})
        image = [c for c in shot["content"] if c["type"] == "image"][0]
        width, height, pixels = decode_png(base64.b64decode(image["data"]))
        x, y = width // 2, height // 2
        return pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]

    real = tool("get_engine_info")["backend"] != "null"
    settings = tool("get_render_settings")
    check(settings["shadows"] is True and settings["shadow_pages_per_frame"] == 64, "shadows are on by default")
    last = settle()
    check(last["shadows"]["on"] and last["shadows"]["capacity"] == 1024,
          f"the shadow page pool is there, got {last['shadows']}")
    if real:
        check(last["shadows"]["pages"] > 0 and last["shadows"]["missing"] == 0,
              f"shading asked for pages and they were drawn, got {last['shadows']}")
        shaded = centre()
        tool("set_render_settings", {"shadows": False})
        settle()
        sunlit = centre()
        check(sum(shaded) < 0.6 * sum(sunlit), f"the block shadows the ground, got {shaded} against {sunlit}")
        tool("set_render_settings", {"shadows": True})
        # Moved away, the block takes its shadow along: its pages are drawn again.
        tool("set_entity", {"id": block, "position": [6, 3, 40]})
        settle()
        moved = centre()
        check(abs(sum(moved) - sum(sunlit)) <= 9, f"the ground is sunlit again, got {moved} against {sunlit}")
    tool("set_render_settings", {"shadows": False})
    last = settle()
    check(not last["shadows"]["on"] and last["shadows"]["draws"] == 0, f"shadows turn off, got {last['shadows']}")
    tool("set_render_settings", {"shadows": True})
    tool("new_scene")


def weather_gate(tool, call, check):
    """N3 part 7: weather. A sky the sun shines through, clouds that drift on the wind and
    shade the ground, and fog; undoable, and a scene without it keeps the flat sky."""
    tool("new_scene")
    tool("create_entity", {"name": "Ground", "shape": "plane", "scale": [2000, 1, 2000],
                           "base_color": [0.5, 0.5, 0.5], "roughness": 0.9})
    tool("set_lighting", {"sun_elevation_degrees": 40, "sun_azimuth_degrees": 180})
    tool("set_camera", {"position": [0, 2, 0], "look_at": [0, 6, -40]})
    real = tool("get_engine_info")["backend"] != "null"

    def frames(n=6):
        for _ in range(n):
            call("ping")
        return tool("get_weather")

    def sky_pixel():
        call("ping")
        shot = call("tools/call", {"name": "capture_screenshot", "arguments": {"max_width": 320}})
        image = [c for c in shot["content"] if c["type"] == "image"][0]
        width, height, pixels = decode_png(base64.b64decode(image["data"]))
        x, y = width // 2, height // 8
        return pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]

    state = frames()
    check(state["weather"] is None and not state["sky"]["on"], f"no weather: the flat sky, got {state}")
    check(set(state["presets"]) >= {"clear", "overcast", "storm", "fog"}, f"presets listed, got {state['presets']}")
    tool("set_weather", {"preset": "clear"})
    state = frames()
    check(state["weather"]["cloud_cover"] == 0 and state["sky"]["on"] and state["progress"] == 1,
          f"clear weather: the sky on, got {state}")
    clear = sky_pixel() if real else None
    tool("set_weather", {"preset": "overcast", "over_seconds": 3600})
    state = frames()
    check(state["weather"]["cloud_cover"] > 0.9 and state["progress"] < 1
          and state["shown"]["cloud_cover"] < 0.1, f"overcast blends in slowly, got {state}")
    tool("set_weather", {"preset": "overcast"})
    state = frames()
    check(state["shown"] == state["weather"] and state["sky"]["clouds"], f"overcast at once, got {state}")
    if real:
        grey = sky_pixel()
        check(clear[2] > clear[0] + 20, f"the clear sky is blue, got {clear}")
        check(abs(grey[2] - grey[0]) < abs(clear[2] - clear[0]), f"overcast is greyer, got {grey} against {clear}")
    # N3 part 8: snow falls and lies on the ground; rain wets it.
    tool("set_weather", {"preset": "snow", "snow_cover": 1})  # a preset alone keeps the ground as it is
    state = frames()
    check(state["falling"]["snow"] > 0 and state["falling"]["rain"] == 0 and state["ground"]["snow_cover"] == 1
          and state["sky"]["falling"] and state["sky"]["wet_surfaces"], f"snow falls and lies, got {state}")
    tool("set_weather", {"preset": "rain", "wetness": 0.5, "snow_cover": 0})
    state = frames()
    check(state["falling"]["rain"] > 0 and state["ground"]["snow_cover"] == 0
          and abs(state["ground"]["wetness"] - 0.5) < 0.05, f"rain on half-wet ground, got {state}")
    tool("undo")  # the rain
    tool("undo")  # the snow
    bad = call("tools/call", {"name": "set_weather", "arguments": {"preset": "hail"}})
    check(bad.get("isError") is True, "an unknown preset is refused")
    tool("undo")  # the overcast at once
    tool("undo")  # the overcast blending in
    check(tool("get_weather")["weather"]["cloud_cover"] == 0, "undo brings the clear sky back")
    tool("clear_weather")
    state = frames()
    check(state["weather"] is None and not state["sky"]["on"], f"cleared: the flat sky again, got {state}")
    tool("new_scene")


def physics_gate(tool, call, check):
    """N4 part 1: physics. Entities with bodies collide; simulating (a play button) lets the
    dynamic ones fall in real time, drawn where they are while the scene keeps where they were
    put; edits are followed, and stopping puts them back."""
    tool("new_scene")
    tool("create_entity", {"name": "Ground", "shape": "plane", "scale": [200, 1, 200]})
    ball = tool("create_entity", {"name": "Ball", "shape": "sphere", "position": [0, 20, 0], "body": "dynamic"})["id"]
    hit = tool("raycast", {"origin": [0, 50, 0], "direction": [0, -1, 0]})
    check(hit["hit"] and hit["entity"] == ball and abs(hit["point"][1] - 20.5) < 0.01, f"a ray meets the ball, got {hit}")
    tool("simulate", {"on": True})

    def where(entity):
        e = tool("get_entity", {"id": entity})
        return e["simulated"]["position"] if "simulated" in e else e["position"]

    def fall(entity, rest_y, what):
        # Frames move it (no step_physics): about 2 s of falling. Judged by the clock, so a
        # slow software GPU only takes longer, and a frame rate with no vsync cannot run out.
        started = time.monotonic()
        y = None
        while time.monotonic() - started < 120:
            call("ping")
            y = where(entity)[1]
            if abs(y - rest_y) < 0.03 and tool("get_physics")["active"] == 0:
                return
        check(False, f"{what}: comes to rest at y {rest_y} in real time, at {y}")

    fall(ball, 0.5, "the ball")
    state = tool("get_physics")
    check(state["simulating"] and state["dynamic_bodies"] == 1 and state["steps"] > 0 and state["refused"] == 0
          and state["moved"] == 1, f"physics while simulating, got {state}")
    check(tool("get_entity", {"id": ball})["position"] == [0, 20, 0], "the scene keeps where the ball was put")
    tool("set_entity", {"id": ball, "position": [5, 10, 0]})  # an edit: falls from there
    fall(ball, 0.5, "the moved ball")
    check(abs(where(ball)[0] - 5) < 0.05, "the moved ball falls straight down")
    tool("simulate", {"on": False})
    for _ in range(3):
        call("ping")
    e = tool("get_entity", {"id": ball})
    check(e["position"] == [5, 10, 0] and "simulated" not in e, f"stopping puts the ball back, got {e}")
    check(not tool("get_physics")["simulating"], "stopped")
    tool("new_scene")


def player_gate(tool, call, check):
    """N4 part 2: play mode. The player stands on what is under the camera, walks in real time
    where it is told (frames move it), up a step, and stopping gives the camera back."""
    tool("new_scene")
    tool("create_entity", {"name": "Ground", "shape": "plane", "scale": [200, 1, 200]})
    step = tool("create_entity", {"name": "Step", "shape": "cube", "position": [0, 0.15, -8],
                                  "scale": [4, 0.3, 4]})["id"]
    tool("set_camera", {"position": [0, 6, 0], "look_at": [0, 0, -10]})
    camera = tool("get_camera")
    player = tool("play", {"on": True})
    check(player["playing"] and abs(player["position"][1]) < 0.05, f"the player stands on the ground, got {player}")
    tool("set_player_input", {"forward": 1, "yaw": 0})  # down -z, onto the step
    started = time.monotonic()
    player = None
    while time.monotonic() - started < 120:
        call("ping")
        player = tool("get_player")
        if player["position"][2] < -7 and player["on_ground"]:
            break
    tool("set_player_input", {"forward": 0})  # stays on the step, the eyes catch up
    for _ in range(30):
        call("ping")
    player = tool("get_player")
    check(player["standing_on"] == step and abs(player["position"][1] - 0.3) < 0.03,
          f"the player walks up onto the step in real time, got {player}")
    check(abs(tool("get_camera")["position"][1] - (0.3 + 1.65)) < 0.2, "the camera is at the player's eyes")
    stopped = tool("play", {"on": False})
    check(not stopped["playing"] and not tool("get_physics")["simulating"], f"stopped, got {stopped}")
    back = tool("get_camera")
    check(all(abs(a - b) < 1e-6 for a, b in zip(back["position"], camera["position"])),
          f"the camera goes back, got {back}")
    tool("new_scene")


def gi_gate(tool, call, check):
    """N2 part 5: global illumination. The voxel world is lit a budgeted number of bricks a
    frame (new ones first, then all in turn), in the lit views and the voxel view."""
    tool("new_scene")
    tool("create_entity", {"name": "Ground", "shape": "plane", "scale": [40, 1, 40],
                           "base_color": [0.8, 0.8, 0.8], "roughness": 0.9})
    tool("create_entity", {"name": "Wall", "shape": "cube", "position": [2, 1, 0], "scale": [0.4, 2, 8],
                           "base_color": [0.9, 0.05, 0.05], "roughness": 0.9})
    tool("set_lighting", {"sun_elevation_degrees": 37, "sun_azimuth_degrees": 270})
    tool("set_camera", {"position": [-2, 8, 6], "look_at": [1, 0, 0]})
    tool("set_render_settings", {"gi": True, "gi_bricks_per_frame": 64, "screen_space_reflections": True})

    def settle():
        last = None
        for _ in range(60):
            for _ in range(5):
                call("ping")
            last = tool("get_render_settings")["last_frame"]
            if last["voxels"]["pending"] == 0 and last["gi"]["waiting"] == 0:
                return last
        return last

    last = settle()
    check(last["gi"]["on"] and last["gi"]["waiting"] == 0 and last["gi"]["lit"] > 0,
          f"the voxel world is lit, new bricks first, then in turn, got {last['gi']}")
    # N2 part 5b: reflections, from the lit voxels and from the image itself.
    check(last["reflections"]["voxel"] and last["reflections"]["screen"],
          f"glossy surfaces reflect the voxel world and the screen, got {last['reflections']}")
    tool("set_render_settings", {"screen_space_reflections": False})
    last = settle()
    check(last["reflections"]["voxel"] and not last["reflections"]["screen"],
          f"screen-space reflections turn off on their own, got {last['reflections']}")
    tool("set_render_settings", {"reflections": False, "screen_space_reflections": True})
    last = settle()
    check(not last["reflections"]["voxel"] and not last["reflections"]["screen"],
          f"reflections turn off, got {last['reflections']}")
    tool("set_render_settings", {"reflections": True, "screen_space_reflections": False})
    tool("set_render_settings", {"debug_view": "lighting"})
    check(tool("get_render_settings")["debug_view"] == "lighting", "the lighting view can be picked")
    last = settle()
    check(last["gi"]["on"], f"the lighting view has global illumination, got {last['gi']}")
    tool("set_render_settings", {"debug_view": "clusters"})
    last = settle()
    check(not last["gi"]["on"] and last["gi"]["lit"] == 0, f"debug views of the geometry have none, got {last['gi']}")
    tool("set_render_settings", {"debug_view": "lit", "gi": False})
    last = settle()
    check(not last["gi"]["on"], f"global illumination turns off, got {last['gi']}")
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
        for expected in ("get_engine_info", "set_clear_color", "undo", "get_log", "quit_editor", "get_animation",
                         "get_swing", "find_path", "get_nav", "walk_to", "get_walker"):
            check(expected in names, f"tool {expected} is listed")

        info = tool("get_engine_info")
        check(info["frames_completed"] >= 0, "engine info")

        way = tool("find_path", {"from": [0, 0, 0], "to": [5, 0, 0]})
        check(way["state"] in ("found", "failed"), "find_path answers")
        check(tool("get_nav")["settings"]["agent_radius"] > 0, "get_nav answers")
        check(tool("get_walker")["walkers"] == 0, "get_walker answers")

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

        settings = tool("get_render_settings")
        check(settings["gi"] is True and settings["gi_bricks_per_frame"] == 256, "global illumination is on by default")
        check(settings["reflections"] is True and settings["screen_space_reflections"] is True,
              "reflections are on by default")
        # Lighting the voxel world and the screen reflections pass are heavy
        # on the software GPU CI runs on: off for the other gates (the
        # renderer's own d3d12 tests check them), on in gi_gate.
        tool("set_render_settings", {"gi": False, "screen_space_reflections": False})
        for gate in (build_scene, world_gate, lod_gate, kit_gate, voxel_gate, far_field_gate,
                     terrain_gate, shadow_gate, weather_gate, physics_gate, player_gate, gi_gate, n1_gate):
            # Each gate's time, so a run near the timeout shows which one grew.
            started = time.monotonic()
            frames = tool("get_engine_info")["frames_completed"]
            gate(tool, call, check)
            frames = tool("get_engine_info")["frames_completed"] - frames
            print(f"mcp_smoke: {gate.__name__} took {time.monotonic() - started:.1f} s, "
                  f"{frames} frames", flush=True)

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
