// The sun's shadow pages on their own (src/shadow_pages.h): windows, page
// requests, the pool, what gets dirtied and what gets drawn first.
#include "giant/geometry/clusters.h"
#include "giant/geometry/mesh.h"
#include "giant_test.h"
#include "gpu_layout.h"
#include "shadow_pages.h"

#include <cmath>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

const Vec3 k_up_sun{0.0f, 1.0f, 0.0f};

struct Requests {
    std::vector<u32> bits;
    explicit Requests(const ShadowPages& s) : bits(s.levels() * (ShadowPages::k_slots / 32), 0u) {}
    // Asks for the page holding a world position on a level.
    void ask(const ShadowPages& s, u32 level, const DVec3& world) {
        const DVec3 l = s.to_light(world);
        const double size = s.page_size(level);
        const u32 slot = s.slot_of(i32(std::floor(l.x / size)), i32(std::floor(l.y / size)));
        bits[level * 8 + slot / 32] |= 1u << (slot % 32);
    }
};

bool has(u32 entry, u32 bit) { return entry != ShadowPages::k_no_page && (entry & bit) != 0u; }

} // namespace

GIANT_TEST("shadow pages: the light basis is orthonormal and looks away from the sun") {
    const Vec3 suns[] = {{0.0f, 1.0f, 0.0f}, {0.3f, 0.8f, -0.52f}, {1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};
    for (const Vec3& sun : suns) {
        const ShadowPages::Basis b = ShadowPages::basis_for(sun);
        GIANT_CHECK_NEAR(length(b.x), 1.0, 1e-9);
        GIANT_CHECK_NEAR(length(b.y), 1.0, 1e-9);
        GIANT_CHECK_NEAR(length(b.z), 1.0, 1e-9);
        GIANT_CHECK_NEAR(dot(b.x, b.y), 0.0, 1e-9);
        GIANT_CHECK_NEAR(dot(b.x, b.z), 0.0, 1e-9);
        GIANT_CHECK_NEAR(dot(b.y, b.z), 0.0, 1e-9);
        const DVec3 s = normalize_or_zero(DVec3{sun.x, sun.y, sun.z});
        GIANT_CHECK_NEAR(dot(b.z, s), -1.0, 1e-6);
    }
    // No direction at all: straight down, still a basis.
    const ShadowPages::Basis none = ShadowPages::basis_for(Vec3{});
    GIANT_CHECK_NEAR(none.z.y, -1.0, 1e-9);
    GIANT_CHECK_NEAR(length(none.x), 1.0, 1e-9);
}

GIANT_TEST("shadow pages: sizes double per level and the pool is a square atlas") {
    ShadowPages s;
    GIANT_CHECK(s.levels() == 12);
    GIANT_CHECK_NEAR(s.page_size(0), 1.0, 1e-12);
    GIANT_CHECK_NEAR(s.page_size(11), 2048.0, 1e-9);
    GIANT_CHECK_NEAR(s.texel_size(0), 1.0 / 128.0, 1e-12);
    GIANT_CHECK(s.atlas_pages_per_axis() == 32);
    ShadowDesc odd;
    odd.pool_pages = 1000;
    odd.levels = 40;
    ShadowPages t(odd);
    GIANT_CHECK(t.atlas_pages_per_axis() == 32);
    GIANT_CHECK(t.levels() == 16);
    GIANT_CHECK(t.stats().capacity == 1000);
}

GIANT_TEST("shadow pages: a page asked for gets room, is drawn once, then stays") {
    ShadowPages s;
    const DVec3 camera{100.0, 5.0, -40.0};
    s.begin_frame(camera, k_up_sun);
    const DVec3 spot = camera + DVec3{0.5, -5.0, 0.5};
    GIANT_CHECK(s.entry_at(0, spot) == ShadowPages::k_no_page); // never asked for
    Requests r(s);
    r.ask(s, 0, spot);
    r.ask(s, 3, spot);
    s.request(r.bits);
    GIANT_CHECK(s.stats().resident == 2);
    const u32 waiting = s.entry_at(0, spot);
    GIANT_CHECK(waiting != ShadowPages::k_no_page);
    GIANT_CHECK(!has(waiting, ShadowPages::k_drawn_bit)); // reads as missing until drawn

    const std::vector<ShadowPages::Draw> draws = s.plan(16);
    GIANT_CHECK(draws.size() == 2);
    GIANT_CHECK(draws[0].level == 3); // coarse first
    GIANT_CHECK(draws[1].level == 0);
    GIANT_CHECK(draws[0].physical != draws[1].physical);
    const u32 drawn = s.entry_at(0, spot);
    GIANT_CHECK(has(drawn, ShadowPages::k_drawn_bit) && has(drawn, ShadowPages::k_draw_bit));
    GIANT_CHECK((drawn & ShadowPages::k_page_mask) == draws[1].physical);
    // The page's own coordinate holds the spot.
    const DVec3 l = s.to_light(spot);
    GIANT_CHECK(draws[1].page_x == i32(std::floor(l.x / s.page_size(0))));
    GIANT_CHECK(draws[1].page_y == i32(std::floor(l.y / s.page_size(0))));

    // The next frame: cached, nothing to draw, the draw bit gone.
    s.begin_frame(camera, k_up_sun);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16).empty());
    const u32 cached = s.entry_at(0, spot);
    GIANT_CHECK(has(cached, ShadowPages::k_drawn_bit) && !has(cached, ShadowPages::k_draw_bit));
    GIANT_CHECK(s.stats().resident == 2);

    // The table says the same.
    std::vector<u32> table(s.levels() * ShadowPages::k_slots);
    s.write_table(table);
    u32 listed = 0;
    for (const u32 e : table) listed += e != ShadowPages::k_no_page ? 1u : 0u;
    GIANT_CHECK(listed == 2);
}

