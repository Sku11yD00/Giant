"""A person for the gates: a skinned glTF (.glb) written from nothing, since
the repository carries no character of its own.

    from gate_person import write_person
    info = write_person("person.glb", detail=1.0)

It stands 1.8 m tall facing +z (its left along +x), its arms hanging, with
the joints the engine knows by name (Hips, Spine, Spine1, Neck, Head,
Left/Right Shoulder, Arm, ForeArm, Hand, UpLeg, Leg, Foot). Each bone is a
round limb skinned to its joint and, near where it starts, to the joint
above (so elbows and knees bend smoothly); the head is a sphere with two
eyes, each a sphere of its own material whose iris is at uv (0.5, 0.5)
facing +z. Materials: "skin", "clothes", "eye" (`materials` writes only
some: a file of one material). With `detail` 1 it has
about 44 thousand triangles (a background NPC's count: enough for the
skinned levels of detail to have something to drop); 0.1 is a few
thousand, for tests.

Clips, all looping, the hips moving through model space as motion
matching wants: "idle" (2 s, standing, breathing), "walk" (1 s, 1.4 m
along +z), "run" (0.7 s, 2.8 m along +z). The legs swing as sines and the
hips ride on the lower leg: a gait to exercise the engine with, not one to
judge foot sliding by (the standing foot slips some centimetres a step).

No third-party modules: the file is packed by hand.
"""
import json
import math
import struct

# name, parent, where it starts (model space, metres), where its limb ends, radius at each end, material
JOINTS = [
    ("Hips", None, (0.0, 1.00, 0.0), (0.0, 1.14, 0.0), (0.15, 0.14), "clothes"),
    ("Spine", "Hips", (0.0, 1.12, 0.0), (0.0, 1.32, 0.0), (0.14, 0.15), "clothes"),
    ("Spine1", "Spine", (0.0, 1.30, 0.0), (0.0, 1.50, 0.0), (0.15, 0.13), "clothes"),
    ("Neck", "Spine1", (0.0, 1.50, 0.0), (0.0, 1.60, 0.0), (0.055, 0.05), "skin"),
    ("Head", "Neck", (0.0, 1.60, 0.0), (0.0, 1.80, 0.0), (0.0, 0.0), "skin"),
    ("LeftShoulder", "Spine1", (0.04, 1.46, 0.0), (0.19, 1.46, 0.0), (0.06, 0.055), "clothes"),
    ("LeftArm", "LeftShoulder", (0.19, 1.46, 0.0), (0.19, 1.16, 0.0), (0.05, 0.042), "clothes"),
    ("LeftForeArm", "LeftArm", (0.19, 1.16, 0.0), (0.19, 0.90, 0.0), (0.042, 0.032), "skin"),
    ("LeftHand", "LeftForeArm", (0.19, 0.90, 0.0), (0.19, 0.78, 0.0), (0.035, 0.025), "skin"),
    ("RightShoulder", "Spine1", (-0.04, 1.46, 0.0), (-0.19, 1.46, 0.0), (0.06, 0.055), "clothes"),
    ("RightArm", "RightShoulder", (-0.19, 1.46, 0.0), (-0.19, 1.16, 0.0), (0.05, 0.042), "clothes"),
    ("RightForeArm", "RightArm", (-0.19, 1.16, 0.0), (-0.19, 0.90, 0.0), (0.042, 0.032), "skin"),
    ("RightHand", "RightForeArm", (-0.19, 0.90, 0.0), (-0.19, 0.78, 0.0), (0.035, 0.025), "skin"),
    ("LeftUpLeg", "Hips", (0.09, 0.98, 0.0), (0.09, 0.53, 0.0), (0.085, 0.06), "clothes"),
    ("LeftLeg", "LeftUpLeg", (0.09, 0.53, 0.0), (0.09, 0.09, 0.0), (0.06, 0.042), "clothes"),
    ("LeftFoot", "LeftLeg", (0.09, 0.09, 0.0), (0.09, 0.05, 0.17), (0.045, 0.04), "clothes"),
    ("RightUpLeg", "Hips", (-0.09, 0.98, 0.0), (-0.09, 0.53, 0.0), (0.085, 0.06), "clothes"),
    ("RightLeg", "RightUpLeg", (-0.09, 0.53, 0.0), (-0.09, 0.09, 0.0), (0.06, 0.042), "clothes"),
    ("RightFoot", "RightLeg", (-0.09, 0.09, 0.0), (-0.09, 0.05, 0.17), (0.045, 0.04), "clothes"),
]
NAMES = [j[0] for j in JOINTS]
MATERIALS = {
    "skin": {"name": "skin", "pbrMetallicRoughness": {"baseColorFactor": [0.78, 0.57, 0.46, 1.0], "metallicFactor": 0.0,
                                                      "roughnessFactor": 0.5}},
    "clothes": {"name": "clothes", "pbrMetallicRoughness": {"baseColorFactor": [0.22, 0.26, 0.32, 1.0],
                                                            "metallicFactor": 0.0, "roughnessFactor": 0.85}},
    "eye": {"name": "eye", "pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.9, 0.88, 1.0], "metallicFactor": 0.0,
                                                    "roughnessFactor": 0.1}},
}
HEAD_CENTRE = (0.0, 1.70, 0.0)
HEAD_RADIUS = 0.105
EYE_RADIUS = 0.013
EYES = ((0.034, 1.715, 0.094), (-0.034, 1.715, 0.094))
WALK_SPEED = 1.4
RUN_SPEED = 4.0


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _unit(a):
    n = math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
    return (a[0] / n, a[1] / n, a[2] / n)


