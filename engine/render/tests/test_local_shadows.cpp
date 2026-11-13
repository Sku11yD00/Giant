// Local light shadow tiles (src/local_shadows.h): which lights keep tiles,
// which faces are drawn when, and that a face's matrix finds the texels
// shaders/lights.slang reads.
#include "local_shadows.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

using Boxes = std::vector<std::pair<DVec3, DVec3>>;

LocalLight lamp(u32 id, const DVec3& p, LightKind kind = LightKind::point) {
    LocalLight l;
    l.position = p;
    l.range = 5.0f;
    l.kind = kind;
    l.shadow_id = id;
    return l;
}

std::vector<ShadowCandidate> candidates(const std::vector<LocalLight>& lights) {
    std::vector<ShadowCandidate> out;
    for (u32 i = 0; i < lights.size(); ++i) out.push_back({i, &lights[i]});
    return out;
}

void plan(LocalShadows& s, const std::vector<LocalLight>& lights, u32 budget = 24, const Boxes& changed = {},
          bool all = false) {
    s.plan(candidates(lights), DVec3{}, changed, all, budget);
}

const LocalShadows::Shadowed* shadowed(const LocalShadows& s, u32 light) {
    for (const LocalShadows::Shadowed& x : s.shadowed())
        if (x.light == light) return &x;
    return nullptr;
}

struct LightShadowRight {
    Vec3 v;
    explicit LightShadowRight(const float (&r)[3]) : v{r[0], r[1], r[2]} {}
};

} // namespace

GIANT_TEST("local shadows: a spot takes one tile, a point light six; drawn once, then kept") {
    LocalShadows s(16);
    std::vector<LocalLight> lights{lamp(7, {0.0, 2.0, 0.0}), lamp(9, {10.0, 2.0, 0.0}, LightKind::spot)};
    plan(s, lights);
    GIANT_CHECK(s.draws().size() == 7);
    GIANT_CHECK(s.tiles_used() == 7);
    GIANT_CHECK(s.waiting() == 0);
    GIANT_CHECK(s.shadowed().size() == 2);
    const LocalShadows::Shadowed* p = shadowed(s, 0);
    const LocalShadows::Shadowed* q = shadowed(s, 1);
    GIANT_CHECK(p != nullptr && p->record.drawn == 0x3fu);
    GIANT_CHECK(q != nullptr && q->record.drawn == 0x1u);
    GIANT_CHECK(p != nullptr && q != nullptr && p->record.tile != q->record.tile);
    // The faces drawn are the light's tiles, in face order.
    for (u32 f = 0; f < 6 && p != nullptr; ++f) GIANT_CHECK(s.draws()[f].tile == p->record.tile + f);
    plan(s, lights);
    GIANT_CHECK(s.draws().empty());
    GIANT_CHECK(s.shadowed().size() == 2 && s.tiles_used() == 7);
}

GIANT_TEST("local shadows: a few faces a frame, nearest lights first; undrawn lights shine unshadowed") {
    LocalShadows s(32);
    std::vector<LocalLight> lights{lamp(1, {0.0, 0.0, 0.0}), lamp(2, {20.0, 0.0, 0.0}), lamp(3, {40.0, 0.0, 0.0})};
    plan(s, lights, 8);
    GIANT_CHECK(s.draws().size() == 8);
    GIANT_CHECK(s.waiting() == 10);
    GIANT_CHECK(s.shadowed().size() == 2); // the third has nothing drawn yet
    GIANT_CHECK(shadowed(s, 0) != nullptr && shadowed(s, 0)->record.drawn == 0x3fu);
    GIANT_CHECK(shadowed(s, 1) != nullptr && shadowed(s, 1)->record.drawn == 0x3u);
    GIANT_CHECK(shadowed(s, 2) == nullptr);
    plan(s, lights, 8);
    GIANT_CHECK(s.draws().size() == 8 && s.waiting() == 2);
    GIANT_CHECK(shadowed(s, 1) != nullptr && shadowed(s, 1)->record.drawn == 0x3fu);
    GIANT_CHECK(shadowed(s, 2) != nullptr && shadowed(s, 2)->record.drawn == 0xfu);
    plan(s, lights, 8);
    GIANT_CHECK(s.draws().size() == 2 && s.waiting() == 0);
    plan(s, lights, 8);
    GIANT_CHECK(s.draws().empty());
}

