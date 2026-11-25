"""Checks gate_common's summaries (no editor needed): python test_gate_common.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate_common import (Editor, slow_over, frame_stats, held_by_window, hitch_limit, leg_slow_frames,  # noqa: E402
                         slow_frames, without_held, worst_frame)


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
    # The slowest frame of those kept: which phases took more than they typically do, and the rest.
    kept = [frame(11.0, 9.5, render=9.0, render__voxels=2.1, end_frame=1.0),
            frame(14.0, 11.6, render=8.1, render__voxels=4.5, scatter=3.0, end_frame=1.2, streaming=0.2)]
    text = worst_frame(kept, typicals)
    check(text.startswith("the slowest frame, 14.0 ms: ms over a typical frame's, by phase: scatter +2.5;"),
          f"the slowest frame's phases over typical, the most over first (under 0.3 ms left out): {text}")
    check("inside render: voxels +2.5" in text, f"and render's parts: {text}")
    check("its CPU time 11.6 ms (typically 9.0), and 1.2 ms with the main thread neither working nor waiting" in text,
          f"its CPU time and what is unaccounted for (14.0 - 11.6 - end_frame's 1.2): {text}")
    check("waiting on the GPU" not in text, f"no wait stood out: {text}")
    waited = worst_frame([frame(14.0, 9.0, render=8.0, render__voxels=2.0, end_frame=5.0)], typicals)
    check("by phase: end_frame +4.0 (begin_frame, end_frame, tail: waiting on the GPU, the driver or the display);" in waited,
          f"a frame that waited says so, by the phases: {waited}")
    first = worst_frame([frame(15.0, 9.0, render=8.0, tail=6.0, physics=1.0), frame(11.0, 9.0, render=9.0)], typicals)
    check(first.startswith("the slowest frame, 15.0 ms") and "by phase: tail +6.0, physics +1.0 (" in first,
          f"the slowest wherever it is in the list, with phases a typical frame has none of: {first}")
    check("by phase: none stands out;" in worst_frame([frame(9.5, 9.0, render=8.1, end_frame=1.0)], typicals),
          "nothing over: said")
    check(worst_frame([], typicals) is None and worst_frame(kept, []) is None, "nothing kept, or nothing typical: no line")
    ground = worst_frame([dict(frame(17.0, 15.0, render=8.0, render__voxels=2.0, terrain=7.5, terrain__uploads=7.2,
                                     terrain__starts=0.2, end_frame=1.0),
                               device={"created": 6, "allocations": 1, "released": 0, "wait_ms": 0.0})], typicals)
    check("by phase: terrain +7.5;" in ground and "inside terrain: uploads +7.2;" in ground and "inside render" not in ground,
          f"the terrain's parts, as render's (a part under 0.3 ms over left out): {ground}")
    check("in it the GPU device made 6 resources, made new memory 1 times (a resource's own, a heap or a staging chunk) "
          "and freed 0 resources;" in ground, f"what the device made in the frame: {ground}")
    both = worst_frame([dict(frame(20.0, 18.0, render=12.0, render__voxels=6.0, terrain=5.0, terrain__paint=4.0),
                             device={"created": 0, "allocations": 1, "released": 3},
                             typical_device={"created": 2, "allocations": 0, "released": 1})], typicals)
    check("; inside render: voxels +4.0; inside terrain: paint +4.0; in it the GPU device made 0 (typically 2) resources, "
          "made new memory 1 (typically 0) times" in both and "freed 3 (typically 1) resources; its CPU time" in both,
          f"render's parts and terrain's in one line, and the device's counts beside a typical frame's: {both}")
    check("the GPU device" not in text and "the GPU device" not in
          worst_frame([dict(frame(14.0, 9.0, render=8.0), device={"created": 0, "allocations": 0, "released": 0})], typicals),
          "an older editor, or a device that made nothing: not said")

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
    # A phase's part is told only of that phase: terrain's largest is not render's.
    ground_typicals = [(f, dict(t, **{"terrain": 0.5, "terrain/uploads": 0.3}), c) for f, t, c in typicals]
    mixed = slow_frames(times, [frame(52.0, 51.0, render=30.0, render__voxels=10.0, terrain=20.0, terrain__uploads=19.8)],
                        ground_typicals)
    check("inside render, most over: render/rest 1" in mixed and "terrain/uploads" not in mixed,
          f"render blamed: render's own part, though a part of terrain is more over: {mixed}")
    ground_hitch = slow_frames(times, [frame(40.0, 39.0, render=8.0, render__voxels=2.0, terrain=30.0, terrain__uploads=29.0)],
                               ground_typicals)
    check("inside terrain, most over: terrain/uploads 1" in ground_hitch and "inside render" not in ground_hitch,
          f"terrain blamed: its part named: {ground_hitch}")
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
    # A leg uneven by itself is told of by its own frames; one that passes, not at all.
    leg = leg_slow_frames(uneven, [dict(frame(22.0, 21.0, scatter=12.0), stop=5)] * 10 +
                          [dict(frame(22.0, 21.0, render=20.0), stop=150)] * 10, typicals, 20.0, lambda stop: stop < 100)
    check(leg is not None and leg.startswith("10 slow frames") and "each: scatter 10" in leg, f"a leg's own: {leg}")
    check(leg_slow_frames([10.0] * 200, [dict(frame(22.0, 21.0, scatter=12.0), stop=5)], typicals, 20.0,
                          lambda stop: True) is None, "a leg that passes: nothing")
    check(leg_slow_frames([], [], typicals, 20.0, lambda stop: True) is None, "no frames: nothing")
    # Hitches and uneven pacing both: each told, the slow frames without the hitches, by the leg.
    both = [10.0] * 180 + [22.0] * 10 + [40.0, 40.0]
    told = slow_frames(both, [dict(frame(22.0, 21.0, scatter=12.0), stop=5)] * 10 +
                       [dict(frame(40.0, 39.0, render=38.0, render__voxels=32.0), stop=150)] * 2, typicals,
                       slow_over_ms=20.0, leg=lambda stop: "low" if stop < 100 else "street")
    check(told.startswith("2 hitches (over 25.0 ms)") and "\n10 slow frames (over 20.8 ms)" in told, f"both: {told}")
    check("each: scatter 10" in told and "counted with the next): low 10" in told and
          "the next): street 2" in told, f"blamed and by leg: {told}")
    # A frame held in the window's own messages says so.
    with_events = [(100, dict(typical, events=0.1), 9.0)]
    pump = slow_frames(times, [frame(40.0, 2.0, events=38.0), frame(45.0, 44.0, render=43.0)], with_events)
    check("events 1" in pump and "leave the window be" in pump, f"the window's messages named: {pump}")
    check("leave the window be" not in text, "not said when no frame was held there")
    # Held by the window being handled: the editor saw the window handled, most of a long frame in
    # `events`. Such frames are set aside from a flight's times and its slow frames.
    moved = dict(frame(5000.0, 800.0, events=4990.0), window=True, frame=103)
    check(held_by_window(moved), "a moved window's long pump is held")
    check(not held_by_window(dict(moved, window=False)), "not without the window handled")
    check(not held_by_window(dict(frame(5000.0, 800.0, events=100.0, scene=4900.0), window=True)), "not when the work was elsewhere")
    check(not held_by_window(dict(frame(20.0, 2.0, events=19.0), window=True)), "not a short frame")
    check(not held_by_window(frame(5000.0, 800.0, events=4990.0)), "older editors say nothing of the window")
    other = dict(frame(40.0, 39.0, render=38.0), window=True, frame=101)
    flown, held, left = without_held([1.0, 2.0, 3.0, 40.0, 5.0, 5000.0, 7.0], [other, moved], 105)
    check(flown == [1.0, 2.0, 3.0, 40.0, 5.0, 7.0] and held == [5000.0] and left == [other],
          f"the held frame set aside: {flown} {held} {left}")
    check(without_held([1.0, 2.0], [other], 105) == ([1.0, 2.0], [], [other]), "none held: as it was")
    # Not kept that low (a software adapter), or every frame slow: nothing to say.
    check(slow_frames(uneven, [frame(22.0, 21.0, render=20.0)] * 10, typicals) is None, "not kept: nothing")
    check(slow_frames([22.0] * 200, [frame(22.0, 21.0, render=20.0)] * 200, typicals, slow_over_ms=20.0) is None,
          "all slow: nothing stands out")
    # No hitches, or no samples: nothing to say.
    check(slow_frames([10.0] * 50, [frame(20.0, 19.0, render=18.0)], typicals) is None, "no hitches")
    check(slow_frames(times, slow, []) is None, "no samples")
    # Frames read in pieces (Editor.begin_frames, read_frames): each read adds those since the last,
    # a held one set aside, the slow ones named by the stop; frames missed between reads are said.
    class Fake(Editor):
        def __init__(self, replies):  # (no process: the replies get_engine_info gives, in turn)
            self.replies = list(replies)
            self.name = "test"

        def real_gpu(self):
            return False

        def tool(self, name, arguments=None):
            return {"frame_times": self.replies.pop(0)}

    def reply(recorded, recent, slow=()):
        phases = {"frames": len(recent), "slow": list(slow), "typical": {"render": 8.0}, "typical_cpu_ms": 9.0}
        return {"recorded": recorded, "recent_ms": recent, "phases": phases}

    held = dict(frame(900.0, 5.0, events=890.0), window=True, frame=104)
    late = dict(frame(40.0, 39.0, render=38.0), frame=106)
    fake = Fake([{"recorded": 100}, reply(103, [9.0, 1.0, 2.0, 3.0]), reply(107, [3.0, 4.0, 900.0, 40.0, 7.0], [held, late])])
    count = fake.begin_frames()
    check(count == 100 and fake.slow_over_ms == 25.0 and fake.slow == [] and fake.held == [], "begun: nothing yet")
    read, count = fake.read_frames([], count, 1)
    check(read == [1.0, 2.0, 3.0] and count == 103, f"the frames since the count, no older ones: {read}")
    read, count = fake.read_frames(read, count, 2)
    check(read == [1.0, 2.0, 3.0, 4.0, 40.0, 7.0] and count == 107 and fake.held == [900.0], f"a held frame set aside: {read}")
    check([f["stop"] for f in fake.slow] == [2] and len(fake.typicals) == 2, "the slow frame kept, named by its stop")
    try:
        missed = Fake([{"recorded": 100}, reply(500, [1.0] * 240)])
        missed.read_frames([], missed.begin_frames())
        check(False, "frames missed between reads raise")
    except RuntimeError as error:
        check("not timed" in str(error), f"and say so: {error}")
    # GIANT_GATE_SLOW_MS keeps more frames, never fewer; nonsense is said.
    check(slow_over(None, 25.0) == 25.0 and slow_over("", 25.0) == 25.0 and slow_over("0.5", 25.0) == 0.5, "asked lower")
    check(slow_over("90", 25.0) == 25.0, "never higher than usual")
    for wrong in ("fast", "0", "-1", "nan", "inf"):
        try:
            slow_over(wrong, 25.0)
            check(False, f"GIANT_GATE_SLOW_MS={wrong} raises")
        except RuntimeError:
            pass
    for f in failures:
        print(f"FAIL: {f}")
    print("test_gate_common: " + ("FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
