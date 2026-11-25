#!/usr/bin/env python3
"""The game gate: a world packaged as a game, and that game played.

    python3 tools/game_gate.py <path to giant_editor> [editor options...]

Run it on a real GPU (the owner's PC), in a visible window (not --hidden,
which idles between frames), and leave the window be. The editor builds
the N3 gate's valley (a 4 km edited valley, a town, a forest and stones
by scatter rules), saves it as a world and packages it as a game
(`package_game`) into a folder of its own. That folder's program is then
run as anyone handed it would run it, from another folder: the main menu
stands, New game is pressed, the loading screen stands while the world
round the player comes in, the player begins at the world's player start
(GAME_START, set with `set_player_start` before the world is saved) and
runs down the valley towards
the town for RUN_FRAMES frames (20 seconds at 60 fps), cells, ground and scatter
streaming in round it. The game is saved, left for the main menu, and
taken up again with Continue (the loading screen again), and the player
runs back the way it came. It passes when:
- the package is whole (the program, game.json, the world) and nothing of
  the chat or a key is in it;
- each run holds 60 fps without micro stutter from the first frame of
  play (as the other gates: average under 16.7 ms, 99th percentile under
  20.8 ms, at most one hitch): the frames are counted from before the
  loading screen went, so what it should have waited for shows;
- each loading screen stood once, ended in time (not by its longest
  wait) with its bar full and nothing it counts still to come, with
  cells of the world in, and took under LOADING_SECONDS;
- the player ran (RUN_METRES or more over the ground: frames are counted, not seconds) and ended on the
  ground, and the game taken up again is where it was saved;
- streaming never failed.
How long each loading took and over how many frames, what was still behind when play began
(the voxel world, its light, the shadows: waited for by the loading
screen while they get done, 30 s at most) and when each thing the
screen waited for was last still to come (where its time went) is printed.
Do not touch the mouse or the keys while it runs: they are the player's.

Before the packaged game is run, the editor's camera (not the game's
player: the editor's own streaming, at play's share of a frame for the
voxel world) also flies down the valley towards the town, low over the
trees, at FLIGHT_SPEEDS (30 and 90 m/s: a car and a jet; by the clock, so
the speed is the same at any frame rate) for FLIGHT_SECONDS each, from a
standstill with everything round it in. Printed for each: the frame
times, how far behind the voxel world fell (its work pending: bricks to
find or build and changes to take in, at most and as the flight ended),
how many bricks it built of those marked in the frames sampled (every
other frame: those the camera moved in; a brick marked that leaves the
window before its turn is never built), the most of a frame it took the
main thread and on what, and how long after stopping
everything had caught up, with what came last. These are measured, not
yet judged, but for one thing on a real GPU: it must catch up within
CATCH_UP_SECONDS of stopping. A flight that cannot be flown is a failure
of its own and the game is still run.

Then two jumps, each to a place over a kilometre from the last, where
nothing of the world is in. The editor's camera is put JUMP_METRES on
down the valley (`set_camera`): a view that jumps brings the world round it in with a
loading screen's shares of each frame (get_engine_info's `arriving`), and
how long that took is printed with what came last. And in the game, taken
up again and run, the player fast travels (`travel_player`) to the far
end of the valley, past what is loaded round it (to a point in the air:
the ground there is not read yet): the loading screen stands a third time
while the world round the new place comes in, and is judged as the other
two; the player is to be there, put down on the ground, and the frames from the first of play on
after it are printed (measured, not yet judged).

On CI (the null device) it checks all but the speed.
"""
import math
import os
import shutil
import sys
import tempfile
import time

from gate_common import Editor, describe, frame_stats, slow_frames, speed_failures, worst_frame
from n2_gate import ALL_ON, behind_text, set_resolution
from n3_gate import TOWN, WEATHER_ON, build_valley