GIANT_TEST("shadow pages: windows follow the camera and pages that leave are freed") {
    ShadowPages s;
    DVec3 camera{0.0, 0.0, 0.0};
    s.begin_frame(camera, k_up_sun);
    const DVec3 spot{0.25, 0.0, 0.25};
    Requests r(s);
    r.ask(s, 0, spot);
    r.ask(s, 5, spot);
    s.request(r.bits);
    s.plan(16);
    GIANT_CHECK(s.stats().resident == 2);
    // Every window holds the camera, the camera near its middle.
    for (u32 level = 0; level < s.levels(); ++level) {
        const DVec3 l = s.to_light(camera);
        const double pages = l.x / s.page_size(level) - double(s.window_x(level));
        GIANT_CHECK(pages >= 7.5 && pages < 8.5);
    }
    // 20 m on: level 0 (16 m across) has left the spot, level 5 (512 m) has not.
    camera = {20.0, 0.0, 20.0};
    s.begin_frame(camera, k_up_sun);
    GIANT_CHECK(s.entry_at(0, spot) == ShadowPages::k_no_page);
    GIANT_CHECK(has(s.entry_at(5, spot), ShadowPages::k_drawn_bit));
    GIANT_CHECK(s.stats().resident == 1);
    // Coming back finds the page gone: asked for again, drawn again.
    camera = {0.0, 0.0, 0.0};
    s.begin_frame(camera, k_up_sun);
    s.request(r.bits);
    GIANT_CHECK(!has(s.entry_at(0, spot), ShadowPages::k_drawn_bit));
    GIANT_CHECK(s.plan(16).size() == 1);

    // Write_levels: the window's corner relative to the camera, small numbers far from the origin.
    camera = {30000.0, 0.0, -25000.0};
    s.begin_frame(camera, k_up_sun);
    std::vector<gpu::ShadowLevel> levels(s.levels());
    s.write_levels(levels, camera);
    for (u32 level = 0; level < s.levels(); ++level) {
        const double size = s.page_size(level);
        GIANT_CHECK(levels[level].origin[0] <= -7.5 * size + 1e-3 && levels[level].origin[0] > -8.5 * size);
        GIANT_CHECK(levels[level].origin[1] <= -7.5 * size + 1e-3 && levels[level].origin[1] > -8.5 * size);
        GIANT_CHECK(levels[level].page[0] == s.window_x(level));
        GIANT_CHECK_NEAR(levels[level].page_size, size, 1e-6 * size);
    }
}

GIANT_TEST("shadow pages: a change dirties the pages under it, on every level") {
    ShadowPages s;
    const DVec3 camera{0.0, 2.0, 0.0};
    s.begin_frame(camera, k_up_sun);
    const DVec3 a{-3.5, 0.0, -3.5};
    const DVec3 b{3.5, 0.0, 3.5};
    Requests r(s);
    r.ask(s, 0, a);
    r.ask(s, 0, b);
    r.ask(s, 6, a);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16).size() == 3);
    s.begin_frame(camera, k_up_sun);
    // A box over `a` only (tall: the sun is straight up, height does not matter).
    s.invalidate(a - DVec3{0.1, 50.0, 0.1}, a + DVec3{0.1, 50.0, 0.1});
    s.request(r.bits);
    const std::vector<ShadowPages::Draw> draws = s.plan(16);
    GIANT_CHECK(draws.size() == 2); // a's level 0 page and the level 6 page
    GIANT_CHECK(draws[0].level == 6 && draws[1].level == 0);
    GIANT_CHECK(has(s.entry_at(0, b), ShadowPages::k_drawn_bit) && !has(s.entry_at(0, b), ShadowPages::k_draw_bit));
    // A dirty page keeps reading as drawn (old content) until redrawn.
    s.begin_frame(camera, k_up_sun);
    s.invalidate_all();
    GIANT_CHECK(has(s.entry_at(0, a), ShadowPages::k_drawn_bit));
    GIANT_CHECK(s.plan(1).size() == 1);
    GIANT_CHECK(s.stats().waiting == 2);
    // Empty and NaN boxes dirty nothing.
    s.begin_frame(camera, k_up_sun);
    s.plan(16);
    s.begin_frame(camera, k_up_sun);
    s.invalidate(DVec3{1.0, 1.0, 1.0}, DVec3{0.0, 0.0, 0.0});
    s.invalidate(DVec3{std::nan(""), 0.0, 0.0}, DVec3{1.0, 1.0, 1.0});
    GIANT_CHECK(s.plan(16).empty());
}

