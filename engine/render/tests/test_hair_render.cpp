// Hair (src/hair.h): grooms packed for the GPU, what a frame draws of them,
// and hair through a frame on the null device and on D3D12.
#include "hair.h"

#include "giant/geometry/groom.h"
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

// A curtain of straight strands hanging from y = 0.5 to -0.5, their roots
// spread along x from -0.5 to 0.5 (the same strands whatever the count's
// order: groom_from_strands shuffles them into an even order).
geometry::Groom curtain(u32 strands, u32 guides) {
    std::vector<std::vector<Vec3>> lines(strands);
    for (u32 s = 0; s < strands; ++s) {
        const float x = (static_cast<float>(s) + 0.5f) / static_cast<float>(strands) - 0.5f;
        lines[s] = {{x, 0.5f, 0.0f}, {x, -0.5f, 0.0f}};
    }
    return geometry::groom_from_strands(lines, guides);
}

HairView looking(float pixels_high = 1080.0f) {
    // Down -Z from the origin, 60 degrees up and down, as wide as high.
    HairView view;
    const float t = std::tan(degrees_to_radians(30.0f));
    const float n = 1.0f / std::sqrt(1.0f + t * t);
    view.planes[0] = {n, 0.0f, -t * n, 0.0f};
    view.planes[1] = {-n, 0.0f, -t * n, 0.0f};
    view.planes[2] = {0.0f, n, -t * n, 0.0f};
    view.planes[3] = {0.0f, -n, -t * n, 0.0f};
    view.planes[4] = {0.0f, 0.0f, -1.0f, -0.1f};
    view.pixels_per_metre_at_one = 0.5f / t * pixels_high;
    return view;
}

// A groom as the renderer would hold it once uploaded.
GroomId held(Grooms& grooms, const geometry::Groom& groom) {
    const GroomId id = grooms.add(pack_groom(groom));
    GroomSlot* slot = grooms.find(id);
    if (slot != nullptr) {
        slot->uploaded = slot->size;
        slot->buffer = {1, 1};
        grooms.waiting().clear();
    }
    return id;
}

} // namespace

GIANT_TEST("hair: a groom packs to 16 bits an axis, its guides and weights kept; bad ones are refused") {
    geometry::GroomParams params;
    params.strands = 3000;
    params.guides = 64;
    params.curl_radius = 0.01f;
    params.curl_turns = 3.0f;
    const geometry::Groom groom = geometry::generate_groom(geometry::make_sphere(), params);
    const PackedGroom packed = pack_groom(groom);
    GIANT_CHECK(packed.strands == 3000 && packed.guides == 64);
    GIANT_CHECK(packed.bytes.size() == 3000u * 16u + 3000u * 16u * 8u && packed.points_offset == 3000u * 16u);
    GIANT_CHECK_NEAR(packed.longest, groom.longest, 1e-5);
    float worst = 0.0f;
    for (u32 s = 0; s < 3000; ++s) {
        for (u32 k = 0; k < geometry::k_strand_points; ++k) {
            worst = std::max(worst, length(unpack_point(packed, s, k) - groom.strand(s)[k]));
        }
    }
    std::printf("hair: packed points are within %.7f m of the groom's\n", static_cast<double>(worst));
    GIANT_CHECK(worst < 2e-5f);
    for (u32 s : {0u, 63u, 64u, 1500u, 2999u}) {
        gpu::HairStrand strand{};
        std::memcpy(&strand, packed.bytes.data() + s * sizeof(strand), sizeof(strand));
        const geometry::StrandFollow& follow = groom.follow[s];
        GIANT_CHECK((strand.guides01 & 0xffffu) == follow.guides[0] && strand.guides01 >> 16 == follow.guides[1]);
        GIANT_CHECK((strand.guide2_weight0 & 0xffffu) == follow.guides[2]);
        GIANT_CHECK_NEAR(static_cast<float>(strand.guide2_weight0 >> 16) / 65535.0f, follow.weights[0], 1e-4);
        GIANT_CHECK_NEAR(static_cast<float>(strand.weight1) / 65535.0f, follow.weights[1], 1e-4);
        GIANT_CHECK((strand.guide2_weight0 >> 16) + strand.weight1 <= 65535u); // the third's share is never negative
        GIANT_CHECK(strand.seed == groom.seeds[s]);
    }
    // No strands, or a groom with something wrong: nothing to draw.
    GIANT_CHECK(pack_groom(geometry::Groom{}).bytes.empty());
    geometry::Groom broken = groom;
    broken.points[40].y = std::numeric_limits<float>::quiet_NaN();
    GIANT_CHECK(pack_groom(broken).bytes.empty());
    // A flat groom (a curtain: no depth) still spans something.
    const PackedGroom flat = pack_groom(curtain(100, 10));
    GIANT_CHECK(flat.strands == 100 && flat.bounds_size.z > 0.0f);
    GIANT_CHECK(length(unpack_point(flat, 0, 0) - curtain(100, 10).strand(0)[0]) < 2e-5f);
}

