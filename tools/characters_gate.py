#!/usr/bin/env python3
"""The characters gate (characters plan C9, the N5 gate): a crowd in the N3
gate's town, flown through.

    python3 tools/characters_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames). It builds the N3 gate's edited valley and
peoples its town with the gates' person (tools/gate_person.py, about 44
thousand triangles each): walkers in the alleys between the houses, each
wearing strand hair, sent walking from crossing to crossing and kept at it
all flight long, each in its turn sent back (motion matched: they stand,
walk and stand again by themselves); people standing
round the town; and a few by the street to be seen close: each one entity
whose skin, clothes and eyes are parts of the one file with a look apiece
(`parts`), with long hair that moves. It saves that as a world and opens it
again, simulating, with every N2 feature on, and flies the N3 gate's
flight (up the river towards the town, down its street at head height
through the crowd and past those close-ups, spiralling up and out) at
2560x1440 where the screen allows. In the street a blast goes off among
the walkers and shots are fired at others: those struck go limp, lie, and
get up again. It passes when:
- the flight holds 60 fps without micro stutter (as the other gates:
  average under 16.7 ms, 99th percentile under 20.8 ms, at most one
  hitch), over the whole flight and over each leg on its own;
- the crowd was there: every one of them animated (none frozen or waiting),
  the walkers walking in their walk clip and none failing to find its way,
  those standing in their idle;
- they were drawn by distance: from up the river the crowd (what is drawn
  with it less what is drawn with it hidden) takes under a fiftieth of its
  triangles and one far off is posed less than every frame; a close-up is
  posed every frame;
- the blast and the shots dropped ragdolls, none left waiting to be made
  one, and at least one got up again;
- close up, skin spread its light, the eyes are eyes, bodies shut light
  out (character occlusion) and hair was grown, simulated and drawn;
- what characters add on the GPU (the "skin" pass that poses them and the
  "character occlusion", "subsurface scattering", "hair" and "hair
  shadows" passes) takes under 2.0 ms half a metre from a close-up's
  face, caught up first;
- nothing overflowed.

On CI (the null device, which draws nothing and times nothing) it checks
all but the speed, the passes' budget and the crowd's share of triangles
from afar: tools are tested, not timings.
"""
import math
import os
import sys
import tempfile
import time

from gate_common import Editor, describe, frame_stats, slow_frames, speed_failures
from gate_person import write_person
from n2_gate import ALL_ON, behind_text, set_resolution, settle
from n3_gate import (HOUSES_X, HOUSES_Z, SPACING, STOPS, TOWN, build_valley, flight_path, leg_ends, leg_of,
                     wait_streaming)

RESOLUTION = (2560, 1440)
WALKERS = 160              # in the alleys, walking
STANDING = 240             # round the town
# Off a real GPU (CI's null device, which clears every target on the CPU): a smaller crowd, the same checks.
SMALL_WALKERS, SMALL_STANDING = 48, 72
CLOSE_UPS = 4              # by the street, each one entity with a look a part (skin, clothes, eyes)
WALKER_STRANDS = 2000      # of hair each (too far to move: not simulated)
CLOSE_UP_STRANDS = 30000
CHARACTERS_BUDGET_MS = 2.0
CHARACTER_PASSES = ("skin", "character occlusion", "subsurface scattering", "hair", "hair shadows")
CHARACTERS_ON = {"character_occlusion": True, "subsurface_scattering": True}
BLAST_AT = 0.25            # the street leg's share gone when the blast goes off
SHOT_EVERY = 15            # stops between shots in the street
READ_EVERY = 25            # stops between reads of the crowd in the flight
SEND_EVERY = 3             # stops between walkers sent on in the flight, one at a time, each in its turn
SAMPLE_EVERY = 50          # stops between the flight's reads of the frame times (the editor keeps 240 frames)
GET_UP_SECONDS = 2.0       # lying still this long, a ragdoll gets up
FAR_SHARE = 0.02           # of the crowd's triangles drawn from up the river, at most


