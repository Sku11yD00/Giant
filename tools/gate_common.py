"""What the performance gates (n1_gate.py, n2_gate.py) share: driving the
editor over MCP, flying the camera along a path and timing the frames.

Gates run on a real GPU (the owner's PC), in a visible window (not
--hidden, which idles between frames). Each stop of a flight is two
commands, a frame each.

A flight with hitches prints where the main thread's time went in them
(their phases, from get_engine_info's phases_since). With
GIANT_GATE_FRAMES set to a file, each flight's frame times, slow frames
and typical frames are added to it as a line of JSON, for a closer look.
"""
import json
import os
import subprocess
import threading

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


def _median(values):
    ordered = sorted(values)
    return ordered[len(ordered) // 2] if ordered else 0.0


# Waiting on the GPU or the display, not work of the main thread's own.
WAIT_PHASES = ("begin_frame", "end_frame", "tail")
# A hitch with this much wall time unaccounted for by CPU work and those
# waits was stalled (not busy).
STALL_MS = 5.0


def slow_frames(times, slow, typicals):
    """Where the main thread's time went in a flight's hitches: `times` as
    Editor.fly gives them, `slow` its slow frames' phases and `typicals` each
    sample's typical frame (Editor.fly's self.slow, self.typicals). Lines
    naming the phase most over its typical time in each hitch, and how much
    of a hitch the thread spent neither working nor waiting on the GPU
    (preempted, or blocked on a lock or the disk); None when there were no
    hitches."""
    if not times or not typicals:
        return None
    limit = hitch_limit(times)
    hitches = [f for f in slow if f["ms"] > limit]
    if not hitches:
        return None
    names = set()
    for _, typical, _ in typicals:
        names.update(typical)
    typical = {n: _median([t.get(n, 0.0) for _, t, _ in typicals]) for n in names}
    typical["render/rest"] = max(0.0, typical.get("render", 0.0) -
                                 sum(v for n, v in typical.items() if n.startswith("render/")))
    typical_cpu = _median([c for _, _, c in typicals])

    def phases_of(frame):
        p = dict(frame["phases"])
        p["render/rest"] = max(0.0, p.get("render", 0.0) - sum(v for n, v in p.items() if n.startswith("render/")))
        return p

    top = sorted(n for n in typical if "/" not in n)
    inner = sorted(n for n in typical if "/" in n)
    blamed, inner_blamed, excess, stalled = {}, {}, {n: [] for n in typical}, []
    for frame in hitches:
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

    def counts(table):
        return ", ".join(f"{n} {c}" for n, c in sorted(table.items(), key=lambda x: -x[1]))

    over = sorted(((n, _median(v)) for n, v in excess.items() if "/" not in n), key=lambda x: -x[1])
    lines = [f"{len(hitches)} hitches (over {limit:.1f} ms); the phase most over its typical time in each: "
             f"{counts(blamed)}",
             "  median ms over typical: " + ", ".join(f"{n} +{e:.1f}" for n, e in over if e >= 0.5)]
    if inner_blamed:
        lines.append(f"  inside render, most over: {counts(inner_blamed)}")
    stalls = sum(1 for t in stalled if t > STALL_MS)
    lines.append(f"  the main thread neither working nor waiting on the GPU (preempted, or blocked on a lock or "
                 f"the disk) over {STALL_MS:.0f} ms in {stalls} of the {len(hitches)}, worst {max(stalled):.1f} ms; "
                 f"a typical frame's CPU time {typical_cpu:.1f} ms")
    if any(blamed.get(n) for n in ("begin_frame", "end_frame")):
        lines.append("  (begin_frame or end_frame: waiting on the GPU, the driver or the display, "
                     "not the CPU's own work)")
    return "\n".join(lines)


def describe(times):
    return (f"average {times['average_ms']:.2f} ms, 99th percentile {times['p99_ms']:.2f} ms, "
            f"worst {times['worst_ms']:.2f} ms, {times['hitches']} hitches")


class Editor:
    """giant_editor --mcp as a child process, killed after `timeout` seconds."""

    def __init__(self, executable, options, name, timeout=300):
        self.process = subprocess.Popen([executable, "--mcp", *options], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=None, text=True, encoding="utf-8",
                                        bufsize=1)
        self.timer = threading.Timer(timeout, self.process.kill)
        self.timer.start()
        self.next_id = 0
        self.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                 "clientInfo": {"name": name, "version": "1"}})

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
        the whole flight. The editor keeps its last 240: they are read every 100
        stops (two commands a stop, a frame each, plus the read; room for a few
        more each_stop makes). Also read at each stop in `marks`: then
        self.marked[stop] is how many of the times came before it ended."""
        self.marked = {}
        # The slow frames' phases (with the stop sampled after them) and
        # each sample's typical frame: (frames, phase medians, CPU ms).
        self.slow = []
        self.typicals = []
        # Hover first so the 240 recorded frame times hold none of what
        # came before (a scene build is one long frame).
        self.tool("set_camera", {"position": path[0][0], "look_at": path[0][1]})
        self.hover(240)
        times = []
        recorded = self.tool("get_engine_info")["frame_times"]["recorded"]
        for stop, (position, look_at) in enumerate(path):
            self.tool("set_camera", {"position": position, "look_at": look_at})
            each_stop(stop, self.last_frame())
            if stop % 100 == 99 or stop == len(path) - 1 or stop in marks:
                now = self.tool("get_engine_info", {"phases_since": recorded})["frame_times"]
                new = now["recorded"] - recorded
                recorded = now["recorded"]
                if new > len(now["recent_ms"]):
                    raise RuntimeError(f"{new} frames passed between samples: some were not timed")
                if new > 0:
                    times += now["recent_ms"][-new:]
                phases = now.get("phases")
                if phases and phases["frames"] > 0:
                    self.slow += [dict(frame, stop=stop) for frame in phases["slow"]]
                    self.typicals.append((phases["frames"], phases["typical"], phases["typical_cpu_ms"]))
                if stop in marks:
                    self.marked[stop] = len(times)
        dump = os.environ.get("GIANT_GATE_FRAMES")
        if dump:  # each frame's time and phases, for a closer look
            with open(dump, "a", encoding="utf-8") as f:
                f.write(json.dumps({"times": times, "marked": self.marked, "slow": self.slow,
                                    "typicals": self.typicals}) + "\n")
        return times

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