GIANT_TEST("hair: its colour goes from white through blond and brown to black, redder with redness") {
    HairLook look;
    look.melanin = 0.0f;
    const Vec3 white = hair_color(look);
    GIANT_CHECK(white.x > 0.85f && white.y > 0.85f && white.z > 0.85f);
    Vec3 last = white;
    for (float m : {0.25f, 0.5f, 0.75f, 1.0f}) {
        look.melanin = m;
        const Vec3 c = hair_color(look);
        GIANT_CHECK(c.x < last.x && c.y < last.y && c.z < last.z); // darker
        GIANT_CHECK(c.x > c.y && c.y > c.z);                       // warm: blue goes first
        last = c;
    }
    GIANT_CHECK(last.x < 0.05f); // black
    look.melanin = 0.5f;
    look.redness = 0.0f;
    const Vec3 brown = hair_color(look);
    look.redness = 1.0f;
    const Vec3 ginger = hair_color(look);
    GIANT_CHECK(ginger.x > brown.x * 1.3f && ginger.x / ginger.z > 3.0f);
    // Dyed: no melanin, the dye's colour. Bad numbers take the defaults.
    look.melanin = 0.0f;
    look.tint = {0.1f, 0.2f, 0.9f};
    const Vec3 blue = hair_color(look);
    GIANT_CHECK(blue.z > blue.x * 5.0f);
    look.melanin = std::numeric_limits<float>::quiet_NaN();
    look.tint = {std::numeric_limits<float>::infinity(), -1.0f, 2.0f};
    const Vec3 odd = hair_color(look);
    GIANT_CHECK(std::isfinite(odd.x + odd.y + odd.z) && odd.y == 0.0f && odd.x <= 1.0f && odd.z <= 1.0f);
}