GIANT_TEST("local shadows: a moved or changed light is unshadowed until drawn again") {
    LocalShadows s(16);
    std::vector<LocalLight> lights{lamp(1, {0.0, 0.0, 0.0})};
    plan(s, lights);
    lights[0].position.x = 0.5;
    plan(s, lights, 2);
    GIANT_CHECK(s.draws().size() == 2);
    GIANT_CHECK(shadowed(s, 0) != nullptr && shadowed(s, 0)->record.drawn == 0x3u); // old faces not used
    plan(s, lights, 24);
    GIANT_CHECK(s.draws().size() == 4);
    lights[0].shadow_near = 0.3f;
    plan(s, lights, 24);
    GIANT_CHECK(s.draws().size() == 6);
    GIANT_CHECK(shadowed(s, 0) != nullptr && std::fabs(shadowed(s, 0)->record.near - 0.3f) < 1e-6f);
    // Moved a little (a lantern swinging): drawn again, the old faces
    // shading meanwhile; moved on further than that from where they were
    // drawn, the faces not drawn again since are dropped.
    plan(s, lights, 24);
    GIANT_CHECK(s.draws().empty());
    lights[0].position.x += 0.03; // under 5 cm (1% of its 5 m range)
    plan(s, lights, 2);
    GIANT_CHECK(s.draws().size() == 2 && s.waiting() == 4);
    GIANT_CHECK(shadowed(s, 0) != nullptr && shadowed(s, 0)->record.drawn == 0x3fu);
    lights[0].position.x += 0.03; // 6 cm from where four faces were drawn, 3 cm from the two just drawn
    plan(s, lights, 1);
    // (Faces with nothing to shade with go first: face 2.)
    GIANT_CHECK(s.draws().size() == 1 && shadowed(s, 0)->record.drawn == 0x7u);
    plan(s, lights, 24);
    GIANT_CHECK(s.draws().size() == 5 && shadowed(s, 0)->record.drawn == 0x3fu);
    // A spot turned: dropped at once.
    std::vector<LocalLight> spot{lamp(5, {0.0, 3.0, 0.0}, LightKind::spot)};
    spot[0].direction = {0.0f, -1.0f, 0.0f};
    plan(s, spot, 24);
    spot[0].direction = {0.1f, -1.0f, 0.0f};
    plan(s, spot, 0);
    GIANT_CHECK(shadowed(s, 0) == nullptr);
    // A point light turned spot gives back five tiles.
    lights[0].kind = LightKind::spot;
    plan(s, lights, 24);
    GIANT_CHECK(s.draws().size() == 1 && s.tiles_used() == 2); // and the spot's
}

GIANT_TEST("local shadows: casters changing near a light draw it again, the old depths standing in") {
    LocalShadows s(16);
    std::vector<LocalLight> lights{lamp(1, {0.0, 0.0, 0.0}), lamp(2, {100.0, 0.0, 0.0})};
    plan(s, lights);
    Boxes far;
    far.push_back({DVec3{50.0, -1.0, -1.0}, DVec3{52.0, 1.0, 1.0}});
    plan(s, lights, 24, far);
    GIANT_CHECK(s.draws().empty());
    Boxes near;
    near.push_back({DVec3{4.5, -0.1, -0.1}, DVec3{4.8, 0.1, 0.1}}); // 4.5 m from the first: inside its 5 m
    plan(s, lights, 2, near);
    // Only the face that sees it (+x, the first light's first tile).
    GIANT_CHECK(s.draws().size() == 1 && s.waiting() == 0);
    GIANT_CHECK(s.draws().size() == 1 && s.draws()[0].tile == shadowed(s, 0)->record.tile);
    GIANT_CHECK(shadowed(s, 0)->record.drawn == 0x3fu);
    // Up a corner (+x, +y, +z): three faces, two drawn now, the old depths standing in for the third.
    Boxes corner;
    corner.push_back({DVec3{2.5, 2.5, 2.5}, DVec3{2.6, 2.6, 2.6}});
    plan(s, lights, 2, corner);
    GIANT_CHECK(s.draws().size() == 2);
    GIANT_CHECK(shadowed(s, 0)->record.drawn == 0x3fu); // still shaded with the old faces
    GIANT_CHECK(s.waiting() == 1);
    plan(s, lights, 24); // the rest without being asked again
    GIANT_CHECK(s.draws().size() == 1);
    plan(s, lights, 24, {}, true);
    GIANT_CHECK(s.draws().size() == 12);
    s.invalidate_all();
    plan(s, lights, 24);
    GIANT_CHECK(s.draws().size() == 12);
}

