"""Checks the gates' person file (no editor needed): python test_gate_person.py"""
import json
import math
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate_person import JOINTS, RUN_SPEED, WALK_SPEED, write_person  # noqa: E402

SIZES = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
FORMATS = {5126: "f", 5123: "H", 5125: "I"}


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, length = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and version == 2 and length == len(data), "not a whole glb"
    text_length, text_kind = struct.unpack_from("<II", data, 12)
    assert text_kind == 0x4E4F534A and text_length % 4 == 0
    document = json.loads(data[20:20 + text_length])
    binary_length, binary_kind = struct.unpack_from("<II", data, 20 + text_length)
    assert binary_kind == 0x004E4942 and binary_length == document["buffers"][0]["byteLength"]
    binary = data[28 + text_length:28 + text_length + binary_length]

    def rows(accessor):
        a = document["accessors"][accessor]
        view = document["bufferViews"][a["bufferView"]]
        size = SIZES[a["type"]]
        flat = struct.unpack_from(f"<{a['count'] * size}{FORMATS[a['componentType']]}", binary, view["byteOffset"])
        assert view["byteOffset"] % 4 == 0 and view["byteLength"] == struct.calcsize(f"<{a['count'] * size}{FORMATS[a['componentType']]}")
        return [flat[i:i + size] for i in range(0, len(flat), size)]

    return document, rows


