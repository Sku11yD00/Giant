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

TIMEOUT = 300  # seconds for each run


class Run(Editor):
    """The program, driven over MCP."""

    def __init__(self, options):
        super().__init__(sys.argv[1], [*sys.argv[2:], *options], "game_smoke", TIMEOUT)

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
