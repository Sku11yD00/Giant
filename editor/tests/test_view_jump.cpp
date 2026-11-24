#include "giant_test.h"

#include "../src/view_jump.h"

#include <limits>

using namespace giant;
using namespace giant::editor;

GIANT_TEST("view jump: a view that jumps far brings the world in fast until it is in, and no longer than it may") {
    ViewJump jump;
    const double frame = 1.0 / 60.0;
    // A frame as the editor's: the view seen, and while on, what is coming counted.
    const auto update = [&jump](const DVec3& at, u64 coming, double seconds, bool held) {
        jump.see(at, seconds, held);
        if (jump.on()) jump.wait(coming);
    };
    // The first view is no jump, nor is flying (90 m/s: 1.5 m a frame), with the world coming or not.
    update({5000.0, 0.0, 0.0}, 100, frame, false);
    GIANT_CHECK(!jump.on() && jump.jumps() == 0);
    DVec3 view{5000.0, 0.0, 0.0};
    for (int i = 0; i < 100; ++i) {
        view.z -= 1.5;
        update(view, 100, frame, false);
    }
    GIANT_CHECK(!jump.on());
    // Just under the distance is a move; over it, a jump: on while the world comes.
    view.x += ViewJump::k_metres - 1.0;
    update(view, 100, frame, false);
    GIANT_CHECK(!jump.on());
    view.x += ViewJump::k_metres + 1.0;
    update(view, 0, frame, false); // (nothing coming yet: what a jump brings is asked for after)
    GIANT_CHECK(jump.on() && jump.jumps() == 1);
    for (int i = 0; i < 300; ++i) update(view, 7, frame, false);
    GIANT_CHECK(jump.on());
    // In: off after the quiet frames, not before; something coming again meanwhile starts them over.
    for (u32 i = 0; i + 1 < ViewJump::k_quiet_frames; ++i) update(view, 0, frame, false);
    GIANT_CHECK(jump.on());
    update(view, 1, frame, false);
    for (u32 i = 0; i + 1 < ViewJump::k_quiet_frames; ++i) update(view, 0, frame, false);
    GIANT_CHECK(jump.on());
    update(view, 0, frame, false);
    GIANT_CHECK(!jump.on());
    for (int i = 0; i < 20; ++i) update(view, 50, frame, false);
    GIANT_CHECK(!jump.on()); // (what comes later is play's own)

    // A world that never comes in: no longer than the most, of the frames' seconds.
    view.y += 1000.0;
    update(view, 3, frame, false);
    GIANT_CHECK(jump.on() && jump.jumps() == 2);
    for (int i = 0; i < 100; ++i) update(view, 3, 100.0, false); // (a frame counts for no more than k_most_frame)
    GIANT_CHECK(jump.on());
    for (int i = 0; i < 99; ++i) update(view, 3, ViewJump::k_most_frame, false);
    GIANT_CHECK(jump.on());
    for (int i = 0; i < 3; ++i) update(view, 3, ViewJump::k_most_frame, false);
    GIANT_CHECK(!jump.on());

    // A flight in a frame that took long is no jump (90 m/s for a second: 90 m, under what
    // k_speed covers); the same way gone in a frame's time is.
    view.z -= 90.0;
    update(view, 3, 1.0, false);
    GIANT_CHECK(!jump.on() && jump.jumps() == 2);
    view.z -= 90.0;
    update(view, 3, frame, false);
    GIANT_CHECK(jump.on() && jump.jumps() == 3);
    for (u32 i = 0; i < ViewJump::k_quiet_frames; ++i) update(view, 0, frame, false);
    GIANT_CHECK(!jump.on());
    // (After a stall however long, a real jump is still one.)
    view.z -= 1200.0;
    update(view, 0, 30.0, false);
    GIANT_CHECK(jump.on() && jump.jumps() == 4);
    for (u32 i = 0; i < ViewJump::k_quiet_frames; ++i) update(view, 0, frame, false);
    GIANT_CHECK(!jump.on());
    // A view that is no number is no jump, nor is the next real one after it.
    update({std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, 3, frame, false);
    GIANT_CHECK(!jump.on() && jump.jumps() == 4);
    update(view, 3, frame, false);
    GIANT_CHECK(!jump.on() && jump.jumps() == 4);

    // A loading screen stands for the world's coming: a jump behind it is not counted, one under
    // way ends, and where the view went behind it is no jump once it is gone.
    view.z += 900.0;
    update(view, 3, frame, false);
    GIANT_CHECK(jump.on());
    update(view, 3, frame, true);
    GIANT_CHECK(!jump.on());
    view.z += 900.0;
    update(view, 3, frame, true);
    update(view, 3, frame, false);
    GIANT_CHECK(!jump.on() && jump.jumps() == 5);
}