RESOLUTION = (2560, 1440)
SLOWEST_FACTOR = 1.15     # a flight's frames this many times a frame at rest are kept in full (the slowest is told)
RUN_FRAMES = 1200         # of each run
RUN_METRES = 20.0         # the least the player is to cover in a run, over the ground (8 m/s: 1200 frames at 480 fps)
LOADING_SECONDS = 60.0    # the most a loading screen may take on a real GPU
NAME = "Valley"
FLIGHT_SPEEDS = (30.0, 90.0)  # metres a second
FLIGHT_SECONDS = 10.0         # of each flight on a real GPU (the null device: FLIGHT_SECONDS_NULL)
FLIGHT_SECONDS_NULL = 2.0
FLIGHT_FROM = (150.0, 900.0)  # (x, z): up the valley, on the river's bank; flown down -z, to end abreast of the town
FLIGHT_OVER = 12.0            # metres over the ground (over the trees)
FLIGHT_SPACING = 8.0          # metres between the ground heights read along the way
CATCH_UP_SECONDS = 60.0       # the most the world may take to catch up once the camera stops (a real GPU)
SETTLE_SECONDS = 300.0        # the most it may take to come in where a flight starts
GAME_START = (150.0, 700.0)   # (x, z): the world's player start, on the river's bank up the valley
JUMP_METRES = 1200.0          # how far on down -z the editor's camera jumps after the flights (they end at z -300 or before)
TRAVEL_Z = 1750.0             # the game's player fast travels to z this or minus this, whichever is farther:
                              # past what is loaded round it (1536 m), where the ground is not read yet
TRAVEL_X = 1500.0             # (and off to the side: cells stay loaded farther than they are read, round
                              # everywhere the player ran, down the valley's middle)
TRAVEL_UP = 200.0             # the height of the point given (over any ground of the valley: none is known there)
TRAVEL_FRAMES = 600           # of play timed after the travel


def never_in_it(name):
    """Whether a file (by its name, lower case) is one a game must not hold: the chat's key, the
    chat's library and what it brings (as package_game's own never_packaged)."""
    return name == "claude_key.txt" or name.startswith(("giant.chat.", "anthropic.", "microsoft.extensions.ai."))


def package_failures(folder, made):
    """What is wrong with the packaged game's folder (`made`: package_game's answer)."""
    files = []
    for root, _, names in os.walk(folder):
        files += [os.path.relpath(os.path.join(root, name), folder).replace("\\", "/") for name in names]
    failures = []
    for needed in (made["program"], "game.json", "world/world.json"):
        if needed not in files:
            failures.append(f"{needed} is not in the package")
    if not any(name.startswith("world/cells/") for name in files):
        failures.append("the world's cells are not in the package")
    if made["files"] != len(files):
        failures.append(f"package_game says {made['files']} files, the folder holds {len(files)}")
    unwanted = [name for name in files if never_in_it(name.rsplit("/", 1)[-1].lower())]
    if unwanted:
        failures.append(f"files that are never to be in a game: {unwanted[:5]}")
    return failures


def heading(position, to):
    """The yaw (degrees: 0 looks down -z, 90 down -x) from `position` towards the point `to` (x, z)."""
    return math.degrees(math.atan2(position[0] - to[0], position[2] - to[1]))


def flight_at(metres, heights):
    """Where a flight is `metres` along its way down -z from FLIGHT_FROM (never past the last of
    `heights`, the ground every FLIGHT_SPACING metres along it): (position, look_at), FLIGHT_OVER
    over the higher of the two heights it is between, looking 100 m on at the same height."""
    metres = min(max(metres, 0.0), FLIGHT_SPACING * (len(heights) - 1))
    i = min(int(metres / FLIGHT_SPACING), len(heights) - 2)
    y = max(heights[i], heights[i + 1]) + FLIGHT_OVER
    x, z = FLIGHT_FROM[0], FLIGHT_FROM[1] - metres
    return [x, y, z], [x, y, z - 100.0]


def kept_over(usual, at_rest_ms, times):
    """The frame time (ms) over which a flight's frames are kept in full (their phases, for saying
    where the slowest one's time went): SLOWEST_FACTOR times a frame at rest (`at_rest_ms`),
    never under 2 ms; then, once the flight has frames (`times`), never under their 99th
    percentile, so that a flight whose every frame is well over a frame at rest (no vsync) keeps
    one in a hundred, not all of them (kept frames are written out in full inside timed frames);
    and never over `usual` (Editor.begin_frames's). The slowest frame so far is always over it."""
    over = max(2.0, SLOWEST_FACTOR * at_rest_ms)
    if times:
        over = max(over, frame_stats(times)["p99_ms"])
    return min(usual, over)


