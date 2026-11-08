// Following a path: corners in turn, slowing to stop at the end; two
// walkers meeting in a corridor pass each other, neither stopping.
#include "giant/nav/follow.h"

#include "giant_test.h"

#include <cmath>

using namespace giant;
using namespace giant::nav;

namespace {

double flat(const DVec3& a, const DVec3& b) { return std::hypot(a.x - b.x, a.z - b.z); }

} // namespace

GIANT_TEST("follow: walks the corners in turn at its speed, slows and stops at the end") {
    PathFollower f;
    f.set_path({{0, 0, 0}, {10, 0, 0}, {10, 0, 10}});
    GIANT_CHECK(f.corner() == 1);
    FollowSettings s;
    DVec3 at{0, 0, 0};
    bool arrived = false;
    float fastest = 0.0f;
    usize reached = 0;
    for (int k = 0; k < 2000 && !arrived; ++k) {
        const Steering st = f.steer(at, {}, s);
        arrived = st.arrived;
        fastest = std::max(fastest, length(st.velocity));
        at = at + DVec3{st.velocity.x, 0.0, st.velocity.z} * (1.0 / 60.0);
        reached = std::max(reached, f.corner());
    }
    GIANT_CHECK(arrived);
    GIANT_CHECK(reached == 2);
    GIANT_CHECK(flat(at, {10, 0, 10}) <= s.arrive + 1e-3);
    GIANT_CHECK(std::fabs(fastest - s.speed) < 1e-3);
    GIANT_CHECK(f.remaining(at) < 0.3);
    // No path: arrived; NaN: nothing.
    PathFollower none;
    GIANT_CHECK(none.steer({}, {}, s).arrived);
    GIANT_CHECK(length(f.steer({NAN, 0, 0}, {}, s).velocity) == 0.0f);
}

GIANT_TEST("follow: two meeting head on pass each other, kept apart, and both arrive") {
    PathFollower a, b;
    a.set_path({{0, 0, 0}, {20, 0, 0}});
    b.set_path({{20, 0, 0}, {0, 0, 0}});
    FollowSettings s;
    DVec3 pa{0, 0, 0}, pb{20, 0, 0};
    Vec3 va{}, vb{};
    double closest = 1e9;
    bool da = false, db = false;
    for (int k = 0; k < 6000 && !(da && db); ++k) {
        const Neighbour nb{pb, vb, s.radius}, na{pa, va, s.radius};
        const Steering sa = a.steer(pa, {&nb, 1}, s);
        const Steering sb = b.steer(pb, {&na, 1}, s);
        da = da || sa.arrived;
        db = db || sb.arrived;
        va = sa.velocity;
        vb = sb.velocity;
        pa = pa + DVec3{va.x, 0.0, va.z} * (1.0 / 60.0);
        pb = pb + DVec3{vb.x, 0.0, vb.z} * (1.0 / 60.0);
        closest = std::min(closest, flat(pa, pb));
    }
    GIANT_CHECK(da && db);
    GIANT_CHECK(closest > 2.0 * s.radius); // never walked into each other
    GIANT_CHECK(flat(pa, {20, 0, 0}) < 0.5 && flat(pb, {0, 0, 0}) < 0.5);
}

GIANT_TEST("follow: a hairpin is walked round, not cut; two sent to one spot both come to rest there") {
    PathFollower f;
    f.set_path({{0, 0, 0}, {10, 0, 0}, {0, 0, 1.5}});
    FollowSettings s;
    const Steering first = f.steer({0, 0, 0}, {}, s);
    GIANT_CHECK(f.corner() == 1 && first.velocity.x > 1.0f); // along the first leg, not at the wall
    DVec3 at{0, 0, 0};
    bool turned = false;
    for (int k = 0; k < 3000; ++k) {
        const Steering st = f.steer(at, {}, s);
        if (st.arrived) break;
        if (f.corner() == 2) {
            turned = true;
            GIANT_CHECK(at.x > 9.0); // turned at the far end
            break;
        }
        at = at + DVec3{st.velocity.x, 0.0, st.velocity.z} * (1.0 / 60.0);
    }
    GIANT_CHECK(turned);

    PathFollower a, b;
    a.set_path({{0, 0, 0}, {10, 0, 0}});
    b.set_path({{0, 0, 3}, {10, 0, 0}});
    DVec3 pa{0, 0, 0}, pb{0, 0, 3};
    Vec3 va{}, vb{};
    bool da = false, db = false;
    for (int k = 0; k < 6000 && !(da && db); ++k) {
        const Neighbour nb{pb, vb, s.radius}, na{pa, va, s.radius};
        const Steering sa = da ? Steering{{}, true} : a.steer(pa, {&nb, 1}, s);
        const Steering sb = db ? Steering{{}, true} : b.steer(pb, {&na, 1}, s);
        da = sa.arrived;
        db = sb.arrived;
        va = sa.velocity;
        vb = sb.velocity;
        pa = pa + DVec3{va.x, 0.0, va.z} * (1.0 / 60.0);
        pb = pb + DVec3{vb.x, 0.0, vb.z} * (1.0 / 60.0);
    }
    GIANT_CHECK(da && db);
    GIANT_CHECK(std::hypot(pa.x - 10.0, pa.z) < 1.5 && std::hypot(pb.x - 10.0, pb.z) < 1.5);
}
