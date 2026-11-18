"""Checks the living-world gate's own arithmetic (no editor needed): python test_living_gate.py"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from living_gate import (FIELD_IN, FLED_AFTER, FLIGHT_HOURS, FRAMES_A_STOP, PEOPLE, PER_HOUSE, READ_EVERY, SHOT_AT,  # noqa: E402
                         SMALL_STOPS, SMALL_VILLAGES, VILLAGES, day_check, flight_clock_speed, house_spots, loaded_failures,
                         market_spots, out_of_doors, people_count, people_ms, plan, runs_home, travelling, village_spots)
from n3_gate import HAMLET, STOPS, TOWN, TOWN_RADIUS, flight_path, hamlet_houses, leg_ends, leg_of, town_houses  # noqa: E402


def person(id_, at, activity, trip=None, failed=False):
    return {"id": id_, "at": list(at), "doing": {"activity": activity, "place": "", "failed": failed, "own": False},
            "trip": trip}


def main():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # The whole valley: 5,000 people, every one with a home, the homes shared evenly.
    check(people_count(VILLAGES) == PEOPLE, f"all the villages hold {people_count(VILLAGES)} people, not {PEOPLE}")
    for villages in (VILLAGES, SMALL_VILLAGES):
        laid = plan(villages, people_count(villages))
        homes = [home for home, _ in laid["people"]]
        per_home = [homes.count(h) for h in range(len(laid["homes"]))]
        check(len(laid["people"]) == people_count(villages), "everyone has a home")
        fixed = len(town_houses()) + 12
        check(set(per_home[:fixed]) == {PER_HOUSE} and max(per_home[fixed:]) - min(per_home[fixed:]) <= 1 and
              min(per_home[fixed:]) >= 1, f"the town's and hamlet's homes hold {PER_HOUSE}, the villages' share the rest evenly, "
              f"got {sorted(set(per_home))}")
        check(len({name for name, _, _ in laid["homes"] + laid["fields"] + laid["wells"] + laid["markets"]}) ==
              len(laid["homes"]) + len(laid["fields"]) + len(laid["wells"]) + len(laid["markets"]), "every place has its own name")
        check(sum(1 for _, routine in laid["people"] if routine == "townsfolk") == len(town_houses()) * PER_HOUSE,
              "the town's people are townsfolk, the rest farmers")
        check(len(laid["fields"]) == villages + 1 and len(laid["wells"]) == villages + 1, "a field and a well a village, and the hamlet's")
        # Villages: clear of the town's site, of each other, and their fields clear of their houses.
        middles = village_spots(villages)
        check(len(set(middles)) == villages, "each village has its own spot")
        for i, middle in enumerate(middles):
            check(math.dist(middle, TOWN) > TOWN_RADIUS + 60.0, f"village {i} is clear of the town")
            check(all(math.dist(middle, other) >= 100.0 for other in middles[:i]), f"village {i} is clear of the others")
            _, fx, fz = laid["fields"][i]
            check(all(math.hypot(x - fx, z - fz) > 15.0 + 4.0 for x, z in house_spots(middle)),
                  f"village {i}'s field is clear of its houses")
            check(abs(fx) == abs(middle[0]) - FIELD_IN and abs(fx) > 180.0, f"village {i}'s field lies towards the river, off its bank")
        # The hamlet's field and well are its own, clear of its houses and of every village's field and well.
        for (name, x, z), radius in ((laid["fields"][-1], 15.0), (laid["wells"][-1], 4.0)):
            check(all(abs(x - hx) > 2.0 + radius or abs(z - hz) > 2.5 + radius for hx, hz in hamlet_houses()),
                  f"{name} is clear of the hamlet's houses")
            check(all(math.hypot(x - ox, z - oz) > radius + 15.0 for _, ox, oz in laid["fields"][:-1] + laid["wells"][:-1]),
                  f"{name} is clear of the villages' fields and wells")
        check(abs(laid["fields"][-1][1] - HAMLET[0]) < 60.0, "the hamlet's field is by the hamlet")
    try:
        plan(4, 10)
        check(False, "too few people for the homes is refused")
    except ValueError:
        pass
    # The market's places lie down the street, between the rows of houses.
    for x, z in market_spots():
        check(all(abs(x - hx) > 2.0 + 0.9 or abs(z - hz) > 2.5 + 2.4 for hx, hz in town_houses()), f"market at {z} is clear of the houses")

    # The day's check: who does what the hour says, where it has them.
    laid = plan(SMALL_VILLAGES, people_count(SMALL_VILLAGES))
    ids = list(range(1, len(laid["people"]) + 1))
    want, where = {"farmer": "work", "townsfolk": "work"}, {"farmer": "fields", "townsfolk": "markets"}

    def at_work(id_, home, routine):
        spots = laid["markets"] if routine == "townsfolk" else laid["fields"]
        name, x, z = min(spots, key=lambda s: math.hypot(s[1] - laid["homes"][home][1], s[2] - laid["homes"][home][2]))
        return person(id_, (x + 1.0, 0.0, z), "work")

    working = {id_: at_work(id_, home, routine) for id_, (home, routine) in zip(ids, laid["people"])}
    check(day_check(working, laid, ids, want, where) == [], f"everyone at work passes, got {day_check(working, laid, ids, want, where)}")
    idle = dict(working)
    for id_ in ids[:len(ids) // 50]:
        idle[id_] = person(id_, working[id_]["at"], "sleep")
    check(any("do not do" in f for f in day_check(idle, laid, ids, want, where)), "a fiftieth asleep at work fails")
    failed = dict(working)
    for id_ in ids[:len(ids) // 50]:
        failed[id_] = person(id_, working[id_]["at"], "work", failed=True)
    check(any("do not do" in f for f in day_check(failed, laid, ids, want, where)), "a fiftieth whose routine failed fails")
    away = dict(working)
    for id_ in ids[:len(ids) // 10]:
        away[id_] = person(id_, (working[id_]["at"][0] + 40.0, 0.0, working[id_]["at"][2]), "work")
    check(any("not where" in f for f in day_check(away, laid, ids, want, where)), "a tenth away from work fails")
    few = dict(working)
    few[ids[0]] = person(ids[0], (0.0, 0.0, 0.0), "sleep")
    check(day_check(few, laid, ids, want, where) == [], "one straggler passes")
    # (By routine: the few townsfolk all astray fail, though they are under a twentieth of everyone.)
    town = [id_ for id_, (_, routine) in zip(ids, laid["people"]) if routine == "townsfolk"]
    lost = dict(working)
    for id_ in town[:len(town) // 10]:
        lost[id_] = person(id_, (working[id_]["at"][0] + 40.0, 0.0, working[id_]["at"][2]), "work")
    check(len(town) // 10 <= len(ids) // 20 and any("townsfolk are not where" in f for f in day_check(lost, laid, ids, want, where)),
          "a tenth of the townsfolk away from the market fails, few as they are")
    gone = dict(working)
    del gone[ids[3]]
    check(any("not there" in f for f in day_check(gone, laid, ids, want, where)), "one missing fails")
    homes_check = day_check({id_: person(id_, (laid["homes"][home][1], 0.0, laid["homes"][home][2] + 1.5), "sleep")
                             for id_, (home, _) in zip(ids, laid["people"])}, laid, ids,
                            {"farmer": "sleep", "townsfolk": "sleep"}, {"farmer": "home", "townsfolk": "home"})
    check(homes_check == [], f"everyone asleep by its own home passes, got {homes_check}")
    swapped = {id_: person(id_, (laid["homes"][(home + 7) % len(laid["homes"])][1], 0.0, laid["homes"][(home + 7) % len(laid["homes"])][2]), "sleep")
               for id_, (home, _) in zip(ids, laid["people"])}
    check(day_check(swapped, laid, ids, {"farmer": "sleep", "townsfolk": "sleep"}, {"farmer": "home", "townsfolk": "home"}) != [],
          "everyone asleep at another's home fails")

    # The people's share of a frame: the worst sample, its parts, the median by frames.
    def sample(frames, travel, senses, routines, people):
        return (frames, {"travel": travel, "senses": senses, "routines": routines, "people": people, "render": 9.0}, 3.0)

    cost = people_ms([sample(100, 0.01, 0.0, 0.02, 0.03), sample(100, 0.1, 0.2, 0.1, 0.2), sample(300, 0.02, 0.0, 0.03, 0.05)])
    check(cost is not None and abs(cost[0] - 0.6) < 1e-9 and cost[1]["senses"] == 0.2 and abs(cost[2] - 0.1) < 1e-9,
          f"the people's cost is the worst sample's, with its parts and the median, got {cost}")
    # (By frames: three samples of 0.1, 0.2 and 0.3 ms, the first as long as the others together and more.)
    cost = people_ms([sample(500, 0.1, 0.0, 0.0, 0.0), sample(100, 0.2, 0.0, 0.0, 0.0), sample(100, 0.3, 0.0, 0.0, 0.0)])
    check(abs(cost[2] - 0.1) < 1e-9, f"the median counts each sample by its frames, got {cost[2]}")
    check(people_ms([(100, {"render": 9.0}, 3.0)]) is None and people_ms([]) is None, "an editor that times no such phase gives none")

    # A loaded game: everyone where it was and doing what it did.
    moving = {"phase": "travelling", "to": [0, 0, 0], "speed": 3.0, "embodied": False}
    before = {i: person(i, (i * 2.0, 0.0, 5.0), "work") for i in range(1, 201)}
    before[1] = person(1, (2.0, 0.0, 5.0), "work", trip=moving)
    same = {i: person(i, (i * 2.0 + 0.3, 0.0, 5.0), "work") for i in range(1, 201)}
    same[1] = person(1, (22.0, 0.0, 5.0), "work")  # (it was on its way: farther on)
    check(loaded_failures(before, same) == [], f"everyone back passes, got {loaded_failures(before, same)}")
    elsewhere = {i: person(i, (i * 2.0 + (30.0 if i <= 5 else 0.0), 0.0, 5.0), "work") for i in range(1, 201)}
    check(any("not where they were" in f for f in loaded_failures(before, elsewhere)), "a few put elsewhere fail")
    other = {i: person(i, (i * 2.0, 0.0, 5.0), "eat" if i <= 5 else "work") for i in range(1, 201)}
    check(any("something else" in f for f in loaded_failures(before, other)), "a few doing something else fail")
    fewer = {i: p for i, p in same.items() if i != 7}
    check(any("are gone" in f for f in loaded_failures(before, fewer)), "one gone fails")
    all_moving = {i: person(i, p["at"], "work", trip=moving) for i, p in before.items()}
    check(any("says little" in f for f in loaded_failures(all_moving, same)), "everyone on their way says little")

    # One whose trip failed is not on its way; one with none neither.
    check(travelling(person(1, (0, 0, 0), "work", trip=moving)) and not travelling(person(1, (0, 0, 0), "work")) and
          not travelling(person(1, (0, 0, 0), "work", trip={"phase": "failed", "why": "no way"})), "who is on its way")
    # Who is out when the shot is fired, and who runs home from it.
    markets = [("Market 0", 0.0, -300.0), ("Market 1", 0.0, -290.0)]
    out = out_of_doors([person(1, (0.5, 0.0, -299.0), "work"), person(2, (30.0, 0.0, -300.0), "sleep"),
                        person(3, (30.0, 0.0, -300.0), "work", trip=moving)], markets)
    check(out == {1, 3}, f"those at a market or on their way are out, got {out}")
    home = ("Home T1", 20.0, -310.0)
    home_trip = dict(moving, place="Home T1")
    check(runs_home(person(1, (5.0, 0.0, -300.0), "flee", trip=home_trip), home) and
          runs_home(person(1, (20.5, 0.0, -309.0), "flee"), home) and
          not runs_home(person(1, (5.0, 0.0, -300.0), "flee"), home) and
          not runs_home(person(1, (5.0, 0.0, -300.0), "flee", trip=dict(moving, place="Market 0")), home) and
          not runs_home(person(1, (20.0, 0.0, -310.0), "work"), home), "who runs home: fleeing, and on its way there or there")

    # The flight: its hour of the day lasts as long as it does, whatever the frames' pace; the shot and
    # the reads after it fall inside it; the camera never leaps (50 m in a frame is a jump to the editor).
    for frame_seconds in (1.0 / 240.0, 1.0 / 60.0, 0.13):
        for stops in (STOPS, SMALL_STOPS):
            lasts = (240 + FRAMES_A_STOP * stops) * frame_seconds
            check(abs(flight_clock_speed(frame_seconds, stops) * lasts - FLIGHT_HOURS * 3600.0) < 1e-6,
                  f"a flight of {stops} stops at {frame_seconds:.4f} s a frame lasts its hour")
    check(0.0 < flight_clock_speed(0.0, STOPS) <= 86400.0 * 60.0, "no pace measured gives a speed still")
    for stops in (STOPS, SMALL_STOPS):
        low_end, street_end = leg_ends(stops)
        shot_at = low_end + int((street_end - low_end) * SHOT_AT)
        check(leg_of(shot_at, stops) == "street" and shot_at + FLED_AFTER < stops, f"{stops} stops: the shot and the read after it are in the flight")
        check(any(leg_of(stop, stops) == "street" for stop in range(0, stops, READ_EVERY)), f"{stops} stops: the street is read")
        # (The shot when two thirds of the townsfolk's setting off, 6.15 h to 6.48 h, is past, or later.)
        shot_hour = 5.8 + FLIGHT_HOURS * (240 + FRAMES_A_STOP * shot_at) / (240 + FRAMES_A_STOP * stops)
        check(6.3 <= shot_hour <= 6.6, f"{stops} stops: the shot falls at {shot_hour:.2f} h, while the townsfolk are out")
        path = flight_path(stops, lambda points: [0.0] * len(points))
        leap = max(math.dist(a[0], b[0]) for a, b in zip(path, path[1:]))
        check(leap < 50.0, f"{stops} stops: the camera moves {leap:.1f} m a stop at most, under a jump")

    for failure in failures:
        print(f"test_living_gate: FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("test_living_gate: PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