GIANT_TEST("shadow pages: pages under swaying things are drawn again in turn, on levels fine enough") {
    ShadowPages s;
    const DVec3 camera{0.0, 2.0, 0.0};
    s.begin_frame(camera, k_up_sun);
    const DVec3 a{-3.5, 0.0, -3.5};
    const DVec3 b{3.5, 0.0, 3.5};
    Requests r(s);
    r.ask(s, 0, a);
    r.ask(s, 0, b);
    r.ask(s, 6, a);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16).size() == 3);
    const DVec3 tall{0.1, 50.0, 0.1};
    // Swaying 10 cm: level 0's texels (8 mm) show it, level 6's (half a
    // metre) do not. No sway budget: nothing.
    s.begin_frame(camera, k_up_sun);
    s.sway(a - tall, a + tall, 0.1f);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16).empty());
    s.begin_frame(camera, k_up_sun);
    s.sway(a - tall, a + tall, 0.1f);
    s.request(r.bits);
    std::vector<ShadowPages::Draw> draws = s.plan(16, 8);
    GIANT_CHECK(draws.size() == 1 && draws[0].level == 0);
    GIANT_CHECK(s.stats().swayed == 1 && s.stats().drawn == 1);
    GIANT_CHECK(has(s.entry_at(0, a), ShadowPages::k_draw_bit) && !has(s.entry_at(6, a), ShadowPages::k_draw_bit));
    // Two pages sway, one a frame: they take turns, longest drawn first.
    u32 a_drawn = 0, b_drawn = 0;
    for (int frame = 0; frame < 6; ++frame) {
        s.begin_frame(camera, k_up_sun);
        s.sway(a - tall, a + tall, 0.1f);
        s.sway(b - tall, b + tall, 0.1f);
        s.request(r.bits);
        draws = s.plan(16, 1);
        GIANT_CHECK(draws.size() == 1);
        a_drawn += has(s.entry_at(0, a), ShadowPages::k_draw_bit) ? 1u : 0u;
        b_drawn += has(s.entry_at(0, b), ShadowPages::k_draw_bit) ? 1u : 0u;
    }
    GIANT_CHECK(a_drawn == 3 && b_drawn == 3);
    // A page dirty for another reason is drawn once, not twice.
    s.begin_frame(camera, k_up_sun);
    s.invalidate(a - tall, a + tall);
    s.sway(a - tall, a + tall, 0.1f);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16, 8).size() == 2); // a's two pages, by the change
    // Swaying a metre reaches level 6 too; bad boxes and reaches do nothing.
    s.begin_frame(camera, k_up_sun);
    s.sway(a - tall, a + tall, 1.0f);
    s.sway(DVec3{1.0, 1.0, 1.0}, DVec3{0.0, 0.0, 0.0}, 1.0f);
    s.sway(b - tall, b + tall, std::nanf(""));
    s.sway(b - tall, b + tall, 0.0f);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16, 8).size() == 2);
    // Pages shading stopped asking for are not drawn again for swaying.
    for (int frame = 0; frame < 40; ++frame) {
        s.begin_frame(camera, k_up_sun);
        s.plan(16);
    }
    s.begin_frame(camera, k_up_sun);
    s.sway(a - tall, a + tall, 1.0f);
    GIANT_CHECK(s.plan(16, 8).empty());
}

GIANT_TEST("shadow pages: a new sun direction dirties everything") {
    ShadowPages s;
    const DVec3 camera{0.0, 0.0, 0.0};
    s.begin_frame(camera, k_up_sun);
    Requests r(s);
    for (u32 level = 0; level < 4; ++level) r.ask(s, level, camera);
    s.request(r.bits);
    GIANT_CHECK(s.plan(64).size() == 4);
    s.begin_frame(camera, Vec3{0.3f, 0.9f, 0.1f});
    GIANT_CHECK(s.plan(64).size() == 4);
    s.begin_frame(camera, Vec3{0.3f, 0.9f, 0.1f});
    GIANT_CHECK(s.plan(64).empty());
}