GIANT_TEST("local shadows: a face sees a box only inside its four sides") {
    const DVec3 at{10.0, 2.0, -5.0};
    const auto sees = [&](u32 face, const DVec3& c, double h) {
        return LocalShadows::face_sees_box(LightKind::point, {}, 0.0f, face, at, at + c - DVec3{h, h, h},
                                           at + c + DVec3{h, h, h});
    };
    // Straight ahead of +x (face 0) only; a box round the light every face.
    GIANT_CHECK(sees(0, {5.0, 0.0, 0.0}, 0.5) && !sees(1, {5.0, 0.0, 0.0}, 0.5) && !sees(2, {5.0, 0.0, 0.0}, 0.5) &&
                !sees(3, {5.0, 0.0, 0.0}, 0.5) && !sees(4, {5.0, 0.0, 0.0}, 0.5) && !sees(5, {5.0, 0.0, 0.0}, 0.5));
    for (u32 f = 0; f < 6; ++f) GIANT_CHECK(sees(f, {0.0, 0.0, 0.0}, 0.5));
    // On the edge between +x and +y: both.
    GIANT_CHECK(sees(0, {4.0, 4.0, 0.0}, 0.2) && sees(2, {4.0, 4.0, 0.0}, 0.2) && !sees(1, {4.0, 4.0, 0.0}, 0.2));
    // A spot looking down sees below, not beside.
    GIANT_CHECK(LocalShadows::face_sees_box(LightKind::spot, {0.0f, -1.0f, 0.0f}, 0.5f, 0, at,
                                            at + DVec3{-0.5, -6.0, -0.5}, at + DVec3{0.5, -5.0, 0.5}));
    GIANT_CHECK(!LocalShadows::face_sees_box(LightKind::spot, {0.0f, -1.0f, 0.0f}, 0.5f, 0, at,
                                             at + DVec3{5.5, -1.0, -0.5}, at + DVec3{6.5, 0.0, 0.5}));
    // A bad box: none.
    GIANT_CHECK(!sees(0, {5.0, 0.0, 0.0}, -1.0));
}

GIANT_TEST("local shadows: a light's planes hold its reach and every face's part of it") {
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const DVec3 camera{-200.0, 10.0, 40.0};
    LocalShadows::Face f;
    f.position = camera + DVec3{12.0, 3.0, -7.0};
    f.range = 6.0f;
    f.near = 0.2f;
    float planes[20];
    LocalShadows::light_planes(f, camera, planes);
    const Vec3 l = to_camera_relative(f.position, camera);
    for (int k = 0; k < 2000; ++k) {
        const Vec3 d{u(rng), u(rng), u(rng)};
        if (length(d) > 1.0f) continue;
        const Vec3 p = l + d * f.range; // within reach
        for (u32 i = 0; i < 5; ++i) {
            GIANT_CHECK(planes[i * 4] * p.x + planes[i * 4 + 1] * p.y + planes[i * 4 + 2] * p.z + planes[i * 4 + 3] >=
                        -1e-4f);
        }
    }
    // Out of reach beside or below: outside; above: kept (the box is open there).
    const auto inside = [&](const Vec3& p) {
        for (u32 i = 0; i < 5; ++i) {
            if (planes[i * 4] * p.x + planes[i * 4 + 1] * p.y + planes[i * 4 + 2] * p.z + planes[i * 4 + 3] < 0.0f)
                return false;
        }
        return true;
    };
    GIANT_CHECK(!inside(l + Vec3{6.5f, 0.0f, 0.0f}) && !inside(l + Vec3{0.0f, 0.0f, -6.5f}) &&
                !inside(l + Vec3{0.0f, -6.5f, 0.0f}));
    GIANT_CHECK(inside(l + Vec3{0.0f, 50.0f, 0.0f}));
}

GIANT_TEST("local shadows: a light cut short by the budget stays in step: its faces drawn together after") {
    LocalShadows s(32);
    std::vector<LocalLight> lights{lamp(1, {0.0, 0.0, 0.0}), lamp(2, {100.0, 0.0, 0.0}), lamp(3, {200.0, 0.0, 0.0})};
    plan(s, lights);
    // All three move every frame (lanterns swinging); 8 faces a frame.
    for (int f = 0; f < 30; ++f) {
        for (LocalLight& l : lights) l.position.y += 0.5;
        plan(s, lights, 8);
        GIANT_CHECK(s.draws().size() == 8);
        // At most two lights a frame: a whole one and part of the next, never three.
        u32 lights_drawn = 0;
        for (usize i = 0; i < s.draws().size(); ++i) {
            if (i == 0 || s.draws()[i].first_tile != s.draws()[i - 1].first_tile) ++lights_drawn;
        }
        GIANT_CHECK(lights_drawn <= 2);
    }
}

