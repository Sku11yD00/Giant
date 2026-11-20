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
round the player comes in, and the player runs down the valley towards
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
How long each loading took and what was still behind when play began
(the voxel world, its light, the shadows: not waited for by the loading
screen yet, so the first run may show them as slow frames) is printed.
Do not touch the mouse or the keys while it runs: they are the player's.

On CI (the null device) it checks all but the speed.
"""
import math
import os
import shutil
import sys
import tempfile
import time

from gate_common import Editor, describe, frame_stats, slow_frames, speed_failures
from n2_gate import ALL_ON, behind_text, set_resolution
from n3_gate import TOWN, WEATHER_ON, build_valley

RESOLUTION = (2560, 1440)
RUN_FRAMES = 1200         # of each run
RUN_METRES = 20.0         # the least the player is to cover in a run, over the ground (8 m/s: 1200 frames at 480 fps)
LOADING_SECONDS = 60.0    # the most a loading screen may take on a real GPU
NAME = "Valley"


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
    while True:
        earlier, before = before, game.tool("get_engine_info")["frame_times"]["recorded"]
        menu = game.tool("get_menu")
        if menu["menu"] == "none" and game.tool("get_player")["playing"]:
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
    still = {k: loading[k] for k in ("game", "cells", "textures", "ground") if loading[k]}
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
        editor = Editor(os.path.abspath(sys.argv[1]), sys.argv[2:], "game_gate", timeout=800, cwd=work)
        tool = editor.tool
        info = tool("get_engine_info")
        print(f"game_gate: {info['adapter']} ({info['backend']})")
        build_valley(tool)
        tool("set_weather", {"preset": "fair", "wetness": 0, "snow_cover": 0})
        saved = tool("save_world", {"folder": "valley"})
        out = os.path.join(folder, "handed over")
        made = tool("package_game", {"folder": out, "name": NAME})
        print(f"game_gate: saved {saved['cells']} cells; packaged {made['files']} files, "
              f"{made['bytes'] / 1e6:.0f} MB, as {made['program']}" +
              "".join(f"\ngame_gate: note: {note}" for note in made.get("notes", [])))
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

        def played(name, button, shown, towards):
            """One run: the game begun by `button`, the player running towards the point `towards`
            (x, z) and on past it. Returns (where it began, where it ended)."""
            nonlocal failures
            recorded = game.begin_frames()
            recorded, seconds, menu = begun(game, press(button), recorded)
            world = dict(tool("get_world"), ground_building=tool("get_terrain")["meshes"]["building"])
            behind = behind_text(game.last_frame())
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
            print(f"game_gate: {name}: loading took {seconds:.1f} s; behind when play began: {behind}")
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
        # Saved, left for the main menu, taken up again.
        tool("save_game", {"slot": "gate", "wait": True})
        tool("open_menu", {"menu": "pause"})
        if tool("ui_click", {"screen": "menu.pause", "id": "quit"})["event"] is None:
            raise RuntimeError("the pause menu's Quit could not be pressed")
        start = time.monotonic()
        while tool("get_menu")["menu"] != "main":
            if time.monotonic() - start > 60:
                raise RuntimeError("the pause menu's Quit did not lead to the main menu")
        # (Back the way it came: on towards the town it would soon stand before a house.)
        back_at, _ = played("the game taken up again", "continue", 2, (began_at[0], began_at[2]))
        if math.dist(left_at, back_at) > 1.0:
            failures.append(f"the game taken up again is at {back_at}, not where it was saved ({left_at})")
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