GIANT_TEST("shadow pages: a sun step keeps the old view standing in until the new one is drawn") {
    ShadowPages s;
    const DVec3 camera{100.0, 0.0, 200.0};
    const Vec3 sun = normalize_or_zero(Vec3{0.2f, 0.9f, 0.1f});
    s.begin_frame(camera, sun);
    Requests r(s);
    for (u32 level = 0; level < 6; ++level) r.ask(s, level, camera);
    s.request(r.bits);
    GIANT_CHECK(s.plan(64).size() == 6);
    GIANT_CHECK(!s.has_previous() && s.stats().resident == 6);
    std::vector<u32> table(2 * size_t{s.levels()} * ShadowPages::k_slots);
    s.write_table(table);
    GIANT_CHECK(table[size_t{s.levels()} * ShadowPages::k_slots] == ShadowPages::k_no_page);
    // A step of the sun: the drawn pages are the previous view's now, the
    // same pages asked for again at once in the new one, not drawn yet.
    const Vec3 moved = normalize_or_zero(Vec3{0.21f, 0.9f, 0.1f});
    s.begin_frame(camera, moved);
    GIANT_CHECK(s.has_previous());
    for (u32 level = 0; level < 6; ++level) {
        GIANT_CHECK(has(s.previous_entry_at(level, camera), ShadowPages::k_drawn_bit));
        GIANT_CHECK(s.entry_at(level, camera) != ShadowPages::k_no_page);
        GIANT_CHECK(!has(s.entry_at(level, camera), ShadowPages::k_drawn_bit));
    }
    GIANT_CHECK(s.stats().resident == 12);
    // Its table and levels go after the current view's.
    s.write_table(table);
    usize held = 0;
    for (usize i = size_t{s.levels()} * ShadowPages::k_slots; i < table.size(); ++i) {
        held += has(table[i], ShadowPages::k_drawn_bit) ? 1u : 0u;
    }
    GIANT_CHECK(held == 6);
    std::vector<gpu::ShadowLevel> levels(2 * size_t{s.levels()});
    s.write_levels(levels, camera);
    const ShadowPages::Basis& before = s.previous_basis();
    const double size = s.page_size(0);
    GIANT_CHECK(levels[s.levels()].page_size == static_cast<float>(size));
    const float corner = static_cast<float>(double(levels[s.levels()].page[0]) * size - dot(camera, before.x));
    GIANT_CHECK(std::fabs(levels[s.levels()].origin[0] - corner) < 1e-3f);
    // Drawn a few at a time: the previous view stays while any is still to
    // come, and a few frames more (shading's requests in the new view)...
    GIANT_CHECK(s.plan(4).size() == 4);
    GIANT_CHECK(s.has_previous());
    s.begin_frame(camera, moved);
    GIANT_CHECK(s.plan(4).size() == 2);
    GIANT_CHECK(s.has_previous());
    // ...and is given back then.
    for (u64 f = 0; f < ShadowPages::k_previous_min_frames; ++f) {
        s.begin_frame(camera, moved);
        s.plan(4);
    }
    GIANT_CHECK(!s.has_previous());
    GIANT_CHECK(s.stats().resident == 6);
    for (u32 level = 0; level < 6; ++level) GIANT_CHECK(has(s.entry_at(level, camera), ShadowPages::k_drawn_bit));
}

GIANT_TEST("shadow pages: the previous view gives its pages up coarse first and only as asked, and outlasts a thinner view") {
    ShadowDesc desc;
    desc.pool_pages = 4;
    ShadowPages s(desc);
    const DVec3 camera{0.0, 0.0, 0.0};
    const Vec3 a = normalize_or_zero(Vec3{0.0f, 1.0f, 0.1f});
    const Vec3 b = normalize_or_zero(Vec3{0.0f, 1.0f, 0.12f});
    const Vec3 c = normalize_or_zero(Vec3{0.0f, 1.0f, 0.14f});
    s.begin_frame(camera, a);
    Requests r(s);
    for (u32 level = 0; level < 3; ++level) r.ask(s, level, camera);
    s.request(r.bits);
    s.plan(16);
    // A step: the 3 drawn held by the previous view; asked again in the
    // new view only into the pool's free page (none of the previous view's
    // goes before anything is drawn).
    s.begin_frame(camera, b);
    GIANT_CHECK(s.has_previous() && s.stats().resident == 4 && s.stats().missing == 0);
    GIANT_CHECK(s.entry_at(0, camera) != ShadowPages::k_no_page);
    for (u32 level = 0; level < 3; ++level) GIANT_CHECK(has(s.previous_entry_at(level, camera), ShadowPages::k_drawn_bit));
    // Shading asks for the rest: the previous view makes room, coarse levels first.
    Requests again(s);
    for (u32 level = 0; level < 3; ++level) again.ask(s, level, camera);
    s.request(again.bits);
    for (u32 level = 0; level < 3; ++level) GIANT_CHECK(s.entry_at(level, camera) != ShadowPages::k_no_page);
    GIANT_CHECK(has(s.previous_entry_at(0, camera), ShadowPages::k_drawn_bit));
    GIANT_CHECK(s.previous_entry_at(1, camera) == ShadowPages::k_no_page);
    GIANT_CHECK(s.previous_entry_at(2, camera) == ShadowPages::k_no_page);
    // Another step before the new view was drawn: the previous view (more
    // drawn) stays, the thinner one's pages go; nothing leaks.
    s.begin_frame(camera, c);
    GIANT_CHECK(s.has_previous() && has(s.previous_entry_at(0, camera), ShadowPages::k_drawn_bit));
    GIANT_CHECK(s.stats().resident == 4);
    s.plan(16);
    s.begin_frame(camera, b);
    GIANT_CHECK(s.has_previous());
    // Not drawn for long (no budget asks it away): given back in time.
    for (u64 f = 0; f <= ShadowPages::k_previous_frames + 1; ++f) s.begin_frame(camera, b);
    GIANT_CHECK(!s.has_previous());
    GIANT_CHECK(s.stats().resident <= 4);
}

