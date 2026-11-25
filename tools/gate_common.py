"""What the performance gates (n1_gate.py, n2_gate.py) share: driving the
editor over MCP, flying the camera along a path and timing the frames.

Gates run on a real GPU (the owner's PC), in a visible window (not
--hidden, which idles between frames). Each stop of a flight is two
commands, a frame each.

A flight with hitches prints where the main thread's time went in them
(their phases, from get_engine_info's phases_since). With
GIANT_GATE_FRAMES set to a file, each flight's frame times, slow frames
and typical frames are added to it as a line of JSON, for a closer look
(GIANT_GATE_SLOW_MS: how slow a frame is kept in full there, in ms).
"""
import json
import os
import subprocess
import threading
import time

FRAME_BUDGET_MS = 1000.0 / 60.0
# No micro stutter: the 99th percentile frame stays near the budget, and
# hitches (frames half again over the target, i.e. at least one missed 60 Hz
# refresh) are at most one per flight, for a stray OS hiccup. The target is
# the median or the budget, whichever is longer, so a 144 Hz screen running
# faster than 60 fps is not judged against its own faster frames.
P99_LIMIT_MS = FRAME_BUDGET_MS * 1.25
HITCH_FACTOR = 1.5
MAX_HITCHES = 1


def frame_stats(times):
    """Average, median, 99th percentile, worst and hitches of frame times (ms)."""
    if not times:
        raise RuntimeError("no frames were timed (was the editor window minimised?)")
    ordered = sorted(times)

    def rank(fraction):
        return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1) + 0.5))]

    median = rank(0.5)
    limit = HITCH_FACTOR * max(median, FRAME_BUDGET_MS)
    return {"frames": len(times), "average_ms": sum(times) / len(times), "median_ms": median,
            "p99_ms": rank(0.99), "worst_ms": ordered[-1],
            "hitches": sum(1 for t in times if t > limit)}


def speed_failures(times):
    """What a flight's frame times (frame_stats) fail of the 60 fps, no stutter targets."""
    failures = []
    if times["average_ms"] > FRAME_BUDGET_MS + 0.3:
        failures.append(f"average frame {times['average_ms']:.2f} ms is over the 60 fps budget")
    if times["p99_ms"] > P99_LIMIT_MS:
        failures.append(f"99th percentile frame {times['p99_ms']:.2f} ms: frame pacing is uneven")
    if times["hitches"] > MAX_HITCHES:
        failures.append(f"{times['hitches']} hitches (frames over {HITCH_FACTOR}x the target): micro stutter")
    return failures


def hitch_limit(times):
    """The frame time over which a frame is a hitch (frame_stats)."""
    ordered = sorted(times)
    median = ordered[min(len(ordered) - 1, int(0.5 * (len(ordered) - 1) + 0.5))]
    return HITCH_FACTOR * max(median, FRAME_BUDGET_MS)