class _Part:
    """One material's triangles."""

    def __init__(self):
        self.positions, self.normals, self.uvs, self.joints, self.weights, self.indices = [], [], [], [], [], []

    def vertex(self, p, n, uv, joint, parent, share):
        """`share` of it follows `parent` (0: all the joint's)."""
        self.positions.append(p)
        self.normals.append(n)
        self.uvs.append(uv)
        if parent is None or share <= 0.0:
            self.joints.append((joint, 0, 0, 0))
            self.weights.append((1.0, 0.0, 0.0, 0.0))
        else:
            self.joints.append((joint, parent, 0, 0))
            self.weights.append((1.0 - share, share, 0.0, 0.0))
        return len(self.positions) - 1

    def grid(self, rows, columns, first, closed=()):
        """Triangles of a grid of rows x (columns + 1) vertices starting at `first`, facing out.
        Rows in `closed` are a single point (a tip, a pole): no triangle lies along them."""
        for r in range(rows - 1):
            for c in range(columns):
                a = first + r * (columns + 1) + c
                b = a + columns + 1
                if r not in closed:
                    self.indices += [a, a + 1, b]
                if r + 1 not in closed:
                    self.indices += [a + 1, b + 1, b]


def _limb(part, joint, parent, start, end, radii, around, along):
    """A round limb from `start` to `end`, closed by a small dome at each end."""
    axis = _unit(_sub(end, start))
    side = _unit(_cross(axis, (0.0, 0.0, 1.0) if abs(axis[2]) < 0.9 else (1.0, 0.0, 0.0)))
    other = _cross(axis, side)
    length = math.dist(start, end)
    # Rings: a dome (3 rings), the shaft, a dome; t along the limb, s the radius' share.
    rings = [(-0.5, 0.0), (-0.35, 0.7)] + [(i / (along - 1), 1.0) for i in range(along)] + [(1.35, 0.7), (1.5, 0.0)]
    first = len(part.positions)
    for t, s in rings:
        inside = min(max(t, 0.0), 1.0)
        radius = (radii[0] + (radii[1] - radii[0]) * inside) * s
        # Past the ends the dome bulges by the end's radius.
        reach = inside * length + (t - inside) * 2.0 * (radii[0] if t < 0.0 else radii[1])
        centre = (start[0] + axis[0] * reach, start[1] + axis[1] * reach, start[2] + axis[2] * reach)
        # Near where it starts it follows the joint above too: half at the start, none a third along.
        share = max(0.0, 0.5 - 1.5 * max(t, 0.0)) if parent is not None else 0.0
        lean = 0.0 if s == 1.0 else (-1.0 if t < 0.0 else 1.0) * (1.0 - s * 0.7)
        for c in range(around + 1):
            angle = 2.0 * math.pi * c / around
            out = tuple(side[k] * math.cos(angle) + other[k] * math.sin(angle) for k in range(3))
            normal = _unit(tuple(out[k] * (1.0 - abs(lean)) + axis[k] * lean for k in range(3))) if s > 0.0 else \
                tuple(axis[k] * (-1.0 if t < 0.0 else 1.0) for k in range(3))
            p = tuple(centre[k] + out[k] * radius for k in range(3))
            part.vertex(p, normal, (c / around, min(max((t + 0.5) / 2.0, 0.0), 1.0)), joint, parent, share)
    part.grid(len(rings), around, first, closed=(0, len(rings) - 1))