GIANT_TEST("shadow pages: a sun step with the pool over half full keeps the finest levels standing in") {
    ShadowPages s; // 1024 pages
    const DVec3 camera{0.0, 0.0, 0.0};
    const Vec3 sun = normalize_or_zero(Vec3{0.1f, 1.0f, 0.1f});
    s.begin_frame(camera, sun);
    Requests r(s);
    for (u32 level = 0; level < 3; ++level) {
        for (u32 word = 0; word < 8; ++word) r.bits[level * 8 + word] = ~0u; // whole windows
    }
    s.request(r.bits);
    for (int f = 0; f < 20; ++f) s.plan(64);
    GIANT_CHECK(s.stats().resident == 768);
    const Vec3 moved = normalize_or_zero(Vec3{0.105f, 1.0f, 0.1f});
    s.begin_frame(camera, moved);
    GIANT_CHECK(s.has_previous() && s.stats().missing == 0);
    // All 768 still held by the previous view; the new view got the 256 free.
    usize held[3] = {};
    for (u32 level = 0; level < 3; ++level) {
        for (u32 slot = 0; slot < ShadowPages::k_slots; ++slot) {
            const double size = s.page_size(level);
            const DVec3 at = s.previous_basis().x * ((double(slot % 16) - 7.5) * size) +
                             s.previous_basis().y * ((double(slot / 16) - 7.5) * size);
            held[level] += has(s.previous_entry_at(level, at), ShadowPages::k_drawn_bit) ? 1u : 0u;
        }
    }
    GIANT_CHECK(held[0] == 256 && held[1] == 256 && held[2] == 256);
    GIANT_CHECK(s.stats().resident == 1024);
    // Shading asks for everything: a budget a frame comes from the previous
    // view, coarse first: level 0 stays while the new view fills in.
    s.request(r.bits);
    GIANT_CHECK(has(s.previous_entry_at(0, camera), ShadowPages::k_drawn_bit));
}

GIANT_TEST("shadow pages: a full pool gives the page used longest ago") {
    ShadowDesc desc;
    desc.pool_pages = 3;
    ShadowPages s(desc);
    const DVec3 camera{0.0, 0.0, 0.0};
    const DVec3 spots[] = {{-6.5, 0.0, -6.5}, {-4.5, 0.0, -6.5}, {-2.5, 0.0, -6.5}, {0.5, 0.0, -6.5}};
    for (u32 i = 0; i < 3; ++i) {
        s.begin_frame(camera, k_up_sun);
        Requests r(s);
        r.ask(s, 0, spots[i]);
        if (i == 2) r.ask(s, 0, spots[0]); // spots[0] used again: spots[1] is now the oldest
        s.request(r.bits);
        s.plan(16);
    }
    GIANT_CHECK(s.stats().resident == 3);
    s.begin_frame(camera, k_up_sun);
    Requests r(s);
    r.ask(s, 0, spots[3]);
    s.request(r.bits);
    GIANT_CHECK(s.entry_at(0, spots[1]) == ShadowPages::k_no_page);
    GIANT_CHECK(s.entry_at(0, spots[0]) != ShadowPages::k_no_page);
    GIANT_CHECK(s.entry_at(0, spots[3]) != ShadowPages::k_no_page);
    GIANT_CHECK(s.stats().missing == 0);
    // All in use at once: one has to go without.
    s.begin_frame(camera, k_up_sun);
    Requests all(s);
    for (const DVec3& p : spots) all.ask(s, 0, p);
    s.request(all.bits);
    GIANT_CHECK(s.stats().resident == 3);
    GIANT_CHECK(s.stats().missing == 1);
    // The pages already there and asked for keep their room: the new one goes without.
    GIANT_CHECK(s.entry_at(0, spots[1]) == ShadowPages::k_no_page);
    GIANT_CHECK(has(s.entry_at(0, spots[2]), ShadowPages::k_drawn_bit));
}

GIANT_TEST("shadow pages: budgets draw new pages first, then coarse levels, at least one") {
    ShadowPages s;
    const DVec3 camera{0.0, 0.0, 0.0};
    s.begin_frame(camera, k_up_sun);
    Requests first(s);
    first.ask(s, 0, camera);
    s.request(first.bits);
    s.plan(16);
    s.begin_frame(camera, k_up_sun);
    s.invalidate_all(); // level 0 dirty, drawn before
    Requests more(s);
    more.ask(s, 0, camera);
    more.ask(s, 1, camera);
    more.ask(s, 4, camera);
    s.request(more.bits);
    std::vector<ShadowPages::Draw> d = s.plan(0); // 0 still draws one
    GIANT_CHECK(d.size() == 1 && d[0].level == 4);
    GIANT_CHECK(s.stats().waiting == 2);
    s.begin_frame(camera, k_up_sun);
    d = s.plan(1);
    GIANT_CHECK(d.size() == 1 && d[0].level == 1); // never drawn beats dirty
    s.begin_frame(camera, k_up_sun);
    d = s.plan(1);
    GIANT_CHECK(d.size() == 1 && d[0].level == 0);
    GIANT_CHECK(s.stats().waiting == 0);
}