GIANT_TEST("hair: fewer, wider strands the smaller it is on screen, covering the same; short pieces are joined") {
    const float width = 0.00008f;
    // Close enough that a strand is a third of a pixel: all of them, as they are.
    const HairLod close = hair_lod(100000, width, 0.3f, 1.0f / (3.0f * width) * 1.01f, 3.0f, 0);
    GIANT_CHECK(close.strands == 100000 && close.widen == 1.0f && close.stride == 1);
    // Ten times as far: a tenth of them, each ten times as wide.
    const HairLod far = hair_lod(100000, width, 0.3f, 1.0f / (30.0f * width), 3.0f, 0);
    GIANT_CHECK(far.strands >= 10000 && far.strands <= 10001);
    GIANT_CHECK_NEAR(far.widen * static_cast<float>(far.strands), 100000.0, 1.0);
    // More detail: more of them.
    GIANT_CHECK(hair_lod(100000, width, 0.3f, 1.0f / (30.0f * width), 6.0f, 0).strands >= 20000);
    // Never fewer than a few dozen (or all it has), never more than asked for.
    GIANT_CHECK(hair_lod(100000, width, 0.3f, 0.01f, 3.0f, 0).strands == k_hair_least_strands);
    GIANT_CHECK(hair_lod(20, width, 0.3f, 0.01f, 3.0f, 0).strands == 20);
    const HairLod capped = hair_lod(100000, width, 0.3f, 1.0e6f, 3.0f, 5000);
    GIANT_CHECK(capped.strands == 5000 && capped.widen == 20.0f);
    GIANT_CHECK(hair_lod(0, width, 0.3f, 1000.0f, 3.0f, 0).strands == 0);
    // A cap under the fewest: the cap.
    GIANT_CHECK(hair_lod(1000, width, 0.3f, 1000.0f, 3.0f, 10).strands == 10);
    GIANT_CHECK(hair_lod(1000, width, 0.3f, 0.01f, 3.0f, 1).strands == 1);
    GIANT_CHECK_NEAR(hair_lod(1000, width, 0.3f, 0.01f, 3.0f, 1).widen, 1000.0, 1e-3);
    // A 0.3 m strand's 15 pieces are 2 cm each: joined until a piece is 3 pixels.
    GIANT_CHECK(hair_lod(1000, width, 0.3f, 200.0f, 3.0f, 0).stride == 1);  // 4 pixels a piece
    GIANT_CHECK(hair_lod(1000, width, 0.3f, 100.0f, 3.0f, 0).stride == 3);  // 2
    GIANT_CHECK(hair_lod(1000, width, 0.3f, 40.0f, 3.0f, 0).stride == 5);   // 0.8
    GIANT_CHECK(hair_lod(1000, width, 0.3f, 10.0f, 3.0f, 0).stride == 15);  // 0.2
    // Bad numbers: the fewest, never none of a groom that has some.
    const HairLod odd = hair_lod(1000, std::numeric_limits<float>::quiet_NaN(), 0.3f,
                                 std::numeric_limits<float>::infinity(), -1.0f, 0);
    GIANT_CHECK(odd.strands == k_hair_least_strands && std::isfinite(odd.widen) && odd.stride == 15);
}