GIANT_TEST("local shadows: out of tiles, lights not wanted this frame give theirs up, oldest first") {
    LocalShadows s(12);
    std::vector<LocalLight> a{lamp(1, {0.0, 0.0, 0.0})};
    std::vector<LocalLight> b{lamp(2, {0.0, 0.0, 0.0})};
    std::vector<LocalLight> c{lamp(3, {0.0, 0.0, 0.0})};
    plan(s, a);
    plan(s, b);
    GIANT_CHECK(s.tiles_used() == 12);
    plan(s, c); // takes the first's (older)
    GIANT_CHECK(s.tiles_used() == 12 && s.draws().size() == 6);
    plan(s, b);
    GIANT_CHECK(s.draws().empty()); // kept its tiles
    plan(s, a);
    GIANT_CHECK(s.draws().size() == 6); // drawn again: it lost them
    // All wanted at once: the nearest (listed first) get them, the rest go unshadowed.
    std::vector<LocalLight> all{lamp(1, {0.0, 0.0, 0.0}), lamp(2, {5.0, 0.0, 0.0}), lamp(3, {9.0, 0.0, 0.0})};
    plan(s, all, 100);
    GIANT_CHECK(s.shadowed().size() == 2);
    GIANT_CHECK(shadowed(s, 2) == nullptr);
}

GIANT_TEST("local shadows: one shadow per id, none without; unused entries forgotten in time") {
    LocalShadows s(32);
    std::vector<LocalLight> lights{lamp(4, {0.0, 0.0, 0.0}), lamp(4, {1.0, 0.0, 0.0}), lamp(0, {2.0, 0.0, 0.0})};
    plan(s, lights);
    GIANT_CHECK(s.shadowed().size() == 1 && s.shadowed()[0].light == 0);
    GIANT_CHECK(s.tiles_used() == 6);
    const std::vector<LocalLight> none;
    for (int i = 0; i < 600; ++i) plan(s, none);
    GIANT_CHECK(s.tiles_used() == 6);
    plan(s, none);
    GIANT_CHECK(s.tiles_used() == 0);
}

GIANT_TEST("local shadows: a face's matrix and planes find the texel the shader reads") {
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const DVec3 camera{1000.0, 20.0, -300.0};
    for (int light = 0; light < 40; ++light) {
        const bool spot = light % 2 == 0;
        const LightKind kind = spot ? LightKind::spot : LightKind::point;
        const Vec3 direction{u(rng), u(rng), u(rng)};
        const float outer = 0.2f + 0.6f * (u(rng) + 1.0f);
        for (u32 face = 0; face < (spot ? 1u : 6u); ++face) {
            LocalShadows::Face f;
            f.position = camera + DVec3{u(rng) * 30.0, u(rng) * 5.0, u(rng) * 30.0};
            LocalShadows::face_axes(kind, direction, face, f.forward, f.right);
            f.tan_half = LocalShadows::tan_half(kind, outer);
            f.near = 0.2f;
            f.range = 7.0f;
            GIANT_CHECK(std::fabs(length(f.forward) - 1.0f) < 1e-4f && std::fabs(length(f.right) - 1.0f) < 1e-4f);
            GIANT_CHECK(std::fabs(dot(f.forward, f.right)) < 1e-4f);
            const Mat4 m = LocalShadows::face_matrix(f, camera);
            float planes[20];
            LocalShadows::face_planes(f, camera, planes);
            const Vec3 l = to_camera_relative(f.position, camera);
            const Vec3 up = cross(f.right, f.forward);
            for (int k = 0; k < 50; ++k) {
                // A point in front of the light, maybe outside the face.
                const float along = 0.05f + 6.0f * (u(rng) + 1.0f);
                const float sx = u(rng) * 1.3f;
                const float sy = u(rng) * 1.3f;
                const Vec3 d = f.forward * along + f.right * (sx * along * f.tan_half) + up * (sy * along * f.tan_half);
                const Vec3 p = l + d;
                const Vec4 clip = m * Vec4{p.x, p.y, p.z, 1.0f};
                GIANT_CHECK(std::fabs(clip.w - along) < 1e-3f * along + 1e-3f);
                const float nx = clip.x / clip.w;
                const float ny = clip.y / clip.w;
                const float nz = clip.z / clip.w;
                // As shaders/lights.slang: x, y across the tile, depth near / along.
                const float x = dot(d, f.right) / (along * f.tan_half);
                const float y = dot(d, up) / (along * f.tan_half);
                GIANT_CHECK(std::fabs(nx - x) < 1e-3f && std::fabs(ny - y) < 1e-3f);
                GIANT_CHECK(std::fabs(nz - f.near / along) < 1e-4f * (1.0f + nz));
                // In the face and within the light's reach (nearer than `near`: clipped by the projection).
                const bool inside = std::fabs(x) <= 1.0f && std::fabs(y) <= 1.0f && along <= f.range;
                bool in_planes = true;
                for (u32 i = 0; i < 5; ++i) {
                    const float s = planes[i * 4] * p.x + planes[i * 4 + 1] * p.y + planes[i * 4 + 2] * p.z +
                                    planes[i * 4 + 3];
                    in_planes = in_planes && s >= -1e-5f;
                }
                const float margin = std::min({std::fabs(std::fabs(x) - 1.0f), std::fabs(std::fabs(y) - 1.0f),
                                               std::fabs(along - f.range)});
                if (margin > 1e-2f) GIANT_CHECK(inside == in_planes);
            }
        }
    }
}