GIANT_TEST("shadow pages: tracking casters dirties where they were and where they are") {
    const geometry::ClusterMesh cube = geometry::build_clusters(geometry::make_cube());
    const ShadowPages::MeshLookup meshes = [&](MeshId id) { return id == 1 ? &cube : nullptr; };
    ShadowPages s;
    const DVec3 camera{0.0, 0.0, 0.0};
    const DVec3 a{-5.5, 0.0, -5.5};
    const DVec3 b{5.5, 0.0, 5.5};
    const DVec3 c{-5.5, 0.0, 5.5};
    std::vector<Instance> instances(1);
    instances[0].position = a;
    instances[0].mesh = 1;
    s.begin_frame(camera, k_up_sun);
    s.track(instances, {}, {}, 1, meshes);
    Requests r(s);
    r.ask(s, 0, a);
    r.ask(s, 0, b);
    r.ask(s, 0, c);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16).size() == 3);
    const auto settle = [&] {
        s.begin_frame(camera, k_up_sun);
        s.track(instances, {}, {}, 1, meshes);
        s.request(r.bits);
        return s.plan(16);
    };
    GIANT_CHECK(settle().empty());
    // A new material changes no shadow.
    instances[0].material = 3;
    GIANT_CHECK(settle().empty());
    // Moved from a to b: both redrawn, c not.
    instances[0].position = b;
    std::vector<ShadowPages::Draw> d = settle();
    GIANT_CHECK(d.size() == 2);
    GIANT_CHECK(has(s.entry_at(0, a), ShadowPages::k_draw_bit) && has(s.entry_at(0, b), ShadowPages::k_draw_bit));
    GIANT_CHECK(!has(s.entry_at(0, c), ShadowPages::k_draw_bit));
    // An unknown mesh casts nothing: only where it was (b) is redrawn.
    instances[0].mesh = 7;
    d = settle();
    GIANT_CHECK(d.size() == 1 && has(s.entry_at(0, b), ShadowPages::k_draw_bit));
    // Added at c, then removed.
    instances[0].mesh = 1;
    settle();
    instances.push_back(instances[0]);
    instances[1].position = c;
    d = settle();
    GIANT_CHECK(d.size() == 1 && has(s.entry_at(0, c), ShadowPages::k_draw_bit));
    instances.pop_back();
    d = settle();
    GIANT_CHECK(d.size() == 1 && has(s.entry_at(0, c), ShadowPages::k_draw_bit));
    GIANT_CHECK(settle().empty());
    // A new mesh table that leaves the bounds alone dirties nothing.
    s.begin_frame(camera, k_up_sun);
    s.track(instances, {}, {}, 2, meshes);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16).empty());
}

GIANT_TEST("shadow pages: many changes at once dirty everything") {
    const geometry::ClusterMesh cube = geometry::build_clusters(geometry::make_cube());
    const ShadowPages::MeshLookup meshes = [&](MeshId) { return &cube; };
    ShadowPages s;
    const DVec3 camera{0.0, 0.0, 0.0};
    std::vector<Instance> instances(300);
    for (u32 i = 0; i < instances.size(); ++i) {
        instances[i].position = {5000.0 + i * 40.0, 0.0, 5000.0}; // far from every page asked for, a square each
        instances[i].mesh = 1;
    }
    s.begin_frame(camera, k_up_sun);
    s.track(instances, {}, {}, 1, meshes);
    Requests r(s);
    r.ask(s, 0, camera);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16).size() == 1);
    // A few moving far away: the page near the camera is left alone.
    for (u32 i = 0; i < 10; ++i) instances[i].position.y += 1.0;
    s.begin_frame(camera, k_up_sun);
    s.track(instances, {}, {}, 1, meshes);
    GIANT_CHECK(s.plan(16).empty());
    // All of them: too many squares to list, so everything is redrawn.
    for (Instance& in : instances) in.position.y += 1.0;
    s.begin_frame(camera, k_up_sun);
    s.track(instances, {}, {}, 1, meshes);
    GIANT_CHECK(s.plan(16).size() == 1);
    // Thousands arriving in a few squares far away (a scatter cell): gathered,
    // so the page near the camera is left alone.
    std::vector<Instance> more = instances;
    for (u32 i = 0; i < 3000; ++i) {
        Instance in;
        in.position = {-5000.0 + (i % 60) * 1.0, 0.0, -5000.0 + (i / 60) * 1.0};
        in.mesh = 1;
        more.push_back(in);
    }
    s.begin_frame(camera, k_up_sun);
    s.track(more, {}, {}, 1, meshes);
    GIANT_CHECK(s.plan(16).empty());
}

GIANT_TEST("shadow pages: a caster that starts or stops being drawn dirties its pages") {
    const geometry::ClusterMesh cube = geometry::build_clusters(geometry::make_cube());
    const ShadowPages::MeshLookup meshes = [&](MeshId) { return &cube; };
    ShadowPages s;
    const DVec3 camera{0.0, 0.0, 0.0};
    std::vector<Instance> instances(2);
    instances[0].position = {-5.5, 0.0, -5.5};
    instances[1].position = {5.5, 0.0, 5.5};
    std::vector<u8> casts{1, 1};
    s.begin_frame(camera, k_up_sun);
    s.track(instances, {}, casts, 1, meshes);
    Requests r(s);
    r.ask(s, 0, instances[0].position);
    r.ask(s, 0, instances[1].position);
    s.request(r.bits);
    GIANT_CHECK(s.plan(16).size() == 2);
    const auto frame = [&] {
        s.begin_frame(camera, k_up_sun);
        s.track(instances, {}, casts, 1, meshes);
        return s.plan(16);
    };
    GIANT_CHECK(frame().empty());
    casts[1] = 0; // left to the voxel world, say
    std::vector<ShadowPages::Draw> d = frame();
    GIANT_CHECK(d.size() == 1 && has(s.entry_at(0, instances[1].position), ShadowPages::k_draw_bit));
    GIANT_CHECK(frame().empty());
    // Moving while not cast dirties nothing.
    instances[1].position.y += 3.0;
    GIANT_CHECK(frame().empty());
    casts[1] = 1;
    d = frame();
    GIANT_CHECK(d.size() == 1 && has(s.entry_at(0, instances[1].position), ShadowPages::k_draw_bit));
}