GIANT_TEST("hair: a frame draws the nearest grooms in view, thinned to fit, their guides only when whole") {
    Grooms grooms;
    const geometry::Groom groom = curtain(2000, 20);
    const GroomId id = held(grooms, groom);
    GIANT_CHECK(id != k_no_groom && grooms.count() == 1);
    std::vector<HairDraw> draws(5);
    for (HairDraw& d : draws) d.groom = id;
    draws[0].position = {0.0, 0.0, -8.0};
    draws[1].position = {0.0, 0.0, -2.0};
    draws[2].position = {0.0, 0.0, 5.0};    // behind the camera
    draws[3].position = {0.0, 0.0, -4000.0}; // too small to see
    draws[4].groom = id + 1;                 // no such groom
    HairPlan plan;
    plan_hair(grooms, draws, looking(), plan);
    GIANT_CHECK(plan.grooms.size() == 2 && plan.left_out == 1);
    GIANT_CHECK(plan.grooms[0].draw == 1 && plan.grooms[1].draw == 0); // nearest first
    GIANT_CHECK(plan.grooms[0].first == 0 && plan.grooms[1].first == plan.grooms[0].lod.strands);
    GIANT_CHECK(plan.grooms[0].lod.strands > plan.grooms[1].lod.strands);
    GIANT_CHECK(plan.strands == plan.grooms[0].lod.strands + plan.grooms[1].lod.strands);
    u32 segments = 0;
    for (const HairPlanned& p : plan.grooms) segments += p.lod.strands * (15 / p.lod.stride);
    GIANT_CHECK(plan.segments == segments);
    GIANT_CHECK(!plan.grooms[0].posed);
    // Guides: all 20 * 16, all numbers; else at rest.
    std::vector<Vec3> guides(groom.points.begin(), groom.points.begin() + 20 * 16);
    draws[1].guides = guides;
    plan_hair(grooms, draws, looking(), plan);
    GIANT_CHECK(plan.grooms[0].posed);
    guides[7].x = std::numeric_limits<float>::quiet_NaN();
    plan_hair(grooms, draws, looking(), plan);
    GIANT_CHECK(!plan.grooms[0].posed);
    draws[1].guides = std::span<const Vec3>(guides).first(100);
    plan_hair(grooms, draws, looking(), plan);
    GIANT_CHECK(!plan.grooms[0].posed);
    // A place or size that is no number: left out.
    draws[0].scale = 0.0f;
    draws[1].position.x = std::numeric_limits<double>::quiet_NaN();
    plan_hair(grooms, draws, looking(), plan);
    GIANT_CHECK(plan.grooms.empty() && plan.left_out == 3);

    // More strands than a frame draws: every groom thinned alike, wider to match.
    Grooms many;
    PackedGroom lots; // (as a groom of 200,000 strands packs: a plan reads only this much)
    lots.bytes.resize(16);
    lots.strands = 200000;
    lots.guides = 50;
    lots.bounds_min = {-0.5f, -0.5f, 0.0f};
    lots.bounds_size = {1.0f, 1.0f, 1e-6f};
    lots.longest = 1.0f;
    const GroomId thick = many.add(std::move(lots));
    many.find(thick)->uploaded = many.find(thick)->size;
    many.find(thick)->buffer = {1, 1};
    std::vector<HairDraw> close(3);
    for (usize i = 0; i < close.size(); ++i) {
        close[i].groom = thick;
        close[i].position = {0.0, 0.0, -1.5 - 0.01 * static_cast<double>(i)};
        close[i].look.width = 0.01f; // every strand wanted
    }
    plan_hair(many, close, looking(), plan);
    GIANT_CHECK(plan.grooms.size() == 3 && plan.strands <= k_max_hair_strands && plan.strands > k_max_hair_strands - 8);
    for (const HairPlanned& p : plan.grooms) {
        GIANT_CHECK_NEAR(p.lod.widen * static_cast<float>(p.lod.strands), 200000.0, 200.0);
        GIANT_CHECK(p.first + p.lod.strands <= k_max_hair_strands);
    }
    // A groom of more strands than the renderer keeps: each kept strand
    // drawn wider for those cut, so it covers what the whole would.
    {
        PackedGroom cut = many.find(thick)->groom;
        cut.bytes.resize(16);
        cut.authored = 800000;
        Grooms kept;
        const GroomId id4 = kept.add(std::move(cut));
        kept.find(id4)->uploaded = kept.find(id4)->size;
        kept.find(id4)->buffer = {1, 1};
        HairDraw one;
        one.groom = id4;
        one.position = {0.0, 0.0, -1.5};
        one.look.width = 0.01f;
        HairPlan whole;
        plan_hair(kept, {&one, 1}, looking(), whole);
        GIANT_CHECK(whole.grooms.size() == 1 && whole.grooms[0].lod.strands == 200000);
        GIANT_CHECK_NEAR(whole.grooms[0].lod.widen, 4.0, 1e-4);
    }
    // Guides blown far outside the groom's bounds at rest keep it in view.
    {
        std::vector<Vec3> blown(50 * 16, Vec3{6.0f, 0.0f, 0.0f});
        HairDraw aside;
        aside.groom = thick;
        aside.position = {-6.0, 0.0, -3.0}; // at rest: out of view to the left
        HairPlan seen;
        plan_hair(many, {&aside, 1}, looking(), seen);
        GIANT_CHECK(seen.grooms.empty() && seen.left_out == 0);
        aside.guides = blown;
        plan_hair(many, {&aside, 1}, looking(), seen);
        GIANT_CHECK(seen.grooms.size() == 1 && seen.grooms[0].posed);
    }
    // When the frame's strands run out part way down the list, what is
    // left of a groom is widened for what was cut, and one with none left
    // counts as left out.
    {
        Grooms mixed;
        PackedGroom tiny;
        tiny.bytes.resize(16);
        tiny.strands = tiny.authored = 1;
        tiny.guides = 1;
        tiny.bounds_min = {-0.5f, -0.5f, 0.0f};
        tiny.bounds_size = {1.0f, 1.0f, 1e-6f};
        tiny.longest = 1.0f;
        PackedGroom huge = tiny;
        huge.strands = huge.authored = k_max_groom_strands;
        const GroomId small_id = mixed.add(std::move(tiny));
        const GroomId huge_id = mixed.add(std::move(huge));
        for (GroomId each : {small_id, huge_id}) {
            mixed.find(each)->uploaded = mixed.find(each)->size;
            mixed.find(each)->buffer = {1, 1};
        }
        std::vector<HairDraw> list(203);
        for (usize i = 0; i < list.size(); ++i) {
            list[i].groom = i < 200 ? small_id : huge_id;
            list[i].position = {0.0, 0.0, -1.5 - 0.001 * static_cast<double>(i)};
            list[i].look.width = 0.01f;
        }
        HairPlan tight;
        plan_hair(mixed, list, looking(), tight);
        GIANT_CHECK(tight.strands <= k_max_hair_strands);
        u32 covered = 0;
        for (const HairPlanned& p : tight.grooms) {
            GIANT_CHECK(p.lod.strands > 0 && p.first + p.lod.strands <= k_max_hair_strands);
            if (p.draw >= 200) {
                GIANT_CHECK_NEAR(p.lod.widen * static_cast<float>(p.lod.strands), static_cast<double>(k_max_groom_strands),
                                 static_cast<double>(k_max_groom_strands) * 1e-3);
            }
            ++covered;
        }
        GIANT_CHECK(covered + tight.left_out == list.size());
    }
    // More grooms than a frame draws: the nearest.
    std::vector<HairDraw> crowd(k_max_hair_draws + 40);
    for (usize i = 0; i < crowd.size(); ++i) {
        crowd[i].groom = thick;
        crowd[i].position = {0.0, 0.0, -3.0 - 0.1 * static_cast<double>(crowd.size() - i)};
    }
    plan_hair(many, crowd, looking(), plan);
    GIANT_CHECK(plan.grooms.size() == k_max_hair_draws && plan.left_out == 40);
    GIANT_CHECK(plan.grooms[0].draw == crowd.size() - 1);
    // Removed: its id no longer resolves, even once the slot is used again.
    many.remove(thick);
    GIANT_CHECK(many.find(thick) == nullptr && many.count() == 0);
    const GroomId next = held(many, curtain(10, 2));
    GIANT_CHECK(next != thick && (next & k_groom_slot_mask) == (thick & k_groom_slot_mask));
    GIANT_CHECK(many.find(thick) == nullptr && many.find(next) != nullptr);
    // The groom as the shaders take it.
    HairDraw placed;
    placed.groom = next;
    placed.position = {10.0, 2.0, -3.0};
    placed.scale = 2.0f;
    placed.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, degrees_to_radians(90.0f));
    HairPlanned planned;
    planned.lod.strands = 10;
    planned.lod.widen = 4.0f;
    planned.lod.stride = 3;
    planned.first = 7;
    const gpu::HairGroom g = to_gpu(planned, *many.find(next), placed, {9.0, 2.0, 0.0});
    // +X in the groom's space is -Z in the world (a quarter turn about +Y), twice as long.
    GIANT_CHECK_NEAR(g.to_world[0], 0.0, 1e-5);
    GIANT_CHECK_NEAR(g.to_world[8], -2.0, 1e-5);
    GIANT_CHECK_NEAR(g.to_world[3], 1.0, 1e-5);
    GIANT_CHECK_NEAR(g.to_world[11], -3.0, 1e-5);
    GIANT_CHECK(g.count == 10 && g.stride == 3 && g.first == 7 && g.posed_on == 0);
    GIANT_CHECK_NEAR(g.half_width, 0.5 * 0.00008 * 2.0 * 4.0, 1e-8);
}