def flight_text(speed, seconds, times, behind, caught_up, last):
    """A flight's line. `behind`: {"most", "end", "built", "marked", "samples", "rebuilds", "owed", "begun",
    "slowest"} of the voxel world over the flight (its work pending at most and as it ended; built and marked
    summed over the frames sampled; how often every window was built again, too much having
    changed at once; the most instances that waited at once to be compared, a share a frame, with
    what it holds of them; the bricks the main thread began itself, summed over the frames sampled
    (the workers prepare them: none is expected in a flight); the frame sampled in which it took
    the main thread longest, as last_frame's voxels, or None); `caught_up`: seconds after stopping,
    or None if it never did; `last`: what was last still to come (coming's answer)."""
    samples = max(behind["samples"], 1)
    came = ", ".join(f"{key} {value}" for key, value in last.items())
    after = f"not caught up ({came or 'nothing seen coming'})" if caught_up is None else \
        f"caught up {caught_up:.1f} s after stopping" + (f" (last to come: {came})" if came else "")
    slowest = behind.get("slowest")
    took = "" if not slowest else (
        f"; at its slowest it took the main thread {slowest['cpu_ms']:.1f} ms of a frame (the windows following the "
        f"camera {slowest['shift_ms']:.1f}, the scene's changes {slowest['changes_ms']:.1f}, finding bricks "
        f"{slowest['find_ms']:.1f}, bricks {slowest['bricks_ms']:.1f} with {slowest['begun_here']} begun there)")
    return (f"flying at {speed:.0f} m/s for {seconds:.0f} s: {describe(times)}; the voxel world's work pending "
            f"at most {behind['most']} ({behind['end']} as it stopped), {after}; it built "
            f"{behind['built'] / samples:.0f} bricks of {behind['marked'] / samples:.0f} marked a frame sampled, and "
            f"built every window again {behind['rebuilds']} times; changed instances waited their turn to be taken "
            f"in, {behind.get('owed', 0)} at most; the main thread began {behind.get('begun', 0)} bricks itself in "
            f"the frames sampled (about every other one){took}")


def jump_text(metres, seconds, last, arriving):
    """The camera jump's line. `seconds`: until all round the new place was in, or None if it never
    was; `last`: what was last still to come (coming's answer); `arriving`: whether the editor
    said the world was coming in round a view that jumped."""
    came = ", ".join(f"{key} {value}" for key, value in last.items())
    took = f"not in after {CATCH_UP_SECONDS:.0f} s ({came or 'nothing seen coming'})" if seconds is None else \
        f"all round it was in after {seconds:.1f} s" + (f" (last to come: {came})" if came else "")
    return (f"the camera jumped {metres:.0f} m: {took}" +
            ("" if arriving else "; the editor never said the world was arriving"))


def travel_to(at):
    """Where the game's player, at `at` ([x, y, z]), fast travels to: (x, z), the end of the
    valley farther from it, TRAVEL_X from its middle."""
    return TRAVEL_X, -TRAVEL_Z if at[2] > 0 else TRAVEL_Z


def coming(frame, world, scatter, ground_building):
    """What is still coming in, by name (empty: all is calm). `frame`: the last frame's figures
    (the voxel world's work pending, its light waiting, shadow pages behind); `world`: get_world
    (cells being read or waiting, the far ground); `scatter`: get_scatter; `ground_building`: the
    ground meshes being built."""
    far = world["far_ground"]
    counts = {"voxel work": frame["voxels"]["pending"], "light": frame["gi"]["waiting"],
              "shadows": frame["shadows"]["behind"], "cells read": world["reading"],
              "cells waiting": world["waiting"], "scatter": scatter["building"], "ground": ground_building,
              "far ground": far["reading"] + sum(level["building"] for level in far["levels"])}
    return {key: value for key, value in counts.items() if value}


