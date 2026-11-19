"""Checks the crowd gate's own arithmetic (no editor needed): python test_crowd_gate.py"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crowd_gate import (APPROACH_FROM, END_RADIUS, FIGURE_DISTANCE, FIGURE_KEEP, HALF_X, HALF_Z, KEEP_SIDE, LANES, LOOKS,  # noqa: E402
                        SMALL_STANDING, SMALL_STOPS, SMALL_WALKING, SQUARE, STANDING, STATES, STOPS, WALK_SPEED, WALKING,
                        added_cost, behaviours, crowd_failures, flight_path, lane_ends, lane_x, leg_ends, leg_of,
                        shadow_failures, standing_spots, walkers)
from n3_gate import TOWN, TOWN_RADIUS, town_houses  # noqa: E402


def bearing(stop):
    """The way the camera looks at a stop of a path, degrees round the upright."""
    (x, _, z), (lx, _, lz) = stop
    return math.degrees(math.atan2(lx - x, -(lz - z)))


def main():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # The square: within the town's site (its outer part eases to the valley's ground: everyone is put
    # on the ground where it stands), clear of its houses.
    corner = math.hypot(abs(SQUARE[0] - TOWN[0]) + HALF_X, HALF_Z)
    check(corner < TOWN_RADIUS, f"the square's corners ({corner:.0f} m from the town's middle) are within its site")
    check(all(abs(x - SQUARE[0]) > HALF_X + 2.0 + 2.0 for x, _ in town_houses()), "the square is clear of the town's houses")
    lanes = [lane_x(lane) for lane in range(LANES)]
    ends = {name: (x, z) for name, x, z in lane_ends()}
    ways = behaviours()
    for standing, walking in ((STANDING, WALKING), (SMALL_STANDING, SMALL_WALKING), (37, 33)):
        # Those standing: in the square, each its own spot, clear of the lanes' walkers, and all over it however few.
        stood = standing_spots(standing)
        check(len(stood) == standing and len({(x, z) for x, z, _ in stood}) == standing, f"{standing} stand, each on its own spot")
        check(all(abs(x - SQUARE[0]) <= HALF_X and abs(z - SQUARE[1]) <= HALF_Z for x, z, _ in stood), "they stand in the square")
        check(all(min(abs(x - lane) for lane in lanes) > KEEP_SIDE + END_RADIUS + 0.3 for x, _, _ in stood),
              "nobody stands where a lane's people walk")
        check(min(math.dist(a[:2], b[:2]) for i, a in enumerate(stood) for b in stood[max(0, i - 40):i]) > 0.4,
              "nobody stands in another")
        check(min(z for _, z, _ in stood) < SQUARE[1] - HALF_Z + 3.0 and max(z for _, z, _ in stood) > SQUARE[1] + HALF_Z - 3.0 and
              min(x for x, _, _ in stood) < lanes[1] and max(x for x, _, _ in stood) > lanes[-2], "they fill the square end to end")
        # The walkers: shared among the lanes and both ways, each set out along its own side of its lane,
        # between its home and where its behaviour has it cross to, facing that.
        walk = walkers(walking)
        check(len(walk) == walking and len({(x, z) for _, _, x, z, _ in walk}) == walking, f"{walking} walk, each from its own spot")
        per_home = [sum(1 for home, *_ in walk if home == name) for name in ends if "home" in name]
        check(max(per_home) - min(per_home) <= 1, f"{walking} walkers: the lanes' ends share them evenly, got {sorted(set(per_home))}")
        for home, behaviour, x, z, yaw in walk:
            hx, hz = ends[home]
            tx, tz = ends[ways[behaviour][0]["place"]]
            check(x == hx == tx and min(hz, tz) < z < max(hz, tz), f"one of {home} starts on its side of its lane, on its way")
            check(yaw == (0 if tz > hz else 180), f"one of {home} faces the way it first goes")
    try:
        standing_spots(100000)
        check(False, "more people than the square has places are refused")
    except ValueError:
        pass
    # The lanes' ends: each its own name; a way's home and its end face each other down the lane, on
    # one side of its middle, the other way's on the other, their people's spots apart.
    check(len(ends) == 4 * LANES, "every end has its own name")
    for lane in range(LANES):
        to_b, end_b = ends[f"Lane {lane} home to b"], ends[f"Lane {lane} end b"]
        to_a, end_a = ends[f"Lane {lane} home to a"], ends[f"Lane {lane} end a"]
        check(to_b[0] == end_b[0] and to_a[0] == end_a[0] and abs(end_b[1] - to_b[1]) == 2 * HALF_Z == abs(end_a[1] - to_a[1]),
              f"lane {lane}: each way is walked straight down the lane, end to end")
        check(to_b[1] == end_a[1] and to_a[1] == end_b[1], f"lane {lane}: one way's home is at the other's end")
        check(abs(to_b[0] - to_a[0]) > 2 * END_RADIUS and abs((to_b[0] + to_a[0]) / 2 - lane_x(lane)) < 1e-6,
              f"lane {lane}: the two ways keep to their own sides of its middle")
    # The behaviours: one a lane and a way; across to the lane's end and, arrived, home and, arrived,
    # across again, at a walk.
    check(len(ways) == 2 * LANES and {behaviour for _, behaviour, *_ in walkers(WALKING)} == set(ways),
          "a behaviour a lane and a way, each with its walkers")
    for name, states in ways.items():
        across, back = states
        check(across["at"] == "place" and across["place"] in ends and "end" in across["place"] and back["at"] == "home",
              f"{name}: across to a lane's end, then home")
        check(across["transitions"] == [{"to": back["name"], "when": [{"is": "arrived"}]}] and
              back["transitions"] == [{"to": across["name"], "when": [{"is": "arrived"}]}], f"{name}: each begins as the other arrives")
        check(all(state["speed"] == WALK_SPEED and state["activity"] == "stroll" for state in states), f"{name}: strolling at a walk")
    # The looks: each its own colours.
    colours = [tuple(tint) for _, tints in LOOKS for tint in tints]
    check(len(set(colours)) == len(colours) and all(len(tints) <= 16 for _, tints in LOOKS), "every look's colours are its own")
    # The flight: three legs in order, no leap and no sudden turn; in from beyond where figures are
    # drawn, through the square at head height, round it and up, looking at its middle.
    for stops in (STOPS, SMALL_STOPS):
        approach_end, lane_end = leg_ends(stops)
        check(0 < approach_end < lane_end < stops - 1, f"{stops} stops: the legs follow one another")
        check([leg_of(s, stops) for s in (0, approach_end, approach_end + 1, lane_end, lane_end + 1, stops - 1)] ==
              ["approach", "approach", "lane", "lane", "rise", "rise"], f"{stops} stops: each leg ends where the next begins")
        path = flight_path(stops, lambda points: [10.0] * len(points))
        check(len(path) == stops, f"{stops} stops: a stop each")
        leap = max(math.dist(a[0], b[0]) for a, b in zip(path, path[1:]))
        check(leap < 6.0, f"{stops} stops: the camera moves {leap:.1f} m a stop at most")
        turn = max(abs((bearing(b) - bearing(a) + 180.0) % 360.0 - 180.0) for a, b in zip(path, path[1:]))
        check(turn < 12.0, f"{stops} stops: the camera turns {turn:.1f} degrees a stop at most")
        check(all(math.dist(p, look) > 30.0 for p, look in path), "the camera never looks at a point close by")
        nearest = path[0][0][2] - (SQUARE[1] + HALF_Z)
        check(nearest > FIGURE_DISTANCE + FIGURE_KEEP and APPROACH_FROM > FIGURE_DISTANCE + FIGURE_KEEP,
              f"the flight sets out {nearest:.0f} m from the nearest people: beyond where figures are drawn or kept")
        check(abs(path[0][0][1] - 35.0) < 1e-6 and path[0][1][::2] == [SQUARE[0], SQUARE[1]],
              "it sets out 25 m over the ground, looking at the square's middle")
        in_lane = path[approach_end + 1:lane_end + 1]
        check(all(abs(p[0] - lane_x(LANES // 2)) < 1e-6 and abs(p[1] - 11.8) < 1e-6 for p, _ in in_lane),
              "the lane is flown down its middle at head height")
        check(in_lane[0][0][2] > SQUARE[1] + HALF_Z and in_lane[-1][0][2] < SQUARE[1] - HALF_Z, "from one end of the square to the other")
        check(all(abs(bearing(stop)) < 1e-6 for stop in in_lane), "looking ahead down it")
        rise = path[lane_end + 1:]
        check(math.dist(rise[0][0], in_lane[-1][0]) < 3.0, "the rise begins where the lane ended")
        off = [math.dist(p[::2], SQUARE) for p, _ in rise]
        check(all(b > a for a, b in zip(off, off[1:])) and all(b[0][1] > a[0][1] for a, b in zip(rise, rise[1:])),
              "the rise goes out and up all the way")
        round_ = sum((math.degrees(math.atan2(b[0][0] - SQUARE[0], -(b[0][2] - SQUARE[1])) -
                                   math.atan2(a[0][0] - SQUARE[0], -(a[0][2] - SQUARE[1]))) + 180.0) % 360.0 - 180.0
                     for a, b in zip(rise, rise[1:]))
        check(350.0 < round_ <= 360.0, f"the rise goes once round the square ({round_:.0f} degrees)")
        check(abs(path[-1][0][1] - 90.0) < 1e-6 and path[-1][1] == [SQUARE[0], 11.0, SQUARE[1]],
              "the flight ends 80 m over the square, looking at its middle")
    # What the figures add: the median over the rounds, of the passes every state timed; their shadows'
    # share is what they add with the shadows on less what they add with them off.
    def timed(both, no_figures, no_shadows, neither):
        return dict(zip(("both", "no_figures", "no_shadows", "neither"), (both, no_figures, no_shadows, neither)))

    check(set(STATES) == {"both", "no_figures", "no_shadows", "neither"} and STATES["both"] == (True, True) and
          STATES["no_figures"] == (False, True) and STATES["no_shadows"] == (True, False) and STATES["neither"] == (False, False),
          "the four states: the figures and the shadows, each on and off")
    order = list(STATES)
    check(abs(order.index("both") - order.index("no_figures")) == 1, "the states with the shadows on are sampled one after the other")
    # (With the shadows off their own pass is not run, and so not timed: it takes no time then.)
    rounds = [timed({"shadows": 1.7, "lamp shadows": 0.2, "raster": 2.1, "sky": 0.5},
                    {"shadows": 1.0, "lamp shadows": 0.2, "raster": 1.2, "sky": 0.5},
                    {"lamp shadows": 0.2, "raster": 2.0, "sky": 0.5},
                    {"lamp shadows": 0.2, "raster": 1.2, "sky": 0.5}),            # adds 1.6, 0.8 without shadows
              timed({"shadows": 1.5, "lamp shadows": 0.2, "raster": 2.0, "sky": 0.4, "post": 1.0},
                    {"shadows": 1.0, "lamp shadows": 0.2, "raster": 1.2, "sky": 0.5},
                    {"lamp shadows": 0.2, "raster": 1.9, "sky": 0.5},
                    {"lamp shadows": 0.2, "raster": 1.2, "sky": 0.5}),            # adds 1.2, 0.7
              timed({"shadows": 1.4, "lamp shadows": 0.2, "raster": 1.9, "sky": 0.4},
                    {"shadows": 1.0, "lamp shadows": 0.2, "raster": 1.2, "sky": 0.5},
                    {"lamp shadows": 0.2, "raster": 1.4, "sky": 0.5},
                    {"lamp shadows": 0.2, "raster": 1.2, "sky": 0.5})]            # adds 1.0, 0.2
    total, shadows, parts = added_cost(rounds)
    check(abs(total - 1.2) < 1e-9, f"the figures add 1.2 ms (the median of 1.6, 1.2 and 1.0; the shadows' pass among it), got {total}")
    check(abs(shadows - 0.8) < 1e-9, f"0.8 ms of it their shadows (the median of 0.8, 0.5 and 0.8), got {shadows}")
    check(list(parts) == ["raster", "shadows", "sky"] and abs(parts["raster"] - 0.8) < 1e-9 and abs(parts["shadows"] - 0.5) < 1e-9 and
          abs(parts["sky"] + 0.1) < 1e-9,
          f"the passes that change with the figures, most first (one not timed with them and without left out), got {parts}")
    check(added_cost([]) is None and added_cost([timed({}, {}, {}, {})]) is None, "nothing timed: nothing told")
    # The shadows keep up with the figures.
    kept = {"both": {"drawn": 40, "waiting": 3, "behind": 2}, "no_figures": {"drawn": 2, "waiting": 0, "behind": 2}}
    check(shadow_failures(kept) == [], "no more pages behind with the figures than without passes")
    behind = shadow_failures(dict(kept, both={"drawn": 64, "waiting": 200, "behind": 3}))
    check(len(behind) == 1 and "3 pages behind" in behind[0], f"more pages behind with the figures fails, got {behind}")
    # The crowd's checks.
    seen = {"drawn_least": 2850, "figures_waiting": 0, "walking_most": 250}
    fine = {"looks_without": 0, "looks_baking": 0, "poses": 27, "walk_poses": 8}

    def fails(what, seen_=seen, figures=fine, walking=1000):
        found = crowd_failures(3000, walking, seen_, figures, 3)
        return len(found) == 1 and what in found[0]

    check(crowd_failures(3000, 1000, seen, fine, 3) == [], "a crowd all but a twentieth drawn, a quarter of its walkers walking, passes")
    check(fails("2849 of the 3000", dict(seen, drawn_least=2849)), "a twentieth and one not drawn fails")
    check(fails("waited for room", dict(seen, figures_waiting=3)), "figures waiting for room in the flight fail")
    check(fails("no pose", figures=dict(fine, looks_without=1)), "a look with no pose fails")
    check(fails("still being baked", figures=dict(fine, looks_baking=1)), "a look still being baked fails")
    check(fails("26 poses", figures=dict(fine, poses=26)), "a walk's pose missing fails")
    check(fails("249 people walked", dict(seen, walking_most=249)), "under a quarter of the walkers walking fails")
    check(crowd_failures(3000, 0, dict(seen, walking_most=0), dict(fine, poses=3), 3) == [], "a crowd with no walkers needs none walking")

    for failure in failures:
        print(f"test_crowd_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("test_crowd_gate: PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