GIANT_TEST("local shadows: a point light's faces cover every direction, each the one its major axis picks") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const float t = LocalShadows::tan_half(LightKind::point, 0.0f);
    GIANT_CHECK(t > 1.0f && t < 1.05f);
    for (int k = 0; k < 2000; ++k) {
        const Vec3 d{u(rng), u(rng), u(rng)};
        const Vec3 m{std::fabs(d.x), std::fabs(d.y), std::fabs(d.z)};
        // The shader's pick (shaders/lights.slang light_shadow).
        u32 face = 0;
        if (m.x >= m.y && m.x >= m.z) face = d.x >= 0.0f ? 0u : 1u;
        else if (m.y >= m.z) face = d.y >= 0.0f ? 2u : 3u;
        else face = d.z >= 0.0f ? 4u : 5u;
        Vec3 forward;
        Vec3 right;
        LocalShadows::face_axes(LightKind::point, {}, face, forward, right);
        const float along = dot(d, forward);
        const Vec3 up = cross(right, forward);
        GIANT_CHECK(along > 0.0f);
        GIANT_CHECK(std::fabs(dot(d, right)) <= along * t && std::fabs(dot(d, up)) <= along * t);
    }
}

GIANT_TEST("local shadows: lights changing every frame share the budget, every face coming round") {
    LocalShadows s(32);
    std::vector<LocalLight> lights{lamp(1, {0.0, 0.0, 0.0}), lamp(2, {20.0, 0.0, 0.0})};
    plan(s, lights, 24);
    const Boxes both{{DVec3{-1.0, -1.0, -1.0}, DVec3{21.0, 1.0, 1.0}}}; // something moving by both, every frame
    std::vector<u32> times(12, 0);
    for (int frame = 0; frame < 12; ++frame) {
        plan(s, lights, 4, both);
        GIANT_CHECK(s.draws().size() == 4);
        for (const LocalShadows::Face& f : s.draws()) {
            const u32 base0 = shadowed(s, 0)->record.tile;
            const u32 base1 = shadowed(s, 1)->record.tile;
            const u32 k = f.tile >= base0 && f.tile < base0 + 6 ? f.tile - base0 : 6 + (f.tile - base1);
            if (k < 12) ++times[k];
        }
        GIANT_CHECK(shadowed(s, 0)->record.drawn == 0x3fu && shadowed(s, 1)->record.drawn == 0x3fu);
    }
    // 48 faces drawn over 12 frames: each of the 12 four times.
    for (const u32 t : times) GIANT_CHECK(t == 4);
    // A swinging light (always moving a little): all six of its faces come round.
    std::vector<LocalLight> swing{lamp(3, {0.0, 5.0, 0.0})};
    u32 seen = 0;
    for (int frame = 0; frame < 4; ++frame) {
        swing[0].position.x = 0.01 * (frame % 2);
        plan(s, swing, frame == 0 ? 24 : 2);
        for (const LocalShadows::Face& f : s.draws()) seen |= 1u << (f.tile - shadowed(s, 0)->record.tile);
        GIANT_CHECK(shadowed(s, 0)->record.drawn == 0x3fu);
    }
    GIANT_CHECK(seen == 0x3fu);
    // Lights moving fast (past where old faces are kept, every frame):
    // their faces still all come round, both lights'.
    LocalShadows fast(32);
    std::vector<LocalLight> runners{lamp(1, {0.0, 0.0, 0.0}), lamp(2, {20.0, 0.0, 0.0})};
    u32 got[2] = {0, 0};
    for (int frame = 0; frame < 12; ++frame) {
        runners[0].position.x += 0.1;
        runners[1].position.x += 0.1;
        plan(fast, runners, 4);
        for (const LocalShadows::Face& f : fast.draws()) {
            for (u32 l = 0; l < 2; ++l) {
                const LocalShadows::Shadowed* sh = shadowed(fast, l);
                if (sh != nullptr && f.tile >= sh->record.tile && f.tile < sh->record.tile + 6)
                    got[l] |= 1u << (f.tile - sh->record.tile);
            }
        }
    }
    GIANT_CHECK(got[0] == 0x3fu && got[1] == 0x3fu);
}