GIANT_TEST("hair: grooms go to the GPU under a budget and draw in the lit view once there (null device)") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    rhi::TextureDesc t;
    t.width = 96;
    t.height = 64;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(t);
    FrameInput frame;
    frame.settings.gpu_culling = false;
    const auto draw = [&] {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        return renderer->last_stats();
    };
    GIANT_CHECK(renderer->add_groom(geometry::Groom{}) == k_no_groom);
    // 60,000 strands are 8.6 MiB: three frames at 4 MiB a frame.
    const GroomId big = renderer->add_groom(curtain(60000, 100));
    const GroomId small = renderer->add_groom(curtain(500, 10));
    GIANT_CHECK(big != k_no_groom && small != k_no_groom && big != small);
    GIANT_CHECK(!renderer->groom_ready(big) && !renderer->groom_ready(small));
    std::vector<HairDraw> hair(2);
    hair[0].groom = big;
    hair[0].position = {0.0, 0.0, -2.0};
    hair[1].groom = small;
    hair[1].position = {0.5, 0.0, -3.0};
    frame.hair = hair;
    const u32 bare_post = draw().post_draws;
    GIANT_CHECK(!renderer->groom_ready(big) && renderer->last_stats().hair_grooms == 0);
    GIANT_CHECK(renderer->last_stats().hair_left_out == 2);
    draw();
    GIANT_CHECK(!renderer->groom_ready(big) && !renderer->groom_ready(small)); // in the order added
    RenderStats s = draw();
    GIANT_CHECK(renderer->groom_ready(big) && renderer->groom_ready(small));
    s = draw();
    GIANT_CHECK(s.hair_grooms == 2 && s.hair_left_out == 0 && s.hair_strands > 100 && s.hair_segments >= s.hair_strands);
    // A dispatch and two draws a groom, the composite and the depth.
    GIANT_CHECK(s.hair_draws == 2 * 3 + 2 && s.post_draws == bare_post);
    frame.settings.hair = false;
    GIANT_CHECK(draw().hair_grooms == 0);
    frame.settings.hair = true;
    frame.settings.debug_view = DebugView::clusters; // not the lit view
    GIANT_CHECK(draw().hair_grooms == 0);
    frame.settings.debug_view = DebugView::lit;
    renderer->remove_groom(big);
    GIANT_CHECK(!renderer->groom_ready(big));
    s = draw();
    GIANT_CHECK(s.hair_grooms == 1 && s.hair_left_out == 1);
    renderer->remove_groom(big); // twice: nothing
    // Removed while its bytes are still going up: gone, and the next goes on.
    const GroomId going = renderer->add_groom(curtain(60000, 100));
    const GroomId after = renderer->add_groom(curtain(500, 10));
    draw();
    GIANT_CHECK(!renderer->groom_ready(going));
    renderer->remove_groom(going);
    draw();
    GIANT_CHECK(!renderer->groom_ready(going) && renderer->groom_ready(after));
    renderer->remove_groom(after);
    renderer->remove_groom(small);
    GIANT_CHECK(draw().hair_grooms == 0);
}