GIANT_TEST("shadow pages: every level gets its turn however often things change") {
    ShadowPages s;
    const DVec3 camera{0.0, 0.0, 0.0};
    s.begin_frame(camera, k_up_sun);
    Requests r(s);
    for (u32 level = 0; level < 6; ++level) r.ask(s, level, camera);
    s.request(r.bits);
    GIANT_CHECK(s.plan(64).size() == 6);
    // Everything changes every frame (a giant walking through), one page a frame.
    std::vector<u32> drawn(6, 0);
    for (int f = 0; f < 6; ++f) {
        s.begin_frame(camera, k_up_sun);
        s.invalidate_all();
        s.request(r.bits);
        for (const ShadowPages::Draw& d : s.plan(1)) ++drawn[d.level];
    }
    for (u32 level = 0; level < 6; ++level) GIANT_CHECK(drawn[level] == 1);
}

GIANT_TEST("shadow pages: pages not asked for lately wait; old requests for moved windows are dropped") {
    ShadowPages s;
    DVec3 camera{0.0, 0.0, 0.0};
    s.begin_frame(camera, k_up_sun);
    Requests r(s);
    r.ask(s, 0, camera);
    s.request(r.bits);
    s.plan(16);
    for (int f = 0; f < 40; ++f) s.begin_frame(camera, k_up_sun); // not asked for since
    s.invalidate_all();
    GIANT_CHECK(s.plan(16).empty() && s.stats().waiting == 0); // drawn and stale: waits to be asked
    s.request(r.bits); // asked for again: drawn
    GIANT_CHECK(s.plan(16).size() == 1);
    // Requests made while the window was elsewhere: the slot holds another page now.
    const ShadowPages::Windows before = s.windows();
    Requests here(s);
    here.ask(s, 0, camera + DVec3{0.0, 0.0, 6.5}); // light x = -6.5: near the window's low edge
    camera = {0.0, 0.0, -9.0};                     // light x +9: the window moves by 9 pages
    s.begin_frame(camera, k_up_sun);
    const u32 before_resident = s.stats().resident;
    s.request(here.bits, &before);
    GIANT_CHECK(s.stats().resident == before_resident); // dropped, not taken for the page now in that slot
    const ShadowPages::Windows now = s.windows();
    Requests fresh(s);
    fresh.ask(s, 0, camera);
    s.request(fresh.bits, &now);
    GIANT_CHECK(s.stats().resident == before_resident + 1);
}

GIANT_TEST("shadow pages: depths come from an anchor; tiny sun moves are not taken up") {
    ShadowPages s;
    const Vec3 sun = normalize_or_zero(Vec3{0.0f, 0.7f, 0.7f});
    DVec3 camera{0.0, 0.0, 0.0};
    s.begin_frame(camera, sun);
    GIANT_CHECK_NEAR(s.depth_offset(camera), 0.0, 1e-12);
    Requests r(s);
    r.ask(s, 0, camera);
    s.request(r.bits);
    s.plan(16);
    // Walking towards the sun: depths keep their anchor, cached pages stay.
    camera = camera + DVec3{sun.x, sun.y, sun.z} * 3.0;
    s.begin_frame(camera, sun);
    GIANT_CHECK_NEAR(s.depth_offset(camera), 3.0 / 40000.0, 1e-7); // 3 m nearer the sun
    // A tenth of a degree: kept as it was.
    const float a = 0.1f * k_pi / 180.0f;
    s.begin_frame(camera, normalize_or_zero(Vec3{0.0f, std::cos(0.785398f + a), std::sin(0.785398f + a)}));
    s.request(r.bits);
    GIANT_CHECK(s.plan(16).empty());
    // Half a degree: every page reads as missing until drawn again.
    const float b = 0.5f * k_pi / 180.0f;
    s.begin_frame(camera, normalize_or_zero(Vec3{0.0f, std::cos(0.785398f + b), std::sin(0.785398f + b)}));
    s.request(r.bits);
    GIANT_CHECK(!has(s.entry_at(0, camera), ShadowPages::k_drawn_bit));
    GIANT_CHECK(!s.plan(16).empty());
    // Past a quarter of the depth range from the anchor: a new anchor, pages drawn again.
    for (int f = 0; f < 3; ++f) {
        s.begin_frame(camera, normalize_or_zero(Vec3{0.0f, std::cos(0.785398f + b), std::sin(0.785398f + b)}));
        s.request(r.bits);
        s.plan(16);
    }
    const ShadowPages::Basis& basis = s.basis();
    const DVec3 far = camera - basis.z * 10001.0;
    Requests coarse(s);
    coarse.ask(s, 11, camera);
    s.request(coarse.bits);
    s.plan(16);
    GIANT_CHECK(has(s.entry_at(11, camera), ShadowPages::k_drawn_bit));
    s.begin_frame(far, normalize_or_zero(Vec3{0.0f, std::cos(0.785398f + b), std::sin(0.785398f + b)}));
    GIANT_CHECK_NEAR(s.depth_offset(far), 0.0, 1e-12);
    // The level 11 page still holds `far` (10 km on, along the light only): drawn again.
    GIANT_CHECK(s.entry_at(11, far) != ShadowPages::k_no_page);
    GIANT_CHECK(!has(s.entry_at(11, far), ShadowPages::k_drawn_bit));
    const std::vector<ShadowPages::Draw> again = s.plan(16);
    GIANT_CHECK(!again.empty() && has(s.entry_at(11, far), ShadowPages::k_drawn_bit));
}