def _sphere(part, joint, centre, radius, around, rings, front_uv=False):
    """A sphere. `front_uv`: its uv is laid flat as seen from +z, (0.5, 0.5) where it faces +z (an eye's iris)."""
    first = len(part.positions)
    for r in range(rings):
        polar = math.pi * r / (rings - 1)
        for c in range(around + 1):
            angle = 2.0 * math.pi * c / around
            n = (math.sin(polar) * math.cos(angle), math.cos(polar), math.sin(polar) * math.sin(angle))
            uv = (0.5 + 0.5 * n[0], 0.5 - 0.5 * n[1]) if front_uv else (c / around, r / (rings - 1))
            part.vertex(tuple(centre[k] + n[k] * radius for k in range(3)), n, uv, joint, None, 0.0)
    part.grid(rings, around, first, closed=(0, rings - 1))


def _quat(axis, degrees):
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def _clips():
    """name -> (seconds, {joint name: (path, [value per key])}), keys evenly spaced, the last the first again."""
    x = (1.0, 0.0, 0.0)
    hips_at = JOINTS[0][2]
    clips = {}
    keys = 33

    thigh_length = math.dist(JOINTS[NAMES.index("LeftUpLeg")][2], JOINTS[NAMES.index("LeftLeg")][2])
    shin_length = math.dist(JOINTS[NAMES.index("LeftLeg")][2], JOINTS[NAMES.index("LeftFoot")][2])

    def ankle_drop(thigh_degrees, knee_degrees):
        """How far under its hip joint a leg's ankle hangs, turned so (both about +x)."""
        a = math.radians(thigh_degrees)
        return thigh_length * math.cos(a) + shin_length * math.cos(a + math.radians(knee_degrees))

    def stride(name, seconds, speed, thigh, knee, arm, elbow, lean):
        tracks = {n: ("rotation", []) for n in ("LeftUpLeg", "RightUpLeg", "LeftLeg", "RightLeg", "LeftArm", "RightArm",
                                                 "LeftForeArm", "RightForeArm", "Spine")}
        tracks["Hips"] = ("translation", [])
        for k in range(keys):
            phase = 2.0 * math.pi * k / (keys - 1)
            for sign, leg, shin, upper, fore in ((1.0, "LeftUpLeg", "LeftLeg", "RightArm", "RightForeArm"),
                                                 (-1.0, "RightUpLeg", "RightLeg", "LeftArm", "LeftForeArm")):
                swing = sign * math.sin(phase)
                # (About +x a leg hanging down swings back: forward is the other way.)
                tracks[leg][1].append(_quat(x, -thigh * swing))
                # The knee folds while its leg comes forward.
                tracks[shin][1].append(_quat(x, knee * max(0.0, sign * math.cos(phase)) + 4.0))
                # The arm across swings with the leg.
                tracks[upper][1].append(_quat(x, -arm * swing))
                tracks[fore][1].append(_quat(x, -elbow * (0.6 + 0.4 * swing)))
            tracks["Spine"][1].append(_quat(x, lean))
            travel = speed * seconds * k / (keys - 1)
            # The hips ride on the leg that reaches lower: its ankle stays at its height at rest.
            reach = max(ankle_drop(-thigh * sign * math.sin(phase), knee * max(0.0, sign * math.cos(phase)) + 4.0)
                        for sign in (1.0, -1.0))
            tracks["Hips"][1].append((hips_at[0], hips_at[1] - (thigh_length + shin_length - reach), travel))
        clips[name] = (seconds, tracks)

    stride("walk", 1.0, WALK_SPEED, 26.0, 50.0, 18.0, 15.0, 2.0)
    stride("run", 0.7, RUN_SPEED, 42.0, 90.0, 40.0, 75.0, 10.0)
    idle = {"Hips": ("translation", []), "Spine": ("rotation", []), "Spine1": ("rotation", [])}
    for k in range(keys):
        breath = math.sin(2.0 * math.pi * k / (keys - 1))
        idle["Hips"][1].append((hips_at[0], hips_at[1] - 0.003 * (1.0 + breath), 0.0))
        idle["Spine"][1].append(_quat(x, 1.0 * breath))
        idle["Spine1"][1].append(_quat(x, -1.5 * breath))
    clips["idle"] = (2.0, idle)
    return clips


