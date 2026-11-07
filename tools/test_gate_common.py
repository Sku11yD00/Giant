"""Checks gate_common's summaries (no editor needed): python test_gate_common.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate_common import frame_stats, hitch_limit, slow_frames  # noqa: E402


def frame(ms, cpu, **phases):
    return {"ms": ms, "cpu_ms": cpu, "phases": {k.replace("__", "/"): v for k, v in phases.items()}, "stop": 0}


def main():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    times = [10.0] * 200 + [40.0, 45.0, 20.0]
    check(hitch_limit(times) == 1.5 * (1000.0 / 60.0), "the limit: 1.5x the 60 fps frame")
    check(frame_stats(times)["hitches"] == 2, "two hitches")
    typical = {"render": 8.0, "render/voxels": 2.0, "scatter": 0.5, "end_frame": 1.0, "streaming": 0.0}
    typicals = [(100, typical, 9.0), (100, typical, 9.0)]
    slow = [
        # The renderer's voxels took 30 ms more: render blamed, voxels inside.
        frame(40.0, 39.0, render=38.0, render__voxels=32.0, scatter=0.5, end_frame=1.0),
        # Waiting 30 ms on nothing it did: streaming blamed, a stall shown.
        frame(45.0, 12.0, render=8.0, render__voxels=2.0, streaming=35.0, end_frame=1.0),
        frame(20.0, 19.0, render=18.0),  # under the limit: not a hitch
    ]
    text = slow_frames(times, slow, typicals)
    check(text is not None and text.startswith("2 hitches"), f"two hitches named: {text}")
    check("render 1" in text and "streaming 1" in text, f"each hitch blamed on its phase: {text}")
    check("inside render, most over: render/voxels 1" in text, f"the renderer's part named: {text}")
    check("over 5 ms in 1 of the 2, worst 32.0 ms" in text, f"the stall: wall minus CPU minus waits: {text}")
    # A spike in render outside its sections: render/rest, not a section.
    rest = slow_frames(times, [frame(40.0, 39.0, render=38.0, render__voxels=2.0)], typicals)
    check("inside render, most over: render/rest 1" in rest, f"the rest of render: {rest}")
    # No hitches, or no samples: nothing to say.
    check(slow_frames([10.0] * 50, [frame(20.0, 19.0, render=18.0)], typicals) is None, "no hitches")
    check(slow_frames(times, slow, []) is None, "no samples")
    for f in failures:
        print(f"FAIL: {f}")
    print("test_gate_common: " + ("FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