GIANT_TEST("shadow pages: with cells, only cells whose version changed are compared") {
    const geometry::ClusterMesh cube = geometry::build_clusters(geometry::make_cube());
    const ShadowPages::MeshLookup meshes = [&](MeshId id) { return id == 1 ? &cube : nullptr; };
    ShadowPages s;
    const DVec3 camera{0.0, 0.0, 0.0};
    const DVec3 a{-5.5, 0.0, -5.5};
    const DVec3 b{5.5, 0.0, 5.5};
    std::vector<Instance> instances(2);
    instances[0].position = a;
    instances[1].position = b;
    instances[0].mesh = instances[1].mesh = 1;
    std::vector<CellRange> cells{{0, 1, 1}, {1, 1, 2}};
    Requests r(s);
    r.ask(s, 0, a);
    r.ask(s, 0, b);
    const auto settle = [&] {
        s.begin_frame(camera, k_up_sun);
        s.track(instances, cells, {}, 1, meshes);
        s.request(r.bits);
        return s.plan(16);
    };
    GIANT_CHECK(settle().size() == 2);
    GIANT_CHECK(settle().empty());
    // Moved without a new version: the cell promises nothing changed.
    instances[1].position = a;
    GIANT_CHECK(settle().empty());
    // A new version: compared, both places redrawn.
    cells[1].version = 3;
    GIANT_CHECK(settle().size() == 2);
    // Another layout: everything compared.
    instances[1].position = b;
    cells = {{0, 2, 4}};
    GIANT_CHECK(settle().size() == 2);
    GIANT_CHECK(settle().empty());
}

GIANT_TEST("shadow pages: meshes coming and going redraw only the casters whose mesh bounds changed") {
    const geometry::ClusterMesh cube = geometry::build_clusters(geometry::make_cube());
    geometry::ClusterMesh bigger = cube;
    bigger.bounds.radius *= 3.0f;
    const geometry::ClusterMesh* second = &cube; // what mesh 2 is now
    const ShadowPages::MeshLookup meshes = [&](MeshId id) {
        return id == 1 ? &cube : id == 2 ? second : nullptr;
    };
    ShadowPages s;
    const DVec3 camera{0.0, 0.0, 0.0};
    const DVec3 a{-5.5, 0.0, -5.5};
    const DVec3 b{5.5, 0.0, 5.5};
    std::vector<Instance> instances(2);
    instances[0].position = a;
    instances[1].position = b;
    instances[0].mesh = 1;
    instances[1].mesh = 2;
    const std::vector<CellRange> cells{{0, 1, 1}, {1, 1, 2}};
    Requests r(s);
    r.ask(s, 0, a);
    r.ask(s, 0, b);
    u64 version = 1;
    const auto settle = [&] {
        s.begin_frame(camera, k_up_sun);
        s.track(instances, cells, {}, version, meshes);
        s.request(r.bits);
        return s.plan(16);
    };
    GIANT_CHECK(settle().size() == 2);
    GIANT_CHECK(settle().empty());
    // Other meshes added and removed (the table's version moves): nothing to redraw.
    for (int i = 0; i < 5; ++i) {
        ++version;
        GIANT_CHECK(settle().empty());
    }
    // Mesh 2's slot now holds a bigger mesh, its cell unchanged: its caster is redrawn.
    second = &bigger;
    ++version;
    const std::vector<ShadowPages::Draw> drawn = settle();
    GIANT_CHECK(!drawn.empty());
    GIANT_CHECK(has(s.entry_at(0, b), ShadowPages::k_drawn_bit));
    GIANT_CHECK(settle().empty());
    // Mesh 2 removed: the caster stops casting, its pages are redrawn.
    second = nullptr;
    ++version;
    GIANT_CHECK(!settle().empty());
    GIANT_CHECK(settle().empty());
}

GIANT_TEST("shadow pages: a mesh arriving for a caster that had none casts; bogus ids cost nothing") {
    const geometry::ClusterMesh cube = geometry::build_clusters(geometry::make_cube());
    bool arrived = false;
    const ShadowPages::MeshLookup meshes = [&](MeshId id) { return id == 3 && arrived ? &cube : nullptr; };
    ShadowPages s;
    const DVec3 camera{0.0, 0.0, 0.0};
    const DVec3 a{-5.5, 0.0, -5.5};
    std::vector<Instance> instances(2);
    instances[0].position = a;
    instances[0].mesh = 3;          // not loaded yet
    instances[1].mesh = 0xFFFFFFF0u; // never a real id
    const std::vector<CellRange> cells{{0, 2, 1}};
    Requests r(s);
    r.ask(s, 0, a);
    u64 version = 1;
    const auto settle = [&] {
        s.begin_frame(camera, k_up_sun);
        s.track(instances, cells, {}, version, meshes);
        s.request(r.bits);
        return s.plan(16);
    };
    GIANT_CHECK(settle().size() == 1); // drawn once, empty
    GIANT_CHECK(settle().empty());
    // Mesh 3 arrives, the cell as it was: its caster is redrawn.
    arrived = true;
    ++version;
    GIANT_CHECK(!settle().empty());
    GIANT_CHECK(settle().empty());
}