GIANT_TEST("hair: d3d12 a curtain of hair covers what is behind it, by its colour, guides and depth") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    if (device == nullptr) return; // not on Windows
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    constexpr u32 k_w = 64, k_h = 64;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    // A grey wall 4 m away; the curtain (1 m square) 2 m away, in front of
    // its middle; a cube that can stand in front of the curtain.
    std::vector<Instance> things(2);
    things[0].mesh = renderer->builtin(BuiltinMesh::cube);
    things[0].position = {0.0, 0.0, -4.5};
    things[0].scale = {8.0f, 8.0f, 1.0f};
    things[1].mesh = renderer->builtin(BuiltinMesh::cube);
    things[1].position = {0.0, 0.0, -1.2};
    things[1].scale = {0.9f, 0.9f, 0.2f};
    std::vector<Material> materials(1);
    materials[0].base_color = {0.5f, 0.5f, 0.5f};
    materials[0].roughness = 0.9f;
    std::vector<ParticleStyle> styles(1);
    styles[0].life_min = styles[0].life_max = 2.0f; // a white puff, opaque, still
    styles[0].size_start = styles[0].size_end = 1.0f;
    styles[0].color_start = styles[0].color_end = {1.0f, 1.0f, 1.0f};
    styles[0].opacity_start = styles[0].opacity_end = 1.0f;
    styles[0].gravity = 0.0f;
    styles[0].soft = 0.0f;
    styles[0].lit = false;
    styles[0].emissive = {4.0f, 4.0f, 4.0f};
    FrameInput frame;
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    frame.settings.shadows = false;
    frame.settings.gi = false;
    frame.settings.physical_sky = false;
    frame.settings.reflections = false;
    frame.settings.screen_space_reflections = false;
    frame.settings.antialiasing = false;
    frame.settings.ambient_occlusion = false;
    frame.settings.contact_shadows = false;
    frame.materials = materials;
    frame.particle_styles = styles;
    frame.delta_seconds = 0.0f;
    frame.lighting.background = {0.5f, 0.5f, 0.5f};
    frame.lighting.sun_direction = {0.0f, 0.0f, 1.0f}; // behind the camera (it looks down -Z)
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.3f, 0.3f, 0.3f};
    frame.lighting.ground_light = {0.1f, 0.1f, 0.1f};
    const geometry::Groom groom = curtain(4000, 40);
    const GroomId id = renderer->add_groom(groom);
    GIANT_CHECK(id != k_no_groom);
    std::vector<HairDraw> hair(1);
    hair[0].groom = id;
    hair[0].position = {0.0, 0.0, -2.0};
    hair[0].look.width = 0.002f; // thick strands: 4000 of them are a wall of hair 8 m of strand wide
    hair[0].look.variation = 0.0f;
    std::vector<u8> again;
    const auto draw = [&](bool with_hair, bool cube_in_front) {
        frame.hair = with_hair ? std::span<const HairDraw>(hair) : std::span<const HairDraw>{};
        frame.instances = std::span<const Instance>(things).first(cube_in_front ? 2 : 1);
        int quiet = 0;
        for (int i = 0; i < 64 && (i < 4 || quiet < 4); ++i) {
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
            const RenderStats& st = renderer->last_stats();
            quiet = st.streamed_in == 0 && st.waiting == 0 && (!with_hair || st.hair_grooms == 1) ? quiet + 1 : 0;
        }
        device->wait_idle();
        std::vector<u8> image = device->read_texture(target);
        // One more frame: the same image (nothing kept from frame to frame).
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        if (cmd != nullptr) {
            renderer->render(*cmd, target, frame);
            device->end_frame();
        }
        device->wait_idle();
        again = device->read_texture(target);
        return image;
    };
    const auto channel = [](const std::vector<u8>& image, u32 x, u32 y, u32 c) {
        const usize i = (static_cast<usize>(y) * k_w + x) * 4;
        return i + 3 < image.size() ? static_cast<int>(image[i + c]) : -1;
    };
    const auto show = [&](const char* what, const std::vector<u8>& image) {
        std::printf("hair: %s: middle %d %d %d, right of it %d %d %d, corner %d %d %d\n", what, channel(image, 32, 32, 0),
                    channel(image, 32, 32, 1), channel(image, 32, 32, 2), channel(image, 52, 32, 0), channel(image, 52, 32, 1),
                    channel(image, 52, 32, 2), channel(image, 3, 3, 0), channel(image, 3, 3, 1), channel(image, 3, 3, 2));
    };
    const std::vector<u8> bare = draw(false, false);
    show("the wall alone", bare);
    // Black hair over the wall's middle: dark there, the wall as it was
    // beside it (the curtain is 28 pixels wide of 64).
    hair[0].look.melanin = 1.0f;
    const std::vector<u8> dark = draw(true, false);
    const bool still = again == dark;
    show("black hair", dark);
    GIANT_CHECK(renderer->last_stats().hair_grooms == 1 && renderer->last_stats().hair_strands > 0);
    GIANT_CHECK(still);
    GIANT_CHECK(channel(dark, 32, 32, 0) < channel(bare, 32, 32, 0) - 20);
    for (u32 c = 0; c < 3; ++c) {
        GIANT_CHECK(channel(dark, 3, 3, c) == channel(bare, 3, 3, c));
        GIANT_CHECK(channel(dark, 60, 32, c) == channel(bare, 60, 32, c));
    }
    // It covers the wall whole: the same along a row across its middle
    // (its highlight runs across the strands, so rows differ), with no
    // wall showing between strands.
    std::printf("hair: across the curtain's middle row: %d %d %d %d %d\n", channel(dark, 22, 32, 0), channel(dark, 27, 32, 0),
                channel(dark, 32, 32, 0), channel(dark, 37, 32, 0), channel(dark, 42, 32, 0));
    for (u32 x : {22u, 27u, 37u, 42u}) {
        GIANT_CHECK(std::abs(channel(dark, x, 32, 0) - channel(dark, 32, 32, 0)) < 12);
    }
    // Brown hair is lighter and warm, blond lighter still.
    hair[0].look.melanin = 0.6f;
    const std::vector<u8> brown = draw(true, false);
    show("brown hair", brown);
    GIANT_CHECK(channel(brown, 32, 32, 0) > channel(dark, 32, 32, 0) + 10);
    GIANT_CHECK(channel(brown, 32, 32, 0) > channel(brown, 32, 32, 2) + 8);
    hair[0].look.melanin = 0.2f;
    const std::vector<u8> blond = draw(true, false);
    show("blond hair", blond);
    GIANT_CHECK(channel(blond, 32, 32, 1) > channel(brown, 32, 32, 1) + 10);
    // A few thin strands let the wall show through: between the two.
    hair[0].look.melanin = 1.0f;
    hair[0].look.width = 0.00002f;
    const std::vector<u8> thin = draw(true, false);
    show("thin black hair", thin);
    GIANT_CHECK(channel(thin, 32, 32, 0) > channel(dark, 32, 32, 0) + 5);
    GIANT_CHECK(channel(thin, 32, 32, 0) < channel(bare, 32, 32, 0));
    hair[0].look.width = 0.002f;
    // Its guides moved 0.7 m to the right carry every strand with them.
    std::vector<Vec3> guides(groom.points.begin(), groom.points.begin() + 40 * 16);
    for (Vec3& p : guides) p.x += 0.7f;
    hair[0].guides = guides;
    const std::vector<u8> moved = draw(true, false);
    show("moved by its guides", moved);
    GIANT_CHECK(std::abs(channel(moved, 32, 32, 0) - channel(bare, 32, 32, 0)) <= 1);
    GIANT_CHECK(std::abs(channel(moved, 52, 32, 0) - channel(dark, 32, 32, 0)) < 12);
    hair[0].guides = {};
    // Behind a cube it is hidden: the image of the cube alone.
    const std::vector<u8> cube = draw(false, true);
    const std::vector<u8> hidden = draw(true, true);
    show("behind a cube", hidden);
    GIANT_CHECK(hidden == cube);
    // A glowing puff behind the curtain stays behind it (the hair's depth
    // is the scene's for what is drawn after); without hair it shows.
    ParticleBurst puff;
    puff.position = {0.0, 0.0, -3.0};
    puff.speed_min = puff.speed_max = 0.0f;
    puff.count = 1;
    frame.particle_bursts = {&puff, 1};
    {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        if (cmd != nullptr) {
            frame.hair = {};
            frame.instances = std::span<const Instance>(things).first(1);
            frame.delta_seconds = 0.5f; // born and faded in
            renderer->render(*cmd, target, frame);
            device->end_frame();
        }
        frame.particle_bursts = {};
        cmd = device->begin_frame(nullptr);
        if (cmd != nullptr) {
            renderer->render(*cmd, target, frame);
            device->end_frame();
        }
        frame.delta_seconds = 0.0f;
    }
    const std::vector<u8> puff_alone = draw(false, false);
    const std::vector<u8> puff_behind = draw(true, false);
    show("a puff alone", puff_alone);
    show("a puff behind the hair", puff_behind);
    GIANT_CHECK(channel(puff_alone, 32, 32, 0) > channel(bare, 32, 32, 0) + 40);
    GIANT_CHECK(std::abs(channel(puff_behind, 32, 32, 0) - channel(dark, 32, 32, 0)) < 12);
    renderer->remove_groom(id);
    device->destroy(target);
}
