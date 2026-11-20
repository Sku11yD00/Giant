#!/usr/bin/env python3
"""Runs the program as a game, end to end: a world made in the editor, then
opened with --game. The main menu stands from the start, Continue plays
a game saved outside play, New game plays, the pause menu's Quit leads
back to the main menu, a save is loaded from the saves menu, and the main
menu's Quit ends the program. The settings changed in it are kept in a
text file and are there when it is run again. A world that cannot be
opened ends it at once with an error.

Usage: game_smoke.py <giant_editor> [editor options...]
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

from gate_common import Editor
from mcp_smoke import write_bumpy_sphere_glb

TIMEOUT = 300  # seconds for each run


class Run(Editor):
    """The program, driven over MCP."""

    def __init__(self, options, program=None, cwd=None, env=None):
        # (The program by its whole path: it is started from other folders too.)
        super().__init__(program or os.path.abspath(sys.argv[1]), [*sys.argv[2:], *options], "game_smoke", TIMEOUT, cwd, env)

    def until(self, what, done):
        """Polls (each call is a frame of the program) until `done()` is true."""
        end = time.monotonic() + 60
        while time.monotonic() < end:
            if done():
                return
            time.sleep(0.01)
        raise RuntimeError("never happened: " + what)

    def ended(self):
        code = self.process.wait(timeout=TIMEOUT)
        self.timer.cancel()
        return code

    def kill(self):
        self.timer.cancel()
        self.process.kill()


def check(condition, what):
    if not condition:
        raise RuntimeError("check failed: " + what)


def packaged(folder):
    """The editor packages a world as a game; the folder runs as that game from anywhere."""
    runs = []
    try:
        packaged_runs(folder, runs)
    except Exception:
        for run in runs:  # (none left running: on Windows a running program keeps its folder from being removed)
            run.kill()
        raise


def packaged_runs(folder, runs):
    work = os.path.join(folder, "work")
    os.makedirs(os.path.join(work, "assets"))
    write_bumpy_sphere_glb(os.path.join(work, "assets", "ball.glb"))
    with open(os.path.join(work, "assets", "note.txt"), "w", encoding="utf-8") as f:
        f.write("a file a script would load by name")
    out = os.path.join(folder, "handed over")
    scripts = os.environ.get("GIANT_SAMPLE_SCRIPTS", "")

    def refused(run, arguments, saying):
        result = run.call("tools/call", {"name": "package_game", "arguments": arguments})
        text = result["content"][0]["text"] if result.get("isError") else ""
        check(saying in text, f"package_game is refused ({saying}): {result}")
        check(not os.path.exists(arguments["folder"]) or not os.listdir(arguments["folder"]), "and nothing is written")

    # The editor, working in a folder of its own: the world names its mesh from there.
    # (The scripts are named, not loaded here: a script at work in the editor changes the world it is saved from.)
    run = Run([], cwd=work)
    runs.append(run)
    wanted = {"folder": out, "name": "Ball Game", "include": ["./assets/note.txt"]}
    if scripts:
        wanted["scripts"] = scripts
    refused(run, wanted, "open world")
    run.tool("create_entity", {"shape": "plane", "scale": [40, 1, 40]})
    check(run.tool("create_entity", {"mesh": "assets/ball.glb", "position": [0, 2, -6]})["shape"] == "mesh", "the mesh is placed")
    run.tool("save_world", {"folder": "Ball World"})
    run.tool("create_entity", {"shape": "cube"})
    refused(run, wanted, "unsaved")
    run.tool("save_world", {})
    refused(run, {**wanted, "name": "ball/game"}, "name")
    refused(run, {**wanted, "name": "world"}, "name")
    refused(run, {**wanted, "include": ["../work/assets/note.txt"]}, "include")
    run.tool("play", {"on": True})
    refused(run, wanted, "playing")
    run.tool("play", {"on": False})
    made = run.tool("package_game", wanted)
    check(made["game"] == {"name": "Ball Game", "version": "1", "world": "world",
                           "scripts": "scripts/" + os.path.basename(scripts) if scripts else ""},
          f"the game is packaged: {made}")
    refused_again = run.call("tools/call", {"name": "package_game", "arguments": wanted})
    check(refused_again.get("isError") and "not empty" in refused_again["content"][0]["text"],
          f"a folder with a game in it is not written over: {refused_again}")
    run.tool("quit_editor")
    check(run.ended() == 0, "the editor ended well")

    files = set()
    for root, _, names in os.walk(out):
        files.update(os.path.relpath(os.path.join(root, name), out).replace("\\", "/") for name in names)
    program = made["program"]
    check(program in ("Ball Game", "Ball Game.exe") and made["files"] == len(files), f"the program has the game's name: {made}")
    for needed in (program, "game.json", "world/world.json", "assets/ball.glb", "assets/note.txt"):
        check(needed in files, f"{needed} is in the folder: {sorted(files)}")
    check(any(name.startswith("world/cells/") for name in files), "the world's cells are in the folder")
    low = [name.lower() for name in files]
    check(not any("chat" in name or "anthropic" in name or name.endswith(".pdb") or name.endswith(".ini") for name in low),
          f"no chat, symbols or editor's files in the folder: {sorted(files)}")
    if os.name == "nt":
        check("d3d10warp.dll" in files and any(name.startswith("D3D12/") for name in files),
              f"the DirectX 12 runtime and the software renderer are in the folder: {sorted(files)}")
    if scripts:
        check("managed/Giant.Scripting.dll" in files and "scripts/" + os.path.basename(scripts) in files,
              f"the C# API and the scripts are in the folder: {sorted(files)}")
    with open(os.path.join(out, "game.json"), encoding="utf-8") as f:
        check(json.load(f) == {"giant_game": 1, **made["game"]}, "game.json says what the game is")

    # The folder, moved, run from somewhere else: the program is that game, its world and mesh found.
    moved = os.path.join(folder, "on another machine")
    shutil.move(out, moved)
    shutil.rmtree(work)
    elsewhere = os.path.join(folder, "elsewhere")
    os.makedirs(elsewhere)
    # (Its settings in a file named from where it is started: the path is from there, not from the game's folder.)
    run = Run(["--player-settings", "mine.json"], program=os.path.join(moved, program), cwd=elsewhere)
    runs.append(run)
    first = run.tool("get_menu")
    check(first["game"] is True and os.path.normcase(first["settings"]["kept_in"]) == os.path.normcase(os.path.join(elsewhere, "mine.json")),
          f"the packaged program runs as a game: {first}")
    world = run.tool("get_world")
    check(world["open"] and world["folder"] == "world" and not os.path.exists(work), f"its world is the folder's own: {world}")
    meshes = run.tool("list_meshes")["meshes"]
    check(len(meshes) == 1 and meshes[0]["triangles"] > 0, f"the mesh the world names is found from the folder: {meshes}")
    if scripts:
        loaded = run.tool("get_scripts")
        check(os.path.basename(loaded["loaded"]) == os.path.basename(scripts) and loaded["running"] >= 1,
              f"the game's scripts run: {loaded}")
    run.until("the main menu", lambda: run.tool("get_menu")["menu"] == "main")
    check(first["loading"]["shown"] == 0, f"no loading screen before a game is begun: {first}")
    check(run.tool("ui_click", {"screen": "menu.main", "id": "new"})["event"] is not None, "New game is pressed")
    run.until("the game played", lambda: run.tool("get_player")["playing"])
    # (The screen may be gone by the time anything can be asked: that it stood is counted.)
    run.until("the loading screen gone", lambda: run.tool("get_menu")["menu"] == "none")
    played = run.tool("get_menu")
    check(played["loading"]["shown"] == 1 and played["loading"]["on"] is False and played["loading"]["progress"] == 1,
          f"the game was begun behind the loading screen, which is gone in play: {played}")
    run.tool("quit_editor")
    check(run.ended() == 0, "the packaged game ended well")
    check(set(os.listdir(elsewhere)) <= {"mine.json"}, f"nothing but its settings is written where it was started from: {os.listdir(elsewhere)}")
    check(not os.path.exists(os.path.join(moved, "mine.json")) and not os.path.exists(os.path.join(moved, "giant_editor_layout.ini")),
          f"no settings or editor's layout in the game's folder: {os.listdir(moved)}")

    # With no file named, its settings are kept under the game's own name in the person's settings folder
    # (here a folder of the test's, not this machine's).
    home = os.path.join(folder, "home")
    env = {**os.environ, "APPDATA": home, "XDG_CONFIG_HOME": home}
    run = Run([], program=os.path.join(moved, program), cwd=elsewhere, env=env)
    runs.append(run)
    kept_in = run.tool("get_menu")["settings"]["kept_in"]
    check(os.path.normcase(os.path.normpath(kept_in)) ==
          os.path.normcase(os.path.join(home, "Giant", "games", "Ball Game", "game_settings.json")),
          f"a packaged game keeps its settings under its name: {kept_in}")
    run.tool("quit_editor")
    check(run.ended() == 0, "the packaged game ended well")
    # Told to be another game, it is that one (here one that is not there).
    other = subprocess.run([os.path.join(moved, program), *sys.argv[2:], "--frames", "5", "--game", "nowhere"], cwd=elsewhere, env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=TIMEOUT, check=False)
    check(other.returncode == 1, f"--game is taken before game.json: {other.returncode}")

    # A game's game.json with something wrong: the program says so and ends, not an editor in a game's folder.
    # Some other program's game.json (not marked a game's) is let be: the editor, as ever.
    for text, code in (('{"giant_game": 1, "name": "../x"}', 1), ('{"name": "another program\'s"}', 0)):
        with open(os.path.join(moved, "game.json"), "w", encoding="utf-8") as f:
            f.write(text)
        ended = subprocess.run([os.path.join(moved, program), *sys.argv[2:], "--frames", "5"], cwd=elsewhere, env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=TIMEOUT, check=False)
        check(ended.returncode == code, f"with the game.json {text} the program ends with {code}: {ended.returncode}")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    folder = tempfile.mkdtemp(prefix="giant_game_")
    world = os.path.join(folder, "Smoke Valley")
    # (The person's settings: in a file of the test's, not this machine's own settings folder.)
    settings = os.path.join(folder, "player", "game_settings.json")
    as_game = ["--game", world, "--player-settings", settings]
    run = None
    try:
        # The editor makes a world (a floor to stand on) and saves a game of it, not playing.
        run = Run([])
        run.tool("create_entity", {"shape": "plane", "scale": [40, 1, 40]})
        run.tool("save_world", {"folder": world})
        run.tool("save_game", {"slot": "made-in-the-editor", "wait": True})
        check(run.tool("get_menu")["game"] is False, "the editor is no game")
        run.tool("quit_editor")
        check(run.ended() == 0, "the editor ended well")

        def menu():
            return run.tool("get_menu")

        def playing():
            return run.tool("get_player")["playing"]

        def press(screen, button):
            return run.tool("ui_click", {"screen": screen, "id": button})["event"] is not None

        def to_the_menu():
            run.tool("open_menu", {"menu": "pause"})
            check(press("menu.pause", "quit"), "Quit is pressed")
            run.until("the main menu", lambda: menu()["menu"] == "main")
            check(playing() is False, "play is over")

        # The same program as a game of that world: the main menu from the start.
        run = Run(as_game)
        first = menu()
        check(first["game"] is True, f"it runs as a game: {first}")
        check(first["settings"]["vsync_now"] is True and first["settings"]["fullscreen_now"] is False,
              f"its frames wait for the display, in a window: {first['settings']}")
        check(first["settings"]["kept_in"] == settings and first["settings"]["fov"] == 70.0 and not os.path.exists(settings),
              f"its settings are the defaults, kept in the file named, which is not there yet: {first['settings']}")
        run.until("the main menu", lambda: menu()["menu"] == "main")
        check(playing() is False, "nothing is played before a game is begun")
        # Continue: the game saved in the editor, outside play, is played.
        run.until("Continue", lambda: press("menu.main", "continue"))
        run.until("the saved game played", playing)
        check(menu()["menu"] == "none" and menu()["notice"] == "", "no menu over the game, nothing went wrong")
        to_the_menu()
        # New game plays; a game saved in it; Quit leads to the main menu, and the program goes on.
        check(press("menu.main", "new"), "New game is pressed")
        run.until("play", playing)
        run.tool("save_game", {"slot": "save-1", "wait": True})
        to_the_menu()
        # Load: the saves menu from the main menu, the newest first.
        check(press("menu.main", "load"), "Load is pressed")
        run.until("the saves read", lambda: [s["name"] for s in menu()["slots"]][:1] == ["save-1"])
        check(press("menu.saves", "slot.0"), "the save is pressed")
        run.until("the loaded game played", playing)
        to_the_menu()
        # A setting changed is written to the file.
        changed = run.tool("set_game_settings", {"fov": 90, "look_sensitivity": 2, "vsync": False})["settings"]
        check(changed["fov"] == 90.0 and changed["vsync"] is False, f"the settings are changed: {changed}")
        run.until("the settings written", lambda: os.path.exists(settings) and menu() is not None)
        # Quit ends the program (its answer may never be read).
        run.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 9999, "method": "tools/call",
                                            "params": {"name": "ui_click", "arguments": {"screen": "menu.main", "id": "quit"}}}) + "\n")
        run.process.stdin.flush()
        check(run.ended() == 0, "Quit ends the program well")
        with open(settings, encoding="utf-8") as f:
            kept = json.load(f)
        check(kept["fov"] == 90 and kept["look_sensitivity"] == 2 and kept["vsync"] is False and kept["fullscreen"] is False and
              kept["window"][0] >= 320 and kept["window"][1] >= 240, f"the file holds them as text, the window's size too: {kept}")

        # Run again, they are as they were left; a file a person broke gives the defaults and is not the end of it.
        run = Run(as_game)
        again = menu()["settings"]
        check(again["fov"] == 90.0 and again["look_sensitivity"] == 2.0 and again["vsync"] is False and
              again["window"] == kept["window"], f"run again, the settings are as they were left: {again}")
        check(again["vsync_now"] is False, f"vsync off is taken up by the run after: {again}")
        # Fullscreen is taken at once, and the run after starts so.
        full = run.tool("set_game_settings", {"fullscreen": True, "vsync": True})["settings"]
        check(full["fullscreen"] is True and full["fullscreen_now"] is True and full["vsync_now"] is False,
              f"the window fills its screen at once; vsync waits for the next start: {full}")
        run.tool("quit_editor")
        check(run.ended() == 0, "the game ended well")
        run = Run(as_game)
        again = menu()["settings"]
        check(again["fullscreen_now"] is True and again["vsync_now"] is True and again["window"] == kept["window"],
              f"run again it starts fullscreen, its window's size kept for later: {again}")
        run.tool("quit_editor")
        check(run.ended() == 0, "the game ended well")
        # A file a person broke gives the defaults, is set aside, and is whole again after the run.
        broken = '{"fov": "wide", "look_sensitivity": 3'
        with open(settings, "w", encoding="utf-8") as f:
            f.write(broken)
        run = Run(as_game)
        check(menu()["settings"]["fov"] == 70.0, f"a settings file that cannot be read gives the defaults: {menu()['settings']}")
        run.tool("quit_editor")
        check(run.ended() == 0, "the game ended well")
        with open(settings + ".bad", encoding="utf-8") as f:
            check(f.read() == broken, "what the person wrote is set aside beside it")
        with open(settings, encoding="utf-8") as f:
            check(json.load(f)["fov"] == 70, "the file is whole again")
        # The option is a game's.
        alone = subprocess.run([sys.argv[1], *sys.argv[2:], "--frames", "5", "--player-settings", settings],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=TIMEOUT, check=False)
        check(alone.returncode != 0, f"--player-settings without --game is refused: {alone.returncode}")

        run = None
        packaged(folder)

        # A world that is not there: no window to nowhere.
        missing = subprocess.run([sys.argv[1], *sys.argv[2:], "--frames", "5", "--game", os.path.join(folder, "nowhere"),
                                  "--player-settings", settings],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=TIMEOUT, check=False)
        check(missing.returncode == 1, f"a world that cannot be opened ends the program with an error: {missing.returncode}")
    except Exception as error:  # report and fail
        print(f"game_smoke: {error}", file=sys.stderr)
        if run is not None:
            run.kill()
        return 1
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    print("game_smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
