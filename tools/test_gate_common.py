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
    # The device around the hitches: allocations beside a typical frame's.
    typical_device = {"created": 1, "allocations": 0, "released": 1, "wait_ms": 0.5}
    waited = [dict(frame(40.0, 8.0, begin_frame=30.0), typical_device=typical_device,
                   device={"created": 4, "allocations": 3, "released": 2, "wait_ms": 29.0},
                   device_before={"created": 10, "allocations": 9, "released": 6, "wait_ms": 0.0}),
              dict(frame(45.0, 8.0, begin_frame=35.0), typical_device=typical_device,
                   device={"created": 0, "allocations": 0, "released": 0, "wait_ms": 34.0},
                   device_before={"created": 2, "allocations": 2, "released": 1, "wait_ms": 0.0})]
    text = slow_frames(times, waited, typicals)
    check("allocations 9 (typical 0)" in text and "created 10 (typical 2)" in text, f"device counts: {text}")
    quiet = [dict(f, device={"wait_ms": 0.0}, device_before={}) for f in waited]
    check("the GPU device" not in slow_frames(times, quiet, typicals), "nothing counted: no device line")
    check("begin_frame waited for the GPU 34.0 ms (worst 34.0), then freed for 0.0 ms" in text, f"the waits: {text}")
    check("the GPU device" not in slow_frames(times, slow, typicals), "older editors: no device line")
    # No hitches but uneven pacing: the frames over the 99th percentile's limit.
    uneven = [10.0] * 190 + [22.0] * 10
    pacing = slow_frames(uneven, [frame(22.0, 21.0, render=20.0, render__voxels=13.0)] * 10, typicals,
                         slow_over_ms=20.0)
    check(pacing is not None and pacing.startswith("10 slow frames (over 20.8 ms)"), f"slow frames named: {pacing}")
    check("inside render, most over: render/voxels 10" in pacing, f"their part named: {pacing}")
    # Not kept that low (a software adapter), or every frame slow: nothing to say.
    check(slow_frames(uneven, [frame(22.0, 21.0, render=20.0)] * 10, typicals) is None, "not kept: nothing")
    check(slow_frames([22.0] * 200, [frame(22.0, 21.0, render=20.0)] * 200, typicals, slow_over_ms=20.0) is None,
          "all slow: nothing stands out")
    # No hitches, or no samples: nothing to say.
    check(slow_frames([10.0] * 50, [frame(20.0, 19.0, render=18.0)], typicals) is None, "no hitches")
    check(slow_frames(times, slow, []) is None, "no samples")
    for f in failures:
        print(f"FAIL: {f}")
    print("test_gate_common: " + ("FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