def write_person(path, detail=1.0, materials=None):
    """Writes the person to `path` (a .glb). Returns {"triangles", "vertices", "joints", "clips", "materials"}.
    `materials`: only those materials' triangles (the skeleton and clips whole), for a file of one
    material (the whole file is drawn in parts, one a material)."""
    wanted = list(MATERIALS) if materials is None else list(materials)
    if not wanted or any(name not in MATERIALS for name in wanted) or len(set(wanted)) != len(wanted):
        raise ValueError(f"materials must be some of {list(MATERIALS)}, each once, got {wanted}")
    around = max(6, int(round(40 * math.sqrt(detail))))
    along = max(3, int(round(24 * math.sqrt(detail))))
    index_of = {name: i for i, name in enumerate(NAMES)}
    parts = {name: _Part() for name in MATERIALS}
    for i, (name, parent, start, end, radii, material) in enumerate(JOINTS):
        if name == "Head":
            _sphere(parts["skin"], i, HEAD_CENTRE, HEAD_RADIUS, max(8, around * 3 // 2), max(6, along * 2))
            continue
        _limb(parts[material], i, index_of[parent] if parent else None, start, end, radii, around, along)
    for centre in EYES:
        _sphere(parts["eye"], index_of["Head"], centre, EYE_RADIUS, max(8, around // 2), max(6, along // 2), front_uv=True)

    binary = bytearray()
    views, accessors = [], []

    def add(data, accessor):
        while len(binary) % 4:
            binary.append(0)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)})
        binary.extend(data)
        accessors.append({"bufferView": len(views) - 1, **accessor})
        return len(accessors) - 1

    def floats(rows):
        flat = [v for row in rows for v in row]
        return struct.pack(f"<{len(flat)}f", *flat)

    primitives = []
    material_names = [name for name in MATERIALS if name in wanted]
    triangles = vertices = 0
    for m, name in enumerate(material_names):
        part = parts[name]
        count = len(part.positions)
        lo = [min(p[k] for p in part.positions) for k in range(3)]
        hi = [max(p[k] for p in part.positions) for k in range(3)]
        attributes = {
            "POSITION": add(floats(part.positions), {"componentType": 5126, "count": count, "type": "VEC3", "min": lo,
                                                     "max": hi}),
            "NORMAL": add(floats(part.normals), {"componentType": 5126, "count": count, "type": "VEC3"}),
            "TEXCOORD_0": add(floats(part.uvs), {"componentType": 5126, "count": count, "type": "VEC2"}),
            "JOINTS_0": add(struct.pack(f"<{count * 4}H", *[v for row in part.joints for v in row]),
                            {"componentType": 5123, "count": count, "type": "VEC4"}),
            "WEIGHTS_0": add(floats(part.weights), {"componentType": 5126, "count": count, "type": "VEC4"}),
        }
        indices = add(struct.pack(f"<{len(part.indices)}I", *part.indices),
                      {"componentType": 5125, "count": len(part.indices), "type": "SCALAR"})
        primitives.append({"attributes": attributes, "indices": indices, "material": m})
        triangles += len(part.indices) // 3
        vertices += count

    inverse = add(floats([(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -j[2][0], -j[2][1], -j[2][2], 1) for j in JOINTS]),
                  {"componentType": 5126, "count": len(JOINTS), "type": "MAT4"})
    nodes = []
    for i, (name, parent, start, _end, _radii, _material) in enumerate(JOINTS):
        above = JOINTS[index_of[parent]][2] if parent else (0.0, 0.0, 0.0)
        node = {"name": name, "translation": list(_sub(start, above))}
        children = [c for c, j in enumerate(JOINTS) if j[1] == name]
        if children:
            node["children"] = children
        nodes.append(node)
    nodes.append({"name": "body", "mesh": 0, "skin": 0})

    animations = []
    for name, (seconds, tracks) in _clips().items():
        keys = len(next(iter(tracks.values()))[1])
        times = [seconds * k / (keys - 1) for k in range(keys)]
        time_accessor = add(struct.pack(f"<{keys}f", *times),
                            {"componentType": 5126, "count": keys, "type": "SCALAR", "min": [0.0], "max": [seconds]})
        samplers, channels = [], []
        for joint, (target, values) in tracks.items():
            output = add(floats(values), {"componentType": 5126, "count": keys,
                                          "type": "VEC3" if target == "translation" else "VEC4"})
            samplers.append({"input": time_accessor, "output": output, "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": index_of[joint], "path": target}})
        animations.append({"name": name, "samplers": samplers, "channels": channels})

    while len(binary) % 4:
        binary.append(0)
    document = {
        "asset": {"version": "2.0", "generator": "giant gate_person"},
        "scene": 0,
        "scenes": [{"nodes": [0, len(JOINTS)]}],
        "nodes": nodes,
        "meshes": [{"name": "person", "primitives": primitives}],
        "materials": [MATERIALS[n] for n in material_names],
        "skins": [{"joints": list(range(len(JOINTS))), "inverseBindMatrices": inverse}],
        "animations": animations,
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    text = json.dumps(document, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as out:
        out.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(binary)))
        out.write(struct.pack("<II", len(text), 0x4E4F534A))
        out.write(text)
        out.write(struct.pack("<II", len(binary), 0x004E4942))
        out.write(binary)
    return {"triangles": triangles, "vertices": vertices, "joints": len(JOINTS), "clips": [a["name"] for a in animations],
            "materials": material_names}


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    print(write_person(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 1.0))