GIANT_TEST("local shadows: casters changing near a light out of view draw it again when back") {
    LocalShadows s(16);
    std::vector<LocalLight> lights{lamp(1, {0.0, 0.0, 0.0})};
    plan(s, lights);
    const std::vector<LocalLight> none;
    const Boxes at{{DVec3{4.5, -0.1, -0.1}, DVec3{4.8, 0.1, 0.1}}}; // (grown 2 m for swaying: still +x's alone)
    plan(s, none, 24, at);
    GIANT_CHECK(s.draws().empty());
    plan(s, none);
    plan(s, lights);
    GIANT_CHECK(s.draws().size() == 1); // the face that sees it (+x)
}

GIANT_TEST("local shadows: faces that could not be drawn are drawn again, unshadowed meanwhile") {
    LocalShadows s(16);
    std::vector<LocalLight> lights{lamp(1, {0.0, 0.0, 0.0})};
    plan(s, lights, 4);
    s.undo_draws(2); // the first two drawn, the rest not
    GIANT_CHECK(s.draws().size() == 2);
    plan(s, lights, 24);
    GIANT_CHECK(s.draws().size() == 4 && shadowed(s, 0)->record.drawn == 0x3fu);
    s.invalidate_all();
    plan(s, lights, 4);
    s.undo_draws();
    GIANT_CHECK(s.draws().empty());
    plan(s, lights, 24);
    GIANT_CHECK(s.draws().size() == 6 && shadowed(s, 0)->record.drawn == 0x3fu);
}

GIANT_TEST("local shadows: a spot keeps the right axis it was drawn with, dropped when the axis jumps") {
    LocalShadows s(16);
    std::vector<LocalLight> spot{lamp(1, {0.0, 3.0, 0.0}, LightKind::spot)};
    // Its axis just under 0.9 along x: the right axis is made from x.
    spot[0].direction = normalize_or_zero(Vec3{0.8999f, -std::sqrt(1.0f - 0.8999f * 0.8999f), 0.0f});
    plan(s, spot);
    const Vec3 drawn{shadowed(s, 0)->record.right[0], shadowed(s, 0)->record.right[1], shadowed(s, 0)->record.right[2]};
    // Turned a hair: kept (and its record's right axis is still the drawn one) while drawn again.
    spot[0].direction = normalize_or_zero(Vec3{0.89995f, -std::sqrt(1.0f - 0.89995f * 0.89995f), 0.00001f});
    plan(s, spot, 0);
    GIANT_CHECK(shadowed(s, 0) != nullptr);
    if (shadowed(s, 0) != nullptr) {
        const LightShadowRight r{shadowed(s, 0)->record.right};
        GIANT_CHECK(r.v.x == drawn.x && r.v.y == drawn.y && r.v.z == drawn.z);
    }
    // Over 0.9: made from z now, a quarter turn from before: dropped at once.
    spot[0].direction = normalize_or_zero(Vec3{0.9001f, -std::sqrt(1.0f - 0.9001f * 0.9001f), 0.0f});
    plan(s, spot, 0);
    GIANT_CHECK(shadowed(s, 0) == nullptr);
}