def median(values):
    ordered = sorted(values)
    return ordered[len(ordered) // 2] if ordered else 0.0


def weighted_median(pairs):
    """The median of (weight, value) pairs: a sample's median counts for
    as many frames as it had."""
    ordered = sorted(pairs, key=lambda p: p[1])
    total = sum(w for w, _ in ordered)
    seen = 0
    for weight, value in ordered:
        seen += weight
        if 2 * seen > total:
            return value
    return 0.0


# Waiting on the GPU or the display, not work of the main thread's own.
# (A wait in any other phase, as an upload waited for between frames was,
# shows as stalled too: the phase blamed says where.)
WAIT_PHASES = ("begin_frame", "end_frame", "tail")
# A hitch with this much wall time unaccounted for by CPU work and those
# waits was stalled (not busy).
STALL_MS = 5.0


# A frame the window's own handling held: this long, most of it in the
# `events` phase, with the window itself moved, sized or its focus changed.
HELD_OVER_MS = 25.0


def held_by_window(frame):
    """Whether a slow frame (get_engine_info's phases.slow) was held by the
    desktop handling the window: Windows runs a window's messages itself
    while its title bar is held or it is moved or sized, and the editor's
    loop stands still until it lets go. The editor says the window was
    handled in the frame (`window`), and the `events` phase took most of it."""
    return bool(frame.get("window") and frame["ms"] > HELD_OVER_MS and
                frame["phases"].get("events", 0.0) >= 0.5 * frame["ms"])


def without_held(times, slow, recorded):
    """A flight's frame times so far (`times`, the last of them frame
    `recorded` - 1 as the editor counts them) and a sample's slow frames,
    with those the window's handling held set aside: (the times without
    them, the held frames' times, the slow frames left)."""
    held = [frame for frame in slow if held_by_window(frame)]
    gone = {len(times) - (recorded - frame["frame"]) for frame in held}
    return ([t for i, t in enumerate(times) if i not in gone], [frame["ms"] for frame in held],
            [frame for frame in slow if not held_by_window(frame)])


def slow_frames(times, slow, typicals, slow_over_ms=25.0, leg=None):
    """Where the main thread's time went in a flight's hitches: `times` as
    Editor.fly gives them, `slow` its slow frames' phases and `typicals` each
    sample's typical frame (Editor.fly's self.slow, self.typicals). Lines
    naming the phase most over its typical time in each hitch, and how much
    of a hitch the thread spent neither working nor waiting at a frame's
    begin or end (preempted, blocked on a lock or the disk, or waiting on
    the GPU elsewhere). With the 99th percentile
    over its limit (uneven pacing), the same of the frames over that limit
    that are no hitches, when `slow` holds every frame over it
    (`slow_over_ms`, Editor.fly's self.slow_over_ms) and the typical frame is
    under it (else every frame is slow: nothing stands out). `leg`: a stop's
    part of the flight by name (the frames are counted by it too). None when
    there is neither."""
    if not times or not typicals:
        return None
    hitch_over = hitch_limit(times)
    names = set()
    for _, typical, _ in typicals:
        names.update(typical)
    typical = {n: weighted_median([(f, t.get(n, 0.0)) for f, t, _ in typicals]) for n in names}
    typical["render/rest"] = max(0.0, typical.get("render", 0.0) -
                                 sum(v for n, v in typical.items() if n.startswith("render/")))
    typical_cpu = weighted_median([(f, c) for f, _, c in typicals])

    def phases_of(frame):
        p = dict(frame["phases"])
        p["render/rest"] = max(0.0, p.get("render", 0.0) - sum(v for n, v in p.items() if n.startswith("render/")))
        return p

    top = sorted(n for n in typical if "/" not in n)
    inner = sorted(n for n in typical if "/" in n)

    def counts(table):
        return ", ".join(f"{n} {c}" for n, c in sorted(table.items(), key=lambda x: -x[1]))

    def told(frames, what, limit):
        blamed, inner_blamed, excess, stalled, legs = {}, {}, {n: [] for n in typical}, [], {}
        for frame in frames:
            p = phases_of(frame)
            for n in typical:
                excess[n].append(p.get(n, 0.0) - typical[n])
            worst = max(top, key=lambda n: p.get(n, 0.0) - typical[n])
            blamed[worst] = blamed.get(worst, 0) + 1
            if worst == "render" and inner:
                part = max(inner, key=lambda n: p.get(n, 0.0) - typical[n])
                if p.get(part, 0.0) - typical[part] >= 1.0:
                    inner_blamed[part] = inner_blamed.get(part, 0) + 1
            waits = sum(p.get(n, 0.0) for n in WAIT_PHASES)
            stalled.append(max(0.0, frame["ms"] - frame["cpu_ms"] - waits))
            if leg is not None:
                name = leg(frame["stop"])
                legs[name] = legs.get(name, 0) + 1
        over = sorted(((n, median(v)) for n, v in excess.items() if "/" not in n), key=lambda x: -x[1])
        lines = [f"{len(frames)} {what} (over {limit:.1f} ms); the phase most over its typical time in each: "
                 f"{counts(blamed)}",
                 "  median ms over typical: " + ", ".join(f"{n} +{e:.1f}" for n, e in over if e >= 0.5)]
        if legs:
            lines.append(f"  by the flight's leg (by the stop the frames were read at: the frame of a leg's last "
                         f"read is counted with the next): {counts(legs)}")
        if inner_blamed:
            lines.append(f"  inside render, most over: {counts(inner_blamed)}")
        stalls = sum(1 for t in stalled if t > STALL_MS)
        lines.append(f"  the main thread neither working nor waiting at a frame's begin or end (preempted, blocked on a "
                     f"lock or the disk, or waiting on the GPU elsewhere) over {STALL_MS:.0f} ms in {stalls} of the {len(frames)}, worst {max(stalled):.1f} ms; "
                     f"a typical frame's CPU time {typical_cpu:.1f} ms")
        if any(blamed.get(n) for n in ("begin_frame", "end_frame")):
            lines.append("  (begin_frame or end_frame: waiting on the GPU, the driver or the display, "
                         "not the CPU's own work)")
        if blamed.get("events"):
            lines.append("  (events: the window's own messages. Windows holds them, and the frame with them, "
                         "while the window's title bar is held or it is moved or sized: leave the window be "
                         "for the run)")
        device = device_line(frames)
        if device:
            lines.append(device)
        return lines

    lines = []
    hitches = [f for f in slow if f["ms"] > hitch_over]
    if hitches:
        lines += told(hitches, "hitches", hitch_over)
    if (frame_stats(times)["p99_ms"] > P99_LIMIT_MS and slow_over_ms <= P99_LIMIT_MS and
            median(times) < P99_LIMIT_MS):
        uneven = [f for f in slow if P99_LIMIT_MS < f["ms"] <= hitch_over]
        if uneven:
            lines += told(uneven, "slow frames", P99_LIMIT_MS)
    return "\n".join(lines) if lines else None


def worst_frame(slow, typicals, least_over_ms=0.3):
    """Where the time of the slowest frame among `slow` went (Editor.read_frames's self.slow, with
    self.typicals): its time, each phase at least `least_over_ms` over its typical time (the
    parts of render after render itself), and how long the main thread neither worked nor waited
    at the frame's begin or end. One line; None with no slow frames kept or nothing typical."""
    if not slow or not typicals:
        return None
    frame = max(slow, key=lambda f: f["ms"])
    names = set(frame["phases"])
    for _, typical, _ in typicals:
        names.update(typical)
    typical = {n: weighted_median([(f, t.get(n, 0.0)) for f, t, _ in typicals]) for n in names}
    over = sorted(((n, frame["phases"].get(n, 0.0) - typical[n]) for n in names), key=lambda x: (-x[1], x[0]))
    top = [f"{n} +{e:.1f}" for n, e in over if "/" not in n and e >= least_over_ms]
    inner = [f"{n[len('render/'):]} +{e:.1f}" for n, e in over if n.startswith("render/") and e >= least_over_ms]
    waits = sum(frame["phases"].get(n, 0.0) for n in WAIT_PHASES)
    stalled = max(0.0, frame["ms"] - frame["cpu_ms"] - waits)
    typical_cpu = weighted_median([(f, c) for f, _, c in typicals])
    text = (f"the slowest frame, {frame['ms']:.1f} ms: ms over a typical frame's, by phase: "
            f"{', '.join(top) if top else 'none stands out'}")
    if any(n in WAIT_PHASES and e >= least_over_ms for n, e in over):
        text += " (begin_frame, end_frame, tail: waiting on the GPU, the driver or the display)"
    if inner:
        text += f"; inside render: {', '.join(inner)}"
    text += (f"; its CPU time {frame['cpu_ms']:.1f} ms (typically {typical_cpu:.1f}), and {stalled:.1f} ms with the main "
             f"thread neither working nor waiting at the frame's begin or end")
    return text


def leg_slow_frames(times, slow, typicals, slow_over_ms, in_leg):
    """Where the main thread's time went in the slow frames of one leg of a flight that fails its speed
    targets by itself (slow_frames of the leg's own `times` and of those of `slow` read at a stop
    `in_leg(stop)` holds for): a leg can be uneven while the whole flight is not, and then nothing
    else says why. None when the leg passes, or there is nothing to tell."""
    if not times or not speed_failures(frame_stats(times)):
        return None
    return slow_frames(times, [f for f in slow if in_leg(f["stop"])], typicals, slow_over_ms)


def device_line(hitches):
    """What the GPU device did before the hitches (editors that report it).
    begin_frame waits for the frame two back, so memory made or freed in the
    two frames before a hitch is what the GPU may have been held up by:
    resources created, new memory allocations and resources released over
    those two frames (median over the hitches) beside two typical frames',
    and how long begin_frame waited. None when there is nothing to tell."""
    with_device = [f for f in hitches if f.get("device") is not None]
    if not with_device:
        return None
    typical = [f["typical_device"] for f in with_device if f.get("typical_device")]

    def typical2(key):
        return 2 * median([t.get(key, 0) for t in typical]) if typical else 0.0

    keys = ("allocations", "created", "released")
    before = {k: median([(f.get("device_before") or {}).get(k, 0) for f in with_device]) for k in keys}
    waits = [f["device"].get("wait_ms", 0.0) for f in with_device]
    releases = [f["device"].get("release_ms", 0.0) for f in with_device]
    if not any(before.values()) and not any(waits) and not any(releases):
        return None  # (the null device counts nothing)
    parts = [f"{k} {before[k]:.0f} (typical {typical2(k):.0f})" for k in keys]
    return ("  the GPU device in the two frames before a hitch (median): " + ", ".join(parts) +
            f"; begin_frame waited for the GPU {median(waits):.1f} ms (worst {max(waits):.1f}), "
            f"then freed for {median(releases):.1f} ms (worst {max(releases):.1f})")


def describe(times):
    return (f"average {times['average_ms']:.2f} ms, 99th percentile {times['p99_ms']:.2f} ms, "
            f"worst {times['worst_ms']:.2f} ms, {times['hitches']} hitches")


def slow_over(asked, usual):
    """How slow a frame (ms) is kept in full: `usual`, or what GIANT_GATE_SLOW_MS asks (`asked`)
    when that is less (more frames for a closer look; never fewer, which would change what a gate
    sets aside and says)."""
    if not asked:
        return usual
    try:
        value = float(asked)
    except ValueError:
        raise RuntimeError(f"GIANT_GATE_SLOW_MS is not a number: {asked}") from None
    if not 0.0 < value < float("inf"):
        raise RuntimeError(f"GIANT_GATE_SLOW_MS must be over 0: {asked}")
    return min(value, usual)


class Editor:
    """giant_editor --mcp as a child process, killed after `timeout` seconds."""

    def __init__(self, executable, options, name, timeout=300, cwd=None, env=None):
        self.process = subprocess.Popen([executable, "--mcp", *options], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=None, text=True, encoding="utf-8",
                                        bufsize=1, cwd=cwd, env=env)
        self.timer = threading.Timer(timeout, self.process.kill)
        self.timer.daemon = True  # (never what keeps the script alive)
        self.timer.start()
        self.name = name
        self.next_id = 0
        try:
            self.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                     "clientInfo": {"name": name, "version": "1"}})
        except Exception:
            self.timer.cancel()
            self.process.kill()
            raise

    def call(self, method, params=None):
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError(f"the editor closed the connection during {method}")
        reply = json.loads(line)
        if "error" in reply:
            raise RuntimeError(f"{method} failed: {reply['error']}")
        return reply["result"]

    def tool(self, name, arguments=None):
        result = self.call("tools/call", {"name": name, "arguments": arguments or {}})
        if result.get("isError"):
            raise RuntimeError(f"tool {name} failed: {result['content'][0]['text']}")
        return result.get("structuredContent", {})

    def real_gpu(self):
        info = self.tool("get_engine_info")
        return not info["software_adapter"] and info["backend"] != "null"

    def last_frame(self):
        return self.tool("get_render_settings")["last_frame"]

    def hover(self, frames):
        for _ in range(frames):
            self.call("ping")

    def fly(self, path, each_stop, marks=()):
        """Flies the camera along `path` ((position, look_at) per stop), calling
        each_stop(stop, last_frame) after each; returns the frame times (ms) of
        the whole flight (but for frames the desktop held by handling the window:
        self.held). The editor keeps its last 240: they are read every 100
        stops (two commands a stop, a frame each, plus the read; room for a few
        more each_stop makes). Also read at each stop in `marks`: then
        self.marked[stop] is how many of the times came before it ended."""
        # Hover first so the 240 recorded frame times hold none of what
        # came before (a scene build is one long frame).
        self.tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        self.hover(240)
        # (A camera put far from where it was brings the world in with a loading screen's shares
        # of each frame, `arriving`: the flight is timed at play's, so that is waited out.)
        waited = time.monotonic()
        while self.tool("get_engine_info").get("arriving") and time.monotonic() - waited < 30:
            self.hover(10)
        # The slow frames' phases (self.slow, with the stop sampled after them: frames over the 99th
        # percentile's limit kept in full on a real GPU, uneven pacing explained too; else only
        # those near a hitch), each sample's typical frame (self.typicals: frames, phase medians,
        # CPU ms) and the times (ms) of frames the window's handling held (self.held: not among
        # those returned).
        times = []
        recorded = self.begin_frames()
        for stop, (position, look_at) in enumerate(path):
            self.tool("set_camera", {"position": position, "look_at": look_at})
            each_stop(stop, self.last_frame())
            if stop % 100 == 99 or stop == len(path) - 1 or stop in marks:
                times, recorded = self.read_frames(times, recorded, stop)
                if stop in marks:
                    self.marked[stop] = len(times)
        self.end_frames(times)
        return times

    def begin_frames(self):
        """Before frames are timed (read_frames): nothing slow, typical or held yet. Returns the
        editor's count of frames recorded so far, to read on from."""
        self.marked = {}
        self.slow = []
        self.typicals = []
        self.held = []
        self.slow_over_ms = slow_over(os.environ.get("GIANT_GATE_SLOW_MS"), P99_LIMIT_MS if self.real_gpu() else 25.0)
        return self.tool("get_engine_info")["frame_times"]["recorded"]

    def read_frames(self, times, recorded, stop=0):
        """The frames since the editor's count `recorded` (it keeps its last 240: read at least that
        often) added to `times`, with the slow ones' phases (self.slow, named by `stop`) and the
        sample's typical frame (self.typicals); those the desktop held by handling the window are no
        frames of the run's: set aside (self.held), neither timed nor told among the slow. Returns
        (times, the count to read on from)."""
        now = self.tool("get_engine_info", {"phases_since": recorded, "phases_over_ms": self.slow_over_ms})["frame_times"]
        new = now["recorded"] - recorded
        if new > len(now["recent_ms"]):
            raise RuntimeError(f"{new} frames passed between samples: some were not timed")
        if new > 0:
            times = times + now["recent_ms"][-new:]
        phases = now.get("phases")
        if phases and phases["frames"] > 0:
            times, held, slow = without_held(times, phases["slow"], now["recorded"])
            self.held += held
            self.slow += [dict(frame, stop=stop, typical_device=phases.get("typical_device")) for frame in slow]
            self.typicals.append((phases["frames"], phases["typical"], phases["typical_cpu_ms"]))
        return times, now["recorded"]

    def end_frames(self, times):
        """After the last read_frames: says what was held, and writes the frames out where asked."""
        if self.held:
            print(f"{self.name}: {len(self.held)} frames were held by the window being handled (moved, sized, its "
                  f"title bar held or its focus changed, or the desktop's own doing: {sum(self.held) / 1000.0:.1f} s in all, the longest "
                  f"{max(self.held):.0f} ms): not the program's, not counted. Leave the window be for the run")
        dump = os.environ.get("GIANT_GATE_FRAMES")
        if dump:  # each frame's time and phases, for a closer look
            with open(dump, "a", encoding="utf-8") as f:
                f.write(json.dumps({"times": times, "marked": self.marked, "slow": self.slow,
                                    "typicals": self.typicals, "held": self.held}) + "\n")

    def gpu_passes(self, frames):
        """Median GPU milliseconds of each timed pass over `frames` frames."""
        samples = {}
        for _ in range(frames):
            self.call("ping")
            for p in self.tool("get_engine_info").get("gpu", {}).get("passes", []):
                samples.setdefault(p["name"], []).append(p["ms"])
        return {name: sorted(ms)[len(ms) // 2] for name, ms in samples.items()}

    def close(self):
        try:
            self.tool("quit_editor")
            self.process.wait(timeout=60)
        finally:
            self.kill()

    def kill(self):
        self.timer.cancel()
        if self.process.poll() is None:
            self.process.kill()
