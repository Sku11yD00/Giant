"""Checks the game gate's own arithmetic (no editor needed): python test_game_gate.py"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from game_gate import (FLIGHT_FROM, FLIGHT_OVER, FLIGHT_SPACING, begun, coming, flight_at, flight_text, heading,  # noqa: E402
                       loading_failures, package_failures, settled)


def main():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # The yaw towards a point: 0 looks down -z, 90 down -x.
    check(abs(heading([0, 0, 0], (0, -10))) < 1e-9, "down -z is yaw 0")
    check(abs(heading([0, 0, 0], (-10, 0)) - 90) < 1e-9, "down -x is yaw 90")
    check(abs(heading([0, 0, 0], (10, 0)) + 90) < 1e-9, "down +x is yaw -90")
    check(abs(abs(heading([5, 0, 5], (5, 20))) - 180) < 1e-9, "down +z is yaw 180")

    # A flight: down -z from where it starts, over the higher of the two heights it is between,
    # looking on level; never past either end of the heights read.
    heights = [1.0, 5.0, 2.0, 2.0]
    position, look_at = flight_at(0.0, heights)
    check(position == [FLIGHT_FROM[0], 5.0 + FLIGHT_OVER, FLIGHT_FROM[1]], f"a flight starts where it is to: {position}")
    check(look_at == [position[0], position[1], position[2] - 100.0], "and looks on down -z, level")
    position, _ = flight_at(FLIGHT_SPACING * 2.5, heights)
    check(position[1] == 2.0 + FLIGHT_OVER and position[2] == FLIGHT_FROM[1] - FLIGHT_SPACING * 2.5, "it is over the ground where it is")
    check(flight_at(1e9, heights)[0][2] == FLIGHT_FROM[1] - FLIGHT_SPACING * 3, "it stops at the last height read")
    check(flight_at(-5.0, heights)[0][2] == FLIGHT_FROM[1], "and never starts before the first")
    times = {"frames": 9, "average_ms": 8.0, "p99_ms": 9.0, "worst_ms": 10.0, "hitches": 0}
    behind = {"most": 400, "end": 120, "built": 900, "marked": 1200, "samples": 3, "rebuilds": 2}
    text = flight_text(90.0, 10.0, times, behind, 1.26, {"scatter": 2})
    check("at 90 m/s for 10 s" in text and "work pending at most 400 (120 as it stopped)" in text, f"a flight's line: {text}")
    check("caught up 1.3 s after stopping (last to come: scatter 2)" in text, f"when it caught up, and what came last: {text}")
    check("built 300 bricks of 400 marked a frame sampled" in text, f"its rates: {text}")
    check("built every window again 2 times" in text, f"its rebuilds: {text}")
    slowest = {"cpu_ms": 3.46, "shift_ms": 0.1, "changes_ms": 2.04, "find_ms": 0.12, "bricks_ms": 1.2, "begun_here": 1}
    text = flight_text(90.0, 10.0, times, dict(behind, slowest=slowest), 1.26, {})
    check("at its slowest it took the main thread 3.5 ms of a frame" in text and "the scene's changes 2.0" in text and
          "bricks 1.2 with 1 begun there" in text, f"its slowest frame, and on what: {text}")
    check("at its slowest" not in flight_text(90.0, 10.0, times, dict(behind, slowest=None), 1.26, {}), "none sampled: not said")
    check("caught up 0.0 s after stopping;" in flight_text(90.0, 10.0, times, behind, 0.0, {}), "nothing came last if nothing came")
    check("not caught up (voxel work 7)" in flight_text(90.0, 10.0, times, dict(behind, samples=0), None, {"voxel work": 7}),
          "one that never caught up says so, and what was still coming")

    def frame(pending=0, light=0, shadows=0):
        return {"voxels": {"pending": pending}, "gi": {"waiting": light}, "shadows": {"behind": shadows}}

    def world(reading=0, waiting=0, far_reading=0, level=0):
        return {"reading": reading, "waiting": waiting,
                "far_ground": {"reading": far_reading, "levels": [{"building": 0}, {"building": level}]}}

    check(coming(frame(), world(), {"building": 0}, 0) == {}, "nothing coming in is calm")
    check(coming(frame(pending=3), world(), {"building": 0}, 0) == {"voxel work": 3}, "the voxel world's work is coming")
    check(coming(frame(light=2, shadows=1), world(), {"building": 0}, 0) == {"light": 2, "shadows": 1}, "its light and the shadows")
    check(coming(frame(), world(reading=1, waiting=2), {"building": 0}, 0) == {"cells read": 1, "cells waiting": 2}, "cells")
    check(coming(frame(), world(far_reading=1, level=2), {"building": 4}, 5) == {"scatter": 4, "ground": 5, "far ground": 3},
          "scatter, the ground and the far ground")

    # settled: five calm looks running; the seconds are to the first of them; what came last is told.
    class Flown:
        def __init__(self, calm_from, busy_again=()):
            self.looks = 0
            self.now = 0.0
            self.calm_from, self.busy_again = calm_from, busy_again

        def hover(self, frames):
            self.looks += 1
            self.now += 1.0

        def clock(self):
            return self.now

        def busy(self):
            return self.looks < self.calm_from or self.looks in self.busy_again

        def last_frame(self):
            return frame(pending=9 if self.busy() else 0)

        def tool(self, name, arguments=None):
            if name == "get_world":
                return world()
            return {"building": 0, "meshes": {"building": 0}}

    editor = Flown(3)
    check(settled(editor, 100.0, editor.clock) == (3.0, {"voxel work": 9}) and editor.looks == 7,
          "settled after five calm looks, timed to the first")
    editor = Flown(1)
    check(settled(editor, 100.0, editor.clock) == (1.0, {}), "calm from the first look: nothing came")
    editor = Flown(2, busy_again=(4,))
    check(settled(editor, 100.0, editor.clock)[0] == 5.0 and editor.looks == 9, "a look that is not calm starts the count again")
    editor = Flown(1000)
    check(settled(editor, 5.0, editor.clock) == (None, {"voxel work": 9}) and editor.looks == 6, "never calm: None once the time is up")
    editor = Flown(4)
    check(settled(editor, 5.0, editor.clock)[0] == 4.0, "a calm run begun in time is waited through past the time")

    # A loading screen: stood once more, gone, its bar full, in time on a real GPU.
    def loading(**changed):
        return {"loading": {"shown": 2, "on": False, "progress": 1.0, "game": False, "cells": 0, "textures": 0,
                            "ground": 0, "scatter": 0, "far_ground": 0, "poses": 0, "voxels": 0, "shadows": 0, "geometry": 0,
                            **changed}}

    good, world = loading(), {"loaded": 9, "reading": 0, "waiting": 0, "ground_building": 0}
    check(len(loading_failures("x", good, dict(world, reading=2), 5.0, 2, True)) == 1, "cells being read as play begins fails")
    check(len(loading_failures("x", good, dict(world, ground_building=1), 5.0, 2, True)) == 1, "ground building then fails")
    check(not loading_failures("x", good, world, 5.0, 2, True), "a good loading passes")
    check(len(loading_failures("x", good, world, 5.0, 1, True)) == 1, "stood twice for one game fails")
    check(len(loading_failures("x", loading(on=True), world, 5.0, 2, True)) == 1, "still standing fails")
    check(len(loading_failures("x", loading(progress=0.7), world, 5.0, 2, True)) == 1, "ended by its longest wait fails")
    check(len(loading_failures("x", loading(cells=3), world, 5.0, 2, True)) == 1, "cells still to come fails")
    check(len(loading_failures("x", loading(scatter=2, poses=1), world, 5.0, 2, True)) == 1, "scatter or poses still to come fails")
    check(len(loading_failures("x", loading(far_ground=4), world, 5.0, 2, True)) == 1, "far ground still to come fails")
    check(not loading_failures("x", loading(shadows=3, voxels=9), world, 5.0, 2, True),
          "the GPU still at it (waited for no longer than its most) passes")
    check(len(loading_failures("x", good, dict(world, loaded=0), 5.0, 2, True)) == 1, "no cell in fails")
    check(len(loading_failures("x", good, world, 500.0, 2, True)) == 1, "too long on a real GPU fails")
    check(not loading_failures("x", good, world, 500.0, 2, False), "how long means nothing without one")

    # begun: the button pressed, the loading waited through; the count given is from two reads back.
    class Game:
        def __init__(self):
            self.frame = 100
            self.calls = []

        def tool(self, name, arguments=None):
            self.frame += 1
            self.calls.append(name)
            if name == "get_engine_info":
                return {"frame_times": {"recorded": self.frame}}
            if name == "get_menu":
                # (Cells still coming until frame 110, the voxel world after them; `on` is no count.)
                return {"menu": "loading" if self.frame < 120 else "none", "notice": "",
                        "loading": {"on": True, "shown": 1, "progress": 0.5, "cells": 3 if self.frame < 110 else 0,
                                    "voxels": 5 if self.frame < 118 else 0, "textures": 0}}
            return {"playing": self.frame >= 120}

    game = Game()
    presses = []
    count, _, menu = begun(game, lambda: presses.append(1) or len(presses) == 3, 100)
    check(len(presses) == 3 and menu["menu"] == "none", "the button is pressed until it takes, the loading waited through")
    waited = [part.split()[0] for part in menu["waited"].split(", ")]
    check(waited == ["cells", "voxels"], f"what the loading screen waited for, in the order it came in: {menu['waited']}")
    reads = [i for i, name in enumerate(game.calls) if name == "get_engine_info"]
    check(100 <= count < 120 and count == 100 + reads[-2] + 1, f"the count is the read before the last: {count}")
    check(menu["loading_frames"] == reads[-1] - reads[0], f"the frames the loading stood, first read to last: {menu['loading_frames']}")

    class Failing(Game):
        def tool(self, name, arguments=None):
            return {"menu": "main", "notice": "no ground"} if name == "get_menu" else super().tool(name, arguments)

    try:
        begun(Failing(), lambda: True, 0)
        check(False, "a game that could not be begun raises")
    except RuntimeError as error:
        check("no ground" in str(error), f"and says why: {error}")

    # The package: the program, game.json and the world in it, the count right, nothing of the chat.
    with tempfile.TemporaryDirectory() as folder:
        for name in ("Valley.exe", "game.json", "world/world.json", "world/cells/0_0.cell.json"):
            os.makedirs(os.path.dirname(os.path.join(folder, name)), exist_ok=True)
            with open(os.path.join(folder, name), "w", encoding="utf-8") as f:
                f.write("x")
        made = {"program": "Valley.exe", "files": 4}
        check(not package_failures(folder, made), f"a whole package passes: {package_failures(folder, made)}")
        check(len(package_failures(folder, {"program": "Other.exe", "files": 4})) == 1, "the program missing fails")
        check(len(package_failures(folder, {"program": "Valley.exe", "files": 5})) == 1, "a wrong count fails")
        # (A name that only holds such a word is no such file.)
        with open(os.path.join(folder, "world", "monkey_chateau.glb"), "w", encoding="utf-8") as f:
            f.write("x")
        check(not package_failures(folder, {"program": "Valley.exe", "files": 5}), "a file named like a key is none")
        os.remove(os.path.join(folder, "world", "monkey_chateau.glb"))
        os.makedirs(os.path.join(folder, "managed"))
        for i, unwanted in enumerate(("Giant.Chat.dll", "claude_key.txt", "Anthropic.dll")):
            with open(os.path.join(folder, "managed", unwanted), "w", encoding="utf-8") as f:
                f.write("x")
            check(any("never" in f for f in package_failures(folder, {"program": "Valley.exe", "files": 5 + i})),
                  f"{unwanted} in it fails")
        for unwanted in ("claude_key.txt", "Anthropic.dll"):
            os.remove(os.path.join(folder, "managed", unwanted))
        os.remove(os.path.join(folder, "game.json"))
        check(any("game.json" in f for f in package_failures(folder, {"program": "Valley.exe", "files": 4})),
              "no game.json in it fails")
        with open(os.path.join(folder, "game.json"), "w", encoding="utf-8") as f:
            f.write("x")
        os.remove(os.path.join(folder, "world", "cells", "0_0.cell.json"))
        check(any("cells" in f for f in package_failures(folder, {"program": "Valley.exe", "files": 4})),
              "no cells in it fails")

    for failure in failures:
        print(f"test_game_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("test_game_gate: ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
