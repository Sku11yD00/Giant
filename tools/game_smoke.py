#!/usr/bin/env python3
"""Runs the program as a game, end to end: a world made in the editor, then
opened with --game. The main menu stands from the start, Continue plays
a game saved outside play, New game plays, the pause menu's Quit leads
back to the main menu, a save is loaded from the saves menu, and the main
menu's Quit ends the program. A world that cannot
be opened ends it at once with an error.

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
        run = Run(["--game", world])
        first = menu()
        check(first["game"] is True, f"it runs as a game: {first}")
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
        # Quit ends the program (its answer may never be read).
        run.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 9999, "method": "tools/call",
                                            "params": {"name": "ui_click", "arguments": {"screen": "menu.main", "id": "quit"}}}) + "\n")
        run.process.stdin.flush()
        check(run.ended() == 0, "Quit ends the program well")

        # A world that is not there: no window to nowhere.
        missing = subprocess.run([sys.argv[1], *sys.argv[2:], "--frames", "5", "--game", os.path.join(folder, "nowhere")],
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