def main():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "person.glb")
        small = write_person(path, 0.1)
        document, rows = read(path)
        check(small["joints"] == len(JOINTS) == len(document["skins"][0]["joints"]), "the joints")
        check(1000 < small["triangles"] < 10000, f"a small one has {small['triangles']} triangles")
        check([m["name"] for m in document["materials"]] == ["skin", "clothes", "eye"], "the materials")
        names = [n["name"] for n in document["nodes"][:len(JOINTS)]]
        for wanted in ("Hips", "Spine", "Head", "LeftFoot", "RightFoot", "LeftForeArm", "RightLeg"):
            check(wanted in names, f"no joint {wanted}")
        low, high = 1e9, -1e9
        for primitive in document["meshes"][0]["primitives"]:
            positions = rows(primitive["attributes"]["POSITION"])
            normals = rows(primitive["attributes"]["NORMAL"])
            uvs = rows(primitive["attributes"]["TEXCOORD_0"])
            joints = rows(primitive["attributes"]["JOINTS_0"])
            weights = rows(primitive["attributes"]["WEIGHTS_0"])
            indices = [i[0] for i in rows(primitive["indices"])]
            count = len(positions)
            check(len(normals) == len(uvs) == len(joints) == len(weights) == count, "the attributes' counts")
            check(len(indices) % 3 == 0 and max(indices) < count, "the indices")
            check(all(abs(sum(w) - 1.0) < 1e-5 and min(w) >= 0.0 for w in weights), "weights that do not add up to 1")
            check(all(max(j) < len(JOINTS) for j in joints), "a joint out of range")
            check(all(abs(math.sqrt(sum(c * c for c in n)) - 1.0) < 1e-3 for n in normals), "a normal not of unit length")
            check(all(0.0 <= u <= 1.0 and 0.0 <= v <= 1.0 for u, v in uvs), "a uv outside the square")
            low = min(low, min(p[1] for p in positions))
            high = max(high, max(p[1] for p in positions))
            # Faces look outward: each triangle's own normal agrees with its corners' (but for slivers at the domes' tips).
            against = faces = empty = 0
            for t in range(0, len(indices), 3):
                a, b, c = (positions[indices[t + k]] for k in range(3))
                e1 = [b[k] - a[k] for k in range(3)]
                e2 = [c[k] - a[k] for k in range(3)]
                face = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                area = math.sqrt(sum(v * v for v in face))
                if area < 1e-12:
                    empty += 1
                    continue
                faces += 1
                n = normals[indices[t]]
                if sum(face[k] * n[k] for k in range(3)) < 0.0:
                    against += 1
            check(faces > 0 and against == 0, f"{against} of {faces} triangles face inward")
            check(empty == 0, f"{empty} triangles have no area")
            stated = document["accessors"][primitive["attributes"]["POSITION"]]
            for axis in range(3):
                check(abs(stated["min"][axis] - min(p[axis] for p in positions)) < 1e-6 and
                      abs(stated["max"][axis] - max(p[axis] for p in positions)) < 1e-6, "the positions' stated bounds")
        check(0.0 <= low < 0.05 and 1.75 < high < 1.85, f"it stands from {low:.2f} to {high:.2f} m")
        # The eyes' irises: uv (0.5, 0.5) where they face +z.
        eye = document["meshes"][0]["primitives"][2]
        flat = all(abs(uv[0] - (0.5 + 0.5 * n[0])) < 1e-5 and abs(uv[1] - (0.5 - 0.5 * n[1])) < 1e-5
                   for n, uv in zip(rows(eye["attributes"]["NORMAL"]), rows(eye["attributes"]["TEXCOORD_0"])))
        check(flat, "an eye's uv is not laid flat as seen from the front")
        # The clips: each ends posed as it began, the hips moved on by its speed.
        clips = {a["name"]: a for a in document["animations"]}
        check(sorted(clips) == ["idle", "run", "walk"], f"the clips {sorted(clips)}")
        for name, speed in (("idle", 0.0), ("walk", WALK_SPEED), ("run", RUN_SPEED)):
            clip = clips[name]
            for channel in clip["channels"]:
                sampler = clip["samplers"][channel["sampler"]]
                times = [t[0] for t in rows(sampler["input"])]
                values = rows(sampler["output"])
                check(times[0] == 0.0 and all(b > a for a, b in zip(times, times[1:])), f"{name}: times not rising")
                if channel["target"]["path"] == "translation":
                    check(names[channel["target"]["node"]] == "Hips", f"{name}: a joint other than the hips is moved")
                    check(abs(values[-1][2] - values[0][2] - speed * times[-1]) < 1e-4, f"{name}: the hips' travel")
                    check(abs(values[-1][1] - values[0][1]) < 1e-5, f"{name}: the hips' height does not come round")
                else:
                    check(all(abs(math.sqrt(sum(c * c for c in q)) - 1.0) < 1e-4 for q in values), f"{name}: a turn not of unit length")
                    check(max(abs(a - b) for a, b in zip(values[0], values[-1])) < 1e-5, f"{name}: a joint does not come round")
        # The gait: the lower ankle stays at its height at rest (no foot in the ground or in the air), and each
        # leg comes forward (+z) while its knee is folded, the arm across from it with it.
        index = {n: i for i, n in enumerate(names)}
        rest = {j[0]: j[2] for j in JOINTS}

        def turn(v, q):  # v turned by the quaternion q
            x, y, z, w = q
            cx, cy, cz = y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]
            return (v[0] + 2.0 * (w * cx + y * cz - z * cy), v[1] + 2.0 * (w * cy + z * cx - x * cz),
                    v[2] + 2.0 * (w * cz + x * cy - y * cx))

        for name in ("walk", "run"):
            clip = clips[name]
            tracks = {}
            for channel in clip["channels"]:
                tracks[(names[channel["target"]["node"]], channel["target"]["path"])] = rows(clip["samplers"][channel["sampler"]]["output"])
            keys = len(tracks[("Hips", "translation")])
            lowest, forward_folded = [], 0
            for k in range(keys):
                hips = tracks[("Hips", "translation")][k]
                ankles = {}
                for side in ("Left", "Right"):
                    thigh, shin = tracks[(side + "UpLeg", "rotation")][k], tracks[(side + "Leg", "rotation")][k]
                    hip = [rest[side + "UpLeg"][a] - rest["Hips"][a] for a in range(3)]
                    knee = [rest[side + "Leg"][a] - rest[side + "UpLeg"][a] for a in range(3)]
                    ankle = [rest[side + "Foot"][a] - rest[side + "Leg"][a] for a in range(3)]
                    lower_leg = turn(ankle, shin)
                    leg = turn([knee[a] + lower_leg[a] for a in range(3)], thigh)
                    ankles[side] = [hips[a] + hip[a] + leg[a] for a in range(3)]
                lowest.append(min(ankles["Left"][1], ankles["Right"][1]))
                # A knee well folded: that foot is ahead of or level with the other one's hip line (coming forward).
                for side, other in (("Left", "Right"), ("Right", "Left")):
                    shin = tracks[(side + "Leg", "rotation")][k]
                    if 2.0 * math.degrees(math.asin(min(1.0, abs(shin[0])))) > 30.0:
                        forward_folded += 1 if shin[0] > 0.0 else -1000  # folds back (a positive turn about +x)
            check(all(abs(h - rest["LeftFoot"][1]) < 0.02 for h in lowest),
                  f"{name}: the lower ankle is at {min(lowest):.3f} to {max(lowest):.3f} m, not at {rest['LeftFoot'][1]} m")
            check(forward_folded > 0, f"{name}: a knee folds forward")
            # A quarter in, the left leg is forward (its thigh turned back about +x is negative) and the right arm with it.
            quarter = (keys - 1) // 4
            check(tracks[("LeftUpLeg", "rotation")][quarter][0] < 0.0 < tracks[("RightUpLeg", "rotation")][quarter][0],
                  f"{name}: the legs do not swing opposite, the left forward first")
            check(tracks[("RightArm", "rotation")][quarter][0] < 0.0 < tracks[("LeftArm", "rotation")][quarter][0],
                  f"{name}: the arm across does not swing with the leg")
        full = write_person(path, 1.0)
        check(30000 < full["triangles"] < 60000, f"a whole one has {full['triangles']} triangles")
        read(path)
        # A file apiece: only that material's triangles, the skeleton and clips whole; the three add up.
        apiece = 0
        for name in ("skin", "clothes", "eye"):
            one = write_person(path, 1.0, materials=[name])
            part, _ = read(path)
            check(one["materials"] == [name] == [m["name"] for m in part["materials"]] and
                  len(part["meshes"][0]["primitives"]) == 1 and part["meshes"][0]["primitives"][0]["material"] == 0,
                  f"a file of the {name} alone")
            check(len(part["skins"][0]["joints"]) == len(JOINTS) and len(part["animations"]) == 3,
                  f"the {name} alone keeps the skeleton and clips")
            apiece += one["triangles"]
        check(apiece == full["triangles"], f"the parts have {apiece} triangles, the whole {full['triangles']}")
        for bad in ([], ["hat"], ["skin", "skin"]):
            try:
                write_person(path, 0.1, materials=bad)
                check(False, f"materials {bad} were taken")
            except ValueError:
                pass
    for f in failures:
        print(f"FAIL: {f}")
    print("test_gate_person: " + ("FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