def crossings():
    """Where the town's alleys cross (x, z), between the houses and round them, but for those on the
    street down its middle: the flight goes down it at head height."""
    xs = [TOWN[0] + (i - HOUSES_X / 2) * SPACING for i in range(HOUSES_X + 1) if i != HOUSES_X // 2]
    zs = [TOWN[1] + (j - HOUSES_Z / 2) * SPACING for j in range(HOUSES_Z + 1)]
    return [(x, z) for x in xs for z in zs]


def walker_plan(count):
    """(start, target) for each of `count` walkers: from one crossing to another well away. Those that
    share a crossing start a little apart round it, so no two start in one spot."""
    spots = crossings()
    stride = 11  # (shares no factor with the 56 crossings: every one is used in turn)
    assert math.gcd(stride, len(spots)) == 1
    plan = []
    for i in range(count):
        first = (i * stride) % len(spots)
        other = (first + 23 + 9 * (i % 5)) % len(spots)
        if math.dist(spots[first], spots[other]) < 25.0:
            other = (first + len(spots) // 2) % len(spots)
        lap = i // len(spots)
        side = ((lap % 3) - 1) * 0.7
        along = (lap // 3 - 0.5) * 0.9
        start = (round(spots[first][0] + side, 2), round(spots[first][1] + along, 2))
        plan.append((start, spots[other]))
    return plan


def close_up_spots():
    """By the street's sides, a few metres apart, about the street leg's middle."""
    return [(TOWN[0] + (1.3 if i % 2 else -1.3), round(TOWN[1] + 9.0 - i * 6.0, 2)) for i in range(CLOSE_UPS)]


def people(tool, whole, first_part, town_h, walking, stood):
    """The crowd: `walking` walkers, `stood` standing, the close-ups. Returns (walker ids, standing ids,
    close-up ids). `first_part`: the name of the person file's first part (the one that takes an
    entity's own look; the others take theirs by name, in `parts`)."""
    plan = walker_plan(walking)
    walkers = tool("create_entities", {"entities": [
        {"name": f"Walker {i}", "shape": "mesh", "mesh": whole, "position": [x, town_h, z], "body": "none",
         "base_color": [0.2 + 0.05 * (i % 7), 0.22 + 0.04 * (i % 5), 0.3 - 0.03 * (i % 4)], "roughness": 0.85,
         "hair": {"on": True, "strands": WALKER_STRANDS, "guides": 16, "length": 0.12, "top": 0.3, "melanin": 0.2 + 0.1 * (i % 8),
                  "moves": False}}
        for i, ((x, z), _) in enumerate(plan)]})["ids"]
    ring = []
    for i in range(stood):
        angle = 2.0 * math.pi * i / stood
        r = 52.0 + 9.0 * (i % 5)
        ring.append([round(TOWN[0] + math.sin(angle) * r, 2), round(TOWN[1] + math.cos(angle) * r, 2)])
    heights = tool("get_terrain", {"points": ring})["heights"]
    standing = tool("create_entities", {"entities": [
        {"name": f"Standing {i}", "shape": "mesh", "mesh": whole, "position": [x, round(h, 3), z], "body": "none",
         "rotation": {"yaw": (i * 47) % 360},
         "base_color": [0.3 - 0.03 * (i % 6), 0.2 + 0.04 * (i % 4), 0.18 + 0.05 * (i % 3)], "roughness": 0.85}
        for i, ((x, z), h) in enumerate(zip(ring, heights))]})["ids"]
    close_ups = []
    for i, (x, z) in enumerate(close_up_spots()):
        # One entity, a look a part: skin, clothes and eyes (the person file's three materials).
        looks = {"skin": {"base_color": [0.78 - 0.1 * i, 0.57 - 0.08 * i, 0.46 - 0.07 * i], "roughness": 0.5,
                          "skin": {"scatter": 0.012}},
                 "clothes": {"base_color": [0.4, 0.12, 0.1], "roughness": 0.85},
                 "eye": {"base_color": [0.9, 0.9, 0.88], "roughness": 0.1,
                         "eye": {"iris_radius": 0.3, "iris_color": [0.1 + 0.1 * i, 0.3, 0.5 - 0.1 * i]}}}
        own = looks.pop(first_part)
        close_ups.append(tool("create_entity", {
            "name": f"Close-up {i}", "shape": "mesh", "mesh": whole, "position": [x, town_h, z], "body": "none",
            **own, "parts": looks,
            # (`top`: the crown of the head, above the eyes, which are part of the one file now.)
            "hair": {"on": True, "strands": CLOSE_UP_STRANDS, "guides": 128, "length": 0.3, "top": 0.3,
                     "melanin": 0.25 + 0.2 * i, "moves": True}})["id"])
    # Motion matched, all of them: one sent walking stands, walks and stands again; the rest stand in their idle.
    for id_ in walkers + standing:
        tool("set_animation", {"id": id_, "clip": "idle", "matched": ["*"]})
    for id_ in close_ups:
        tool("set_animation", {"id": id_, "clip": "idle"})
    return walkers, standing, close_ups


def characters_cost(editor, at, rounds=3):
    """GPU ms of what characters add at a stop (CHARACTER_PASSES), the median
    over `rounds`, with each pass's share. The stop is caught up first.
    Returns (ms or None when the GPU did not time every one of them, the parts, whether it
    caught up, the last frame)."""
    editor.tool("set_camera", {"position": at[0], "look_at": at[1]})
    editor.tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
    streamed = wait_streaming(editor, 6000)[2]
    last, settled = settle(editor, 6000)
    caught_up = settled and streamed
    if not caught_up:
        print(f"characters_gate: behind: streaming {'caught up' if streamed else 'not caught up'}, {behind_text(last)}")
    editor.tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
    totals = []
    for _ in range(rounds):
        editor.hover(10)
        passes = editor.gpu_passes(20)
        if any(name not in passes for name in CHARACTER_PASSES):
            return None, {}, caught_up, editor.last_frame()
        parts = {name: passes[name] for name in CHARACTER_PASSES}
        totals.append((sum(parts.values()), parts))
    totals.sort(key=lambda t: t[0])
    cost, parts = totals[len(totals) // 2]
    return cost, parts, caught_up, editor.last_frame()


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    failures = []
    real_gpu = False
    editor = None
    folder = tempfile.TemporaryDirectory()
    try:
        whole = os.path.join(folder.name, "person.glb").replace("\\", "/")
        person = write_person(whole)
        editor = Editor(sys.argv[1], sys.argv[2:], "characters_gate", timeout=1800)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"characters_gate: {info['adapter']} ({info['backend']})")
        real_gpu = editor.real_gpu()
        if not real_gpu:
            print("characters_gate: warning: not a real GPU, the timings mean nothing")
        width, height = set_resolution(editor, *RESOLUTION)
        print(f"characters_gate: {width}x{height} pixels")
        if real_gpu and (width, height) != RESOLUTION:
            print(f"characters_gate: warning: the budgets are for {RESOLUTION[0]}x{RESOLUTION[1]}")
        town_h = build_valley(tool)
        # The person's file uses three materials: drawn in three parts, each with its look.
        parts = [part["name"] for part in tool("import_mesh", {"path": whole}).get("parts", [])]
        if sorted(parts) != ["clothes", "eye", "skin"]:
            raise RuntimeError(f"the person is not drawn in its three parts (skin, clothes, eye): {parts}")
        first_part = parts[0]
        walkers, standing, close_ups = people(tool, whole, first_part, town_h, *((WALKERS, STANDING) if real_gpu else
                                                                    (SMALL_WALKERS, SMALL_STANDING)))
        crowd = len(walkers) + len(standing) + len(close_ups)

        def ground(points):
            heights = []
            for i in range(0, len(points), 4096):
                heights += tool("get_terrain", {"points": points[i:i + 4096]})["heights"]
            return heights

        path = flight_path(STOPS, ground)
        world_folder = os.path.join(folder.name, "valley")
        saved = tool("save_world", {"folder": world_folder})
        print(f"characters_gate: saved {saved['cells']} cells with {len(walkers)} walkers, {len(standing)} standing and "
              f"{len(close_ups)} close-ups ({person['triangles']} triangles a person); town at {town_h:.1f} m")
        # Opened looking down the street, so the town is there to send its walkers off.
        low_end, street_end = leg_ends(STOPS)
        in_street = path[(low_end + street_end) // 2]
        tool("set_camera", {"position": in_street[0], "look_at": in_street[1]})
        tool("open_world", {"folder": world_folder})
        tool("set_render_settings", {**ALL_ON, **CHARACTERS_ON})
        tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
        world, scatter, streamed = wait_streaming(editor, 6000)
        last, settled = settle(editor, 6000)
        if not (streamed and settled):
            failures.append("loading did not catch up in 6000 frames")
        print(f"characters_gate: loaded: {world['loaded']} cells, {scatter['instances']} scatter instances")
        # Everyone animating and the hair grown before anything is timed.
        tool("get_hair", {"wait": True})

        def wait_animated():
            animation = tool("get_animation")
            for _ in range(600):
                if animation["animated"] >= crowd and animation["waiting"] == 0:
                    break
                editor.hover(10)
                animation = tool("get_animation")
            return animation

        animation = wait_animated()
        if animation["animated"] < crowd or animation["waiting"] or animation["frozen"]:
            failures.append(f"of {crowd} people {animation['animated']} animate, {animation['waiting']} wait, "
                            f"{animation['frozen']} are frozen")
        # From up the river: the crowd by distance. Its triangles are those drawn less those drawn without it.
        tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        streamed = wait_streaming(editor, 6000)[2]
        last, settled = settle(editor, 6000)
        if not (streamed and settled):
            failures.append("the flight's start did not catch up in 6000 frames")

        def triangles_drawn():
            """The median over ten frames (trees sway and what is hidden changes a little frame by frame)."""
            counts = []
            for _ in range(10):
                counts.append(editor.last_frame()["triangles"])
            return sorted(counts)[len(counts) // 2]

        editor.hover(30)
        far = triangles_drawn()
        far_one = tool("get_animation", {"id": standing[0]})
        for id_ in walkers + standing:
            tool("set_entity", {"id": id_, "visible": False})
        editor.hover(30)
        without = triangles_drawn()
        for id_ in walkers + standing:
            tool("set_entity", {"id": id_, "visible": True})
        tool("get_hair", {"wait": True})
        animation = wait_animated()
        if animation["animated"] < crowd:
            failures.append(f"shown again, {animation['animated']} of {crowd} people animate")
        whole_crowd = (len(walkers) + len(standing)) * person["triangles"]
        far_crowd = far - without
        print(f"characters_gate: from up the river: {far_crowd} triangles of the crowd drawn (of its {whole_crowd}; "
              f"{far} in all); one standing is {far_one.get('pixels', 0.0):.0f} pixels, posed every "
              f"{far_one.get('pose_interval', 0.0):.3f} s")
        if real_gpu and not 0 < far_crowd <= FAR_SHARE * whole_crowd:  # (the null device draws none)
            failures.append(f"from up the river {far_crowd} triangles of the crowd are drawn: none, or over "
                            f"{FAR_SHARE:.0%} of its {whole_crowd} (no level of detail)")
        if not far_one["animated"] or far_one["pose_interval"] <= 0.0:
            failures.append(f"far off, one standing is posed every frame (interval {far_one.get('pose_interval')})")
        tool("set_camera", {"position": in_street[0], "look_at": in_street[1]})
        tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
        tool("simulate", {"on": True})
        tool("set_ragdoll_settings", {"get_up_seconds": GET_UP_SECONDS})
        for id_, (_, target) in zip(walkers, walker_plan(len(walkers))):
            tool("walk_to", {"id": id_, "to": [target[0], town_h, target[1]]})
        for _ in range(300):  # their ways found (a few searches a frame)
            if tool("get_walker")["searching"] == 0:
                break
            editor.hover(5)
        sent = tool("get_walker")
        print(f"characters_gate: walkers: {sent['walking']} walking, {sent['searching']} searching, "
              f"{sent['arrived']} arrived, {sent['failed']} failed")
        if sent["failed"] or sent["searching"] or sent["walking"] + sent["arrived"] != len(walkers):
            failures.append(f"of {len(walkers)} walkers sent, {sent['walking']} walk, {sent['arrived']} arrived, "
                            f"{sent['searching']} still search and {sent['failed']} failed")
        editor.hover(60)  # (into their stride)
        walking = [tool("get_animation", {"id": id_}).get("matching", {}).get("clip") for id_ in walkers[:8]]
        if walking.count("walk") < 6:
            failures.append(f"walkers sent walking play {walking}, not their walk")
        stood = [tool("get_animation", {"id": id_}).get("matching", {}).get("clip") for id_ in standing[:4]]
        if stood.count("idle") != len(stood):
            failures.append(f"those standing play {stood}, not their idle")

        knock_at = low_end + int((street_end - low_end) * BLAST_AT)
        overflow = []
        seen = {"walking": 0, "ragdolls": 0, "waiting": 0, "overdue": 0, "posed": 0, "street_walking": 0, "occluders": 0, "skin": False,
                "hair_drawn": 0, "hair_simulated": 0}
        blast = {"walker": None}
        shots = {"tried": 0, "fired": 0, "hit": 0}
        plan = walker_plan(len(walkers))
        sent_on = {"count": 0, "refused": 0}

        def feet_of(id_):
            """Where a walker stands; None for one lying limp (no walker meanwhile: the command refuses)."""
            try:
                return tool("get_walker", {"id": id_})["walker"]["feet"]
            except RuntimeError:
                return None

        def each_stop(stop, frame):
            seen["occluders"] = max(seen["occluders"], frame["occluders"])
            seen["skin"] = seen["skin"] or frame["subsurface_scattering"]
            # Kept walking all flight long: each walker in its turn sent to the other end of its way (one
            # lying limp is refused: it is sent on a later turn, once up).
            if stop % SEND_EVERY == 0:
                turn = sent_on["count"] + sent_on["refused"]
                which = turn % len(walkers)
                end = plan[which][(turn // len(walkers)) % 2]  # (first back to where it started)
                try:
                    tool("walk_to", {"id": walkers[which], "to": [end[0], town_h, end[1]]})
                    sent_on["count"] += 1
                except RuntimeError:
                    sent_on["refused"] += 1
            # (A read every so many stops, a command a frame: the editor keeps its last 240 frame times and
            # the flight reads them every SAMPLE_EVERY stops, so all of these must leave room.)
            if stop % READ_EVERY == 0:
                now_walking = tool("get_walker")["walking"]
                seen["walking"] = max(seen["walking"], now_walking)
                if leg_of(stop, STOPS) == "street":
                    seen["street_walking"] = max(seen["street_walking"], now_walking)
                ragdolls = tool("get_ragdoll")
                seen["ragdolls"] = max(seen["ragdolls"], ragdolls["ragdolls"])
                seen["waiting"] = max(seen["waiting"], ragdolls["waiting"])
                animation = tool("get_animation")
                seen["overdue"] = max(seen["overdue"], animation["overdue"])
                seen["posed"] = max(seen["posed"], animation["posed_last_frame"])
            if stop % (2 * READ_EVERY) == 0 or leg_of(stop, STOPS) == "street" and stop % READ_EVERY == 0:
                hair = tool("get_hair")
                seen["hair_drawn"] = max(seen["hair_drawn"], hair["drawn"])
                seen["hair_simulated"] = max(seen["hair_simulated"], hair["simulated"])
            if stop == knock_at:  # a blast among the walkers
                for id_ in walkers[:3]:
                    feet = feet_of(id_)
                    if feet is not None:
                        blast["walker"] = id_
                        tool("impact", {"point": [feet[0] + 1.5, feet[1] + 0.5, feet[2]], "radius": 7.0,
                                        "impulse": 3000.0})
                        break
            if leg_of(stop, STOPS) == "street" and stop > knock_at and stop % SHOT_EVERY == 0:
                # A heavy pistol round down into a walker from just over its head (nothing between them).
                id_ = walkers[(5 + 13 * shots["tried"]) % len(walkers)]
                shots["tried"] += 1
                feet = feet_of(id_)
                if feet is not None:
                    origin = [feet[0], feet[1] + 2.4, feet[2] + 0.12]
                    aim = [feet[0], feet[1] + 1.3, feet[2]]
                    length = math.dist(origin, aim)
                    fired = tool("fire", {"origin": origin, "direction": [(a - o) / length for a, o in zip(aim, origin)],
                                          "speed": 400.0, "mass": 0.012, "penetration": 0.3, "resolve": True})
                    shots["fired"] += 1
                    if any(h.get("entity") == id_ for h in fired.get("hits", [])):
                        shots["hit"] += 1
            if frame["overflow"] or frame["voxels"]["overflow"] or frame["decal_overflow"] or frame["light_overflow"]:
                overflow.append(stop)

        flown = editor.fly(path, each_stop, marks=(low_end, street_end, *range(SAMPLE_EVERY - 1, STOPS, SAMPLE_EVERY)))
        times = frame_stats(flown)
        slow = slow_frames(flown, editor.slow, editor.typicals, editor.slow_over_ms,
                           leg=lambda at: leg_of(at, STOPS))
        if slow:
            print(f"characters_gate: {slow}")
        print(f"characters_gate: frame time over the flight ({times['frames']} frames sampled): {describe(times)}")
        cuts = [0, editor.marked[low_end], editor.marked[street_end], len(flown)]
        leg_times = {}
        for i, name in enumerate(("low", "street", "climb")):
            leg_times[name] = frame_stats(flown[cuts[i]:cuts[i + 1]])
            print(f"characters_gate: flying {name}: {describe(leg_times[name])}")

        # Those struck down: made ragdolls (none left waiting), and up again after lying still.
        ragdolls = tool("get_ragdoll")
        until = time.monotonic() + 60.0
        while (ragdolls["waiting"] or (ragdolls["dropped"] and not ragdolls["risen"])) and time.monotonic() < until:
            editor.hover(20)
            ragdolls = tool("get_ragdoll")
        walkers_now = tool("get_walker")
        animation = tool("get_animation")
        hair = tool("get_hair")
        print(f"characters_gate: at most {seen['walking']} walking at once in the flight, {seen['street_walking']} in the street ({sent_on['count']} sent on, "
              f"{sent_on['refused']} refused lying limp), {walkers_now['failed']} failed; "
              f"{animation['animated']} animated, at most {seen['posed']} posed in a frame, {seen['overdue']} overdue")
        print(f"characters_gate: the blast and {shots['fired']} shots ({shots['hit']} met their walker) dropped "
              f"{ragdolls['dropped']}, at most {seen['ragdolls']} lying at once, {seen['waiting']} waiting at most; "
              f"{ragdolls['risen']} got up, {ragdolls['ragdolls']} lie yet")
        print(f"characters_gate: hair: {hair['wearing']} wear it, {hair['grown']} grown, {hair['strands']} strands, at most "
              f"{seen['hair_drawn']} drawn and {seen['hair_simulated']} simulated; {hair['left_out']} left out")
        if seen["street_walking"] < len(walkers) // 4:
            failures.append(f"at most {seen['street_walking']} of {len(walkers)} walkers were walking at once while "
                            f"the street was flown ({seen['walking']} in the whole flight)")
        if walkers_now["failed"]:
            failures.append(f"{walkers_now['failed']} walkers failed on their way")
        if animation["frozen"] or animation["animated"] < crowd:
            failures.append(f"at the end {animation['animated']} of {crowd} animate, {animation['frozen']} frozen")
        if blast["walker"] is None:
            failures.append("no walker was left to set the blast by")
        if ragdolls["dropped"] == 0:
            failures.append("the blast and the shots dropped no one")
        if shots["fired"] == 0 or shots["hit"] == 0:
            failures.append(f"of {shots['fired']} shots fired at walkers, {shots['hit']} met theirs")
        if ragdolls["waiting"]:
            failures.append(f"{ragdolls['waiting']} struck down still wait to be made ragdolls")
        if ragdolls["dropped"] and not ragdolls["risen"]:
            failures.append(f"none of the {ragdolls['dropped']} dropped got up again")
        if hair["grown"] < len(walkers) + len(close_ups) or hair["left_out"]:
            failures.append(f"hair: {hair['grown']} grown of {len(walkers) + len(close_ups)}, {hair['left_out']} left out")
        if seen["hair_drawn"] == 0 or seen["hair_simulated"] == 0:
            failures.append(f"hair: at most {seen['hair_drawn']} drawn, {seen['hair_simulated']} simulated")
        if seen["occluders"] == 0:
            failures.append("no body ever shut light out (character occlusion)")
        if not seen["skin"]:
            failures.append("skin never spread its light in the flight")
        if overflow:
            failures.append(f"a list or pool overflowed at stops {overflow[:5]}")

        # A close-up: half a metre from a face, looking at it.
        x, z = close_up_spots()[0]
        face = ([x + 0.1, town_h + 1.72, z + 0.55], [x, town_h + 1.7, z])
        cost, parts, caught_up, near = characters_cost(editor, face)
        near_one = tool("get_animation", {"id": close_ups[0]})
        near_hair = tool("get_hair", {"id": close_ups[0]})["hair"]
        near_entity = tool("get_entity", {"id": close_ups[0]})
        eye = near_entity.get("eye") or near_entity.get("parts", {}).get("eye", {}).get("eye")
        if not caught_up:
            failures.append("streaming, the scatter or the voxel world did not catch up at the close-up")
        # (The frame's flag says a skin look was given and its pass ran, in view or not: so the skin must
        # also fill the view, by its pose sphere's size on screen.)
        if not near["subsurface_scattering"] or near_one.get("pixels", 0.0) < 0.25 * height:
            failures.append(f"close up, skin does not spread its light, or the skin is not in view "
                            f"({near_one.get('pixels', 0.0):.0f} pixels)")
        if near["occluders"] == 0:
            failures.append("close up, no body shuts light out")
        if eye is None:
            failures.append("the close-up's eyes are not eyes")
        if not near_one["animated"] or near_one["pose_interval"] != 0.0:
            failures.append(f"close up, it is posed every {near_one.get('pose_interval')} s, not every frame")
        if not (near_hair["ready"] and near_hair["simulated"] and near_hair["strands"] == CLOSE_UP_STRANDS):
            failures.append(f"close up, the hair: ready {near_hair['ready']}, simulated {near_hair['simulated']}, "
                            f"{near_hair['strands']} strands")
        if cost is None:
            print("characters_gate: the characters' passes at the close-up: not timed")
            if real_gpu:
                failures.append(f"the GPU did not time all of the characters' passes at the close-up ({CHARACTER_PASSES})")
        else:
            shares = ", ".join(f"{name} {ms:.2f}" for name, ms in parts.items())
            print(f"characters_gate: the characters' passes at the close-up: {cost:.2f} ms ({shares})")
            if real_gpu and cost > CHARACTERS_BUDGET_MS:
                failures.append(f"the characters' passes take {cost:.2f} ms at the close-up, over "
                                f"{CHARACTERS_BUDGET_MS} ms ({shares})")
        gpu = tool("get_engine_info").get("gpu", {})
        if gpu.get("passes"):
            passes = ", ".join(f"{p['name']} {p['ms']:.2f}" for p in gpu["passes"])
            print(f"characters_gate: GPU time at the end {gpu['frame_ms']:.2f} ms ({passes} ms)")
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
            print(f"characters_gate: warning: could not remove {folder.name}: {error}")
    for failure in failures:
        print(f"characters_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("characters_gate: PASS" if real_gpu else "characters_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