def settled(editor, seconds, clock=time.perf_counter):
    """Waits until nothing has been coming in for five looks running, ten frames apart: just
    after the camera moved, the world round it has yet to start coming. Returns (the seconds
    until the first of those looks, or None if no such run of looks had begun after `seconds`;
    what was last seen still coming)."""
    tool = editor.tool
    start = clock()
    since = None
    looks = 0
    last = {}
    while True:
        editor.hover(10)
        now = clock() - start
        still = coming(editor.last_frame(), tool("get_world"), tool("get_scatter"),
                       tool("get_terrain")["meshes"]["building"])
        if still:
            last = still
            looks = 0
            if now > seconds:
                return None, last
        else:
            since = now if looks == 0 else since
            looks += 1
            if looks >= 5:
                return since, last


def flown(editor, metres_from, speed, seconds, heights, real_gpu):
    """One flight: from a standstill `metres_from` along the way, `speed` m/s by the clock for
    `seconds`. Returns (frame times, the voxel world's `behind`, seconds to catch up or None,
    what was last still to come)."""
    tool = editor.tool
    position, look_at = flight_at(metres_from, heights)
    tool("set_camera", {"position": position, "look_at": look_at})
    # Coming in where it starts: given more time a frame than the flight has (as a loading screen
    # would), then the flight's own share.
    tool("set_render_settings", {"voxel_ms_per_frame": 8.0})
    ready, still = settled(editor, SETTLE_SECONDS)
    tool("set_render_settings", {"voxel_ms_per_frame": 1.0})
    if ready is None:
        raise RuntimeError(f"the world never settled before the flight at {speed:.0f} m/s: still coming, {still}")
    editor.hover(240)  # (the frame times kept hold none of what came before)
    behind = {"most": 0, "end": 0, "built": 0, "marked": 0, "samples": 0, "rebuilds": 0, "owed": 0, "begun": 0,
              "slowest": None}
    rebuilds = editor.last_frame()["voxels"]["rebuilds"]
    times = []
    at_rest = sorted(tool("get_engine_info")["frame_times"]["recent_ms"])  # (hovering)
    recorded = editor.begin_frames()
    usual = editor.slow_over_ms
    if real_gpu and at_rest:
        # Frames well over a frame at rest are kept in full, for saying where the slowest one's time went.
        editor.slow_over_ms = kept_over(usual, at_rest[len(at_rest) // 2], times)
    start = time.perf_counter()
    stop = 0
    while True:
        elapsed = time.perf_counter() - start
        if elapsed >= seconds:
            break
        position, look_at = flight_at(metres_from + speed * elapsed, heights)
        tool("set_camera", {"position": position, "look_at": look_at})
        voxels = editor.last_frame()["voxels"]  # (of the frame the camera moved in)
        behind["most"] = max(behind["most"], voxels["pending"])
        behind["end"] = voxels["pending"]
        behind["built"] += voxels["built"]
        behind["marked"] += voxels["marked"]
        behind["samples"] += 1
        behind["rebuilds"] = voxels["rebuilds"] - rebuilds
        behind["owed"] = max(behind["owed"], voxels["owed"])
        behind["begun"] += voxels["begun_here"]
        if behind["slowest"] is None or voxels["cpu_ms"] > behind["slowest"]["cpu_ms"]:
            behind["slowest"] = voxels
        stop += 1
        if stop % 50 == 0:  # (the editor keeps its last 240 frames: two commands a stop)
            times, recorded = editor.read_frames(times, recorded, stop)
            if real_gpu and at_rest:
                editor.slow_over_ms = kept_over(usual, at_rest[len(at_rest) // 2], times)
    times, recorded = editor.read_frames(times, recorded, stop)
    editor.end_frames(times)
    return (times, behind, *settled(editor, CATCH_UP_SECONDS))


def flights(editor, real_gpu):
    """The flights down the valley (the editor's camera, the valley built). Returns the failures."""
    tool = editor.tool
    seconds = FLIGHT_SECONDS if real_gpu else FLIGHT_SECONDS_NULL
    width, height = set_resolution(editor, *RESOLUTION)
    print(f"game_gate: the flights: the editor's camera at {width} x {height}" +
          ("" if real_gpu else " (not a real GPU: the timings mean nothing)"))
    tool("set_render_settings", {**ALL_ON, **WEATHER_ON})
    way = sum(speed * seconds for speed in FLIGHT_SPEEDS)
    points = [[FLIGHT_FROM[0], FLIGHT_FROM[1] - FLIGHT_SPACING * i] for i in range(int(way / FLIGHT_SPACING) + 2)]
    heights = tool("get_terrain", {"points": points})["heights"]
    if any(height is None for height in heights):
        raise RuntimeError("the flights' way leaves the ground")
    failures = []
    metres = 0.0
    for speed in FLIGHT_SPEEDS:
        times, behind, caught_up, last = flown(editor, metres, speed, seconds, heights, real_gpu)
        metres += speed * seconds
        print(f"game_gate: {flight_text(speed, seconds, frame_stats(times), behind, caught_up, last)}")
        slow = slow_frames(times, editor.slow, editor.typicals, editor.slow_over_ms)
        if slow and real_gpu:
            print(f"game_gate: flying at {speed:.0f} m/s: {slow}")
        worst = worst_frame(editor.slow, editor.typicals)
        if worst and real_gpu:
            print(f"game_gate: flying at {speed:.0f} m/s: {worst}")
        if caught_up is None and real_gpu:  # (by the clock: on the null device it says nothing)
            failures.append(f"flying at {speed:.0f} m/s: the world had not caught up {CATCH_UP_SECONDS:.0f} s "
                            f"after the camera stopped: still coming, {last}")
    # The jump: from where the flights ended to a place nothing is in, at play's share of a frame
    # for the voxel world (the editor gives more itself while the world arrives).
    was = flight_at(metres, heights)[0]
    ground = tool("get_terrain", {"points": [[was[0], was[2] - JUMP_METRES]]})["heights"][0]
    if ground is None:
        return failures + ["the camera's jump leaves the ground"]
    if tool("get_engine_info")["arriving"]:
        failures.append("the editor says the world is arriving before the camera jumped")
    to = [was[0], ground + FLIGHT_OVER, was[2] - JUMP_METRES]
    tool("set_camera", {"position": to, "look_at": [to[0], to[1], to[2] - 100.0]})
    editor.hover(2)
    arriving = tool("get_engine_info")["arriving"]
    in_after, last = settled(editor, CATCH_UP_SECONDS)
    print(f"game_gate: {jump_text(math.dist(was, to), in_after, last, arriving)}")
    if not arriving:
        failures.append("the camera jumped and the editor did not say the world was arriving")
    if in_after is not None:
        editor.hover(30)
        if tool("get_engine_info")["arriving"]:
            failures.append("the world round the camera's jump is in and the editor still says it is arriving")
    elif real_gpu:
        failures.append(f"the world round the camera's jump was not in after {CATCH_UP_SECONDS:.0f} s: still coming, {last}")
    return failures


# What a loading screen counts as still to come (get_menu's loading), for where its time went.
WAITED_FOR = ("game", "cells", "textures", "ground", "scatter", "far_ground", "poses", "bodies",
              "voxels", "light", "shadows", "geometry")


def begun(game, press, recorded):
    """Presses a main menu's button and waits through the loading screen until the game is played.
    Returns (the frame count from just before play began: `recorded` or one read while the
    loading screen still stood, two reads back so that frames passing between a read and the
    next cost none of play's; the seconds it took; get_menu then)."""
    start = time.monotonic()
    while not press():
        if time.monotonic() - start > 60:
            raise RuntimeError("the main menu's button could not be pressed")
    earlier = before = recorded
    first = None  # the frame count at the first read after the button took (a figure to print: a few frames short)
    last = {}  # what the loading screen waited for: the seconds in at which each was last still to come
    while True:
        earlier, before = before, game.tool("get_engine_info")["frame_times"]["recorded"]
        first = before if first is None else first
        menu = game.tool("get_menu")
        for key, value in menu.get("loading", {}).items():
            if key in WAITED_FOR and value:
                last[key] = time.monotonic() - start
        if menu["menu"] == "none" and game.tool("get_player")["playing"]:
            menu["loading_frames"] = before - first
            menu["waited"] = ", ".join(f"{k} {v:.1f} s" for k, v in sorted(last.items(), key=lambda kv: kv[1])) or "nothing"
            return earlier, time.monotonic() - start, menu
        if menu["menu"] == "main" and menu["notice"]:
            raise RuntimeError(f"the game could not be begun: {menu['notice']}")
        if time.monotonic() - start > 300:
            raise RuntimeError(f"the game was not played 300 s after it was asked for: {menu}")


def run(game, recorded, frames):
    """The player running on for `frames` frames, timed from the count `recorded`. Returns the frame times (ms)."""
    times = []
    idle = 0
    while len(times) < frames:
        game.hover(100)
        had = len(times) + len(game.held)
        times, recorded = game.read_frames(times, recorded)
        idle = idle + 1 if len(times) + len(game.held) == had else 0
        if idle >= 20:
            raise RuntimeError("no frames were timed in 2000 commands (was the game's window minimised?)")
    game.end_frames(times)
    return times


def loading_failures(name, menu, world, seconds, shown, real_gpu):
    """What is wrong with a loading that ended (`menu`: get_menu and `world`: get_world as play began)."""
    loading = menu["loading"]
    failures = []
    still = {k: loading[k] for k in ("game", "cells", "textures", "ground", "scatter", "far_ground", "poses") if loading[k]}
    if still:
        failures.append(f"{name}: play began with what the loading screen counts still to come: {still}")
    if world["loaded"] == 0:
        failures.append(f"{name}: play began with no cell of the world in")
    # (What streaming and the ground's meshes say themselves, not the loading screen's own count.)
    if world["reading"] or world["waiting"] or world["ground_building"]:
        failures.append(f"{name}: play began with cells still coming: {world['reading']} being read, "
                        f"{world['waiting']} waiting, {world['ground_building']} ground meshes building")
    if loading["shown"] != shown or loading["on"]:
        failures.append(f"{name}: the loading screen should have stood once more and be gone: {loading}")
    if loading["progress"] != 1:
        failures.append(f"{name}: the loading ended by its longest wait, not with the world in: {loading}")
    if real_gpu and seconds > LOADING_SECONDS:
        failures.append(f"{name}: loading took {seconds:.1f} s (over {LOADING_SECONDS:.0f})")
    return failures


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    failures = []
    real_gpu = False
    editor = game = None
    folder = tempfile.mkdtemp(prefix="giant_game_gate_")
    try:
        # The editor, working in a folder of its own: the valley, saved as a world, packaged as a game.
        work = os.path.join(folder, "work")
        os.makedirs(work)
        editor = Editor(os.path.abspath(sys.argv[1]), sys.argv[2:], "game_gate", timeout=1500, cwd=work)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"game_gate: {info['adapter']} ({info['backend']})")
        build_valley(tool)
        tool("set_weather", {"preset": "fair", "wetness": 0, "snow_cover": 0})
        # Where the game begins: a point over the bank, looking towards the town.
        start_ground = tool("get_terrain", {"points": [list(GAME_START)]})["heights"][0]
        if start_ground is None:
            raise RuntimeError("the valley has no ground at the game's start")
        start_yaw = heading([GAME_START[0], 0.0, GAME_START[1]], TOWN)
        tool("set_player_start", {"position": [GAME_START[0], start_ground + 2.0, GAME_START[1]], "yaw": start_yaw})
        saved = tool("save_world", {"folder": "valley"})
        out = os.path.join(folder, "handed over")
        made = tool("package_game", {"folder": out, "name": NAME})
        print(f"game_gate: saved {saved['cells']} cells; packaged {made['files']} files, "
              f"{made['bytes'] / 1e6:.0f} MB, as {made['program']}" +
              "".join(f"\ngame_gate: note: {note}" for note in made.get("notes", [])))
        try:  # (measurements: one that cannot be made is a failure, and the game is still run)
            failures += flights(editor, editor.real_gpu())
        except Exception as error:
            failures.append(f"the flights: {error!r}")
        editor.close()
        editor = None
        failures += package_failures(out, made)
        shutil.rmtree(work, ignore_errors=True)  # (the game has only its own folder; one left held is no failure)

        # The game, run from somewhere else, its settings in a file of the gate's.
        elsewhere = os.path.join(folder, "elsewhere")
        os.makedirs(elsewhere)
        game = Editor(os.path.join(out, made["program"]),
                      [*sys.argv[2:], "--player-settings", os.path.join(folder, "settings.json")],
                      "game_gate", timeout=800, cwd=elsewhere)
        tool = game.tool
        real_gpu = game.real_gpu()
        if not real_gpu:
            print("game_gate: warning: not a real GPU, the timings mean nothing")
        first = tool("get_menu")
        if first["game"] is not True:
            raise RuntimeError(f"the packaged program does not run as a game: {first}")
        width, height = set_resolution(game, *RESOLUTION)
        print(f"game_gate: {width}x{height} pixels, vsync {'on' if first['settings']['vsync_now'] else 'off'}")
        if real_gpu and (width, height) != RESOLUTION:
            print(f"game_gate: warning: the budgets are for {RESOLUTION[0]}x{RESOLUTION[1]}")
        tool("set_render_settings", {**ALL_ON, **WEATHER_ON})
        tool("set_autosave", {"seconds": 0})  # (Continue is to take up the gate's own save)

        def press(button):
            return lambda: tool("ui_click", {"screen": "menu.main", "id": button})["event"] is not None

        began_yaw = None  # where the player looked as the last run began

        def played(name, button, shown, towards):
            """One run: the game begun by `button`, the player running towards the point `towards`
            (x, z) and on past it. Returns (where it began, where it ended)."""
            nonlocal failures
            recorded = game.begin_frames()
            recorded, seconds, menu = begun(game, press(button), recorded)
            world = dict(tool("get_world"), ground_building=tool("get_terrain")["meshes"]["building"])
            behind = behind_text(game.last_frame())
            nonlocal began_yaw
            began_yaw = tool("get_player")["yaw"]
            at = tool("get_player")["position"]
            tool("set_player_input", {"forward": 1, "sprint": True, "climb": False, "yaw": heading(at, towards), "pitch": 0})
            flown = run(game, recorded, RUN_FRAMES)
            tool("set_player_input", {"forward": 0, "sprint": False})
            # Stood still (a save and the game taken up from it are compared by where it stands).
            for _ in range(30):
                game.hover(10)
                player = tool("get_player")
                if not player["playing"]:
                    raise RuntimeError(f"{name}: play stopped during the run")
                if player["on_ground"] and math.hypot(*player["velocity"]) < 0.05:
                    break
            ended = player["position"]
            went = math.hypot(ended[0] - at[0], ended[2] - at[2])
            ground = tool("get_terrain", {"points": [[ended[0], ended[2]]]})["heights"][0]
            times = frame_stats(flown)
            print(f"game_gate: {name}: loading took {seconds:.1f} s over {menu['loading_frames']} frames; "
                  f"behind when play began: {behind}")
            print(f"game_gate: {name}: the loading screen last waited for: {menu['waited']}")
            print(f"game_gate: {name}: {times['frames']} frames from the first of play: {describe(times)}; "
                  f"the player ran {went:.0f} m, from {at[1]:.1f} m up to {ended[1]:.1f} m (the ground there: "
                  f"{'none' if ground is None else format(ground, '.1f')} m)")
            slow = slow_frames(flown, game.slow, game.typicals, game.slow_over_ms)
            if slow:
                print(f"game_gate: {name}: {slow}")
            failures += loading_failures(name, menu, world, seconds, shown, real_gpu)
            if real_gpu:
                failures += [f"{name}: {f}" for f in speed_failures(times)]
            if went < RUN_METRES:
                failures.append(f"{name}: the player ran {went:.1f} m, under {RUN_METRES:.0f} (from {at} to {ended})")
            if not player["on_ground"] or ground is None or ended[1] < ground - 1.0:
                failures.append(f"{name}: the player did not end on the ground (at {ended}, the ground {ground}, "
                                f"on_ground {player['on_ground']})")
            return at, ended

        start = time.monotonic()
        while tool("get_menu")["menu"] != "main":
            if time.monotonic() - start > 60:
                raise RuntimeError("the main menu never stood")
        began_at, left_at = played("a new game", "new", 1, TOWN)
        if math.hypot(began_at[0] - GAME_START[0], began_at[2] - GAME_START[1]) > 1.0 or abs(began_at[1] - start_ground) > 1.0:
            failures.append(f"a new game began at {began_at}, not on the ground ({start_ground}) at the world's player "
                            f"start {GAME_START}")
        if abs(began_yaw - start_yaw) > 0.01:
            failures.append(f"a new game began looking along {began_yaw}, not the world's player start's {start_yaw}")
        # Saved, left for the main menu, taken up again.
        tool("save_game", {"slot": "gate", "wait": True})
        tool("open_menu", {"menu": "pause"})
        if tool("ui_click", {"screen": "menu.pause", "id": "quit"})["event"] is None:
            raise RuntimeError("the pause menu's Quit could not be pressed")
        start = time.monotonic()
        while tool("get_menu")["menu"] != "main":
            if time.monotonic() - start > 60:
                raise RuntimeError("the pause menu's Quit did not lead to the main menu")
        # (Back the way it came.)
        back_at, _ = played("the game taken up again", "continue", 2, (began_at[0], began_at[2]))
        if math.dist(left_at, back_at) > 1.0:
            failures.append(f"the game taken up again is at {back_at}, not where it was saved ({left_at})")
        # Fast travel: the loading screen a third time, the player on the ground at the new place.
        at = tool("get_player")["position"]
        x, z = travel_to(at)
        unread = tool("get_terrain", {"points": [[x, z]]})["heights"][0] is None

        def travel():
            tool("travel_player", {"position": [x, TRAVEL_UP, z]})
            return True

        recorded = game.begin_frames()
        recorded, seconds, menu = begun(game, travel, recorded)
        world = dict(tool("get_world"), ground_building=tool("get_terrain")["meshes"]["building"])
        behind = behind_text(game.last_frame())
        # (Where it stands as play goes on, before it could have fallen there.)
        player = tool("get_player")
        there = player["position"]
        ground = tool("get_terrain", {"points": [[x, z]]})["heights"][0]
        flown = run(game, recorded, TRAVEL_FRAMES)
        after = tool("get_player")
        times = frame_stats(flown)
        name = "fast travel"
        print(f"game_gate: {name}: {math.dist(at, there):.0f} m, to where the ground was "
              f"{'not read yet' if unread else 'already in'}; loading took {seconds:.1f} s over "
              f"{menu['loading_frames']} frames; behind when play began: {behind}")
        print(f"game_gate: {name}: the loading screen last waited for: {menu['waited']}")
        print(f"game_gate: {name}: {times['frames']} frames from the first of play: {describe(times)}")
        slow = slow_frames(flown, game.slow, game.typicals, game.slow_over_ms)
        if slow:
            print(f"game_gate: {name}: {slow}")
        failures += loading_failures(name, menu, world, seconds, 3, real_gpu)
        if not unread:
            failures.append(f"{name}: the ground at ({x}, {z}) was read already: a travel to ground not read is to be tried")
        if ground is None or math.hypot(there[0] - x, there[2] - z) > 1.0 or abs(there[1] - ground) > 1.0:
            failures.append(f"{name}: the player was not put on the ground where it travelled to ({x}, {z}, the ground "
                            f"{ground}) as play went on: at {there}")
        if not after["on_ground"] or math.dist(after["position"], there) > 1.0:
            failures.append(f"{name}: the player did not stay standing where it was put ({there}): at "
                            f"{after['position']}, on_ground {after['on_ground']}")
        world = tool("get_world")
        print(f"game_gate: {world['loaded']} cells loaded at the end")
        if world["failed"] or world["far_ground"]["failed"]:
            failures.append(f"streaming failed: {world['failed']} cells, {world['far_ground']['failed']} far chunks")
        game.close()
        game = None
    except Exception as error:
        failures.append(str(error))
    finally:
        for running in (editor, game):
            if running is not None:
                running.kill()
                try:  # (gone before its folder is removed: on Windows a running program keeps it)
                    running.process.wait(timeout=30)
                except Exception:
                    pass
        shutil.rmtree(folder, ignore_errors=True)
    for failure in failures:
        print(f"game_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("game_gate: PASS" if real_gpu else "game_gate: PASS (everything but speed: that needs a real GPU)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
