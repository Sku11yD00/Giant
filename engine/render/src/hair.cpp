#include "hair.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace giant::render {

namespace {

float safe(float v, float low, float high, float otherwise) {
    return std::isfinite(v) ? std::clamp(v, low, high) : otherwise;
}

u32 to_16(float share) { return static_cast<u32>(std::clamp(share, 0.0f, 1.0f) * 65535.0f + 0.5f); }

} // namespace

Vec3 hair_color(const HairLook& look) {
    // What is left of white light after the pigment: eumelanin (brown to
    // black) and pheomelanin (red) take the blue end first, each by its
    // own curve (the absorption d'Eon et al. give them, in "An
    // energy-conserving hair reflectance model"); how much of it there is
    // grows as the square of `melanin`, so the middle of the range is
    // brown and the end black.
    const float melanin = safe(look.melanin, 0.0f, 1.0f, 0.5f);
    const float red = safe(look.redness, 0.0f, 1.0f, 0.0f);
    const float dense = 8.0f * melanin * melanin;
    const Vec3 brown{0.419f, 0.697f, 1.37f};
    const Vec3 ginger{0.187f, 0.4f, 1.05f};
    const Vec3 takes = (brown * (1.0f - red) + ginger * red) * dense;
    const Vec3 left{0.9f * std::exp(-takes.x), 0.9f * std::exp(-takes.y), 0.9f * std::exp(-takes.z)};
    return {left.x * safe(look.tint.x, 0.0f, 1.0f, 1.0f), left.y * safe(look.tint.y, 0.0f, 1.0f, 1.0f),
            left.z * safe(look.tint.z, 0.0f, 1.0f, 1.0f)};
}

PackedGroom pack_groom(const geometry::Groom& groom) {
    PackedGroom packed;
    if (groom.strand_count() == 0 || !geometry::groom_problem(groom).empty()) return packed;
    const u32 strands = std::min(groom.strand_count(), k_max_groom_strands);
    const u32 guides = std::min(groom.guide_count, strands);
    // The bounds of the points kept.
    Vec3 low = groom.points[0];
    Vec3 high = low;
    const usize points = static_cast<usize>(strands) * geometry::k_strand_points;
    for (usize i = 0; i < points; ++i) {
        const Vec3& p = groom.points[i];
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    // (A flat or single-point groom still spans something.)
    const Vec3 size{std::max(high.x - low.x, 1e-6f), std::max(high.y - low.y, 1e-6f), std::max(high.z - low.z, 1e-6f)};
    packed.strands = strands;
    packed.authored = groom.strand_count();
    packed.guides = guides;
    packed.bounds_min = low;
    packed.bounds_size = size;
    packed.points_offset = strands * static_cast<u32>(sizeof(gpu::HairStrand));
    packed.bytes.resize(packed.points_offset + points * gpu::k_hair_point_bytes);
    float longest = 0.0f;
    for (u32 s = 0; s < strands; ++s) {
        const geometry::StrandFollow& follow = groom.follow[s];
        // Guides past those kept (never, for a groom that passes
        // groom_problem and keeps its guides): the strand follows the first.
        const auto guide = [&](u32 slot) { return follow.guides[slot] < guides ? follow.guides[slot] : 0u; };
        gpu::HairStrand strand{};
        const u32 w0 = to_16(follow.weights[0]);
        const u32 w1 = std::min(to_16(follow.weights[1]), 65535u - w0);
        strand.guides01 = guide(0) | guide(1) << 16;
        strand.guide2_weight0 = guide(2) | w0 << 16;
        strand.weight1 = w1;
        strand.seed = groom.seeds[s];
        std::memcpy(packed.bytes.data() + static_cast<usize>(s) * sizeof(strand), &strand, sizeof(strand));
        float along = 0.0f;
        for (u32 k = 0; k < geometry::k_strand_points; ++k) {
            const usize at = static_cast<usize>(s) * geometry::k_strand_points + k;
            const Vec3& p = groom.points[at];
            if (k > 0) along += length(p - groom.points[at - 1]);
            const u32 words[2] = {to_16((p.x - low.x) / size.x) | to_16((p.y - low.y) / size.y) << 16,
                                  to_16((p.z - low.z) / size.z)};
            std::memcpy(packed.bytes.data() + packed.points_offset + at * gpu::k_hair_point_bytes, words, sizeof(words));
        }
        longest = std::max(longest, along);
    }
    packed.longest = longest;
    return packed;
}

Vec3 unpack_point(const PackedGroom& groom, u32 strand, u32 point) {
    u32 words[2] = {0, 0};
    const usize at = groom.points_offset +
                     (static_cast<usize>(strand) * geometry::k_strand_points + point) * gpu::k_hair_point_bytes;
    if (at + sizeof(words) > groom.bytes.size()) return {};
    std::memcpy(words, groom.bytes.data() + at, sizeof(words));
    const Vec3 share{static_cast<float>(words[0] & 0xffffu) / 65535.0f, static_cast<float>(words[0] >> 16) / 65535.0f,
                     static_cast<float>(words[1] & 0xffffu) / 65535.0f};
    return {groom.bounds_min.x + share.x * groom.bounds_size.x, groom.bounds_min.y + share.y * groom.bounds_size.y,
            groom.bounds_min.z + share.z * groom.bounds_size.z};
}

HairLod hair_lod(u32 strands, float width, float longest, float pixels_per_metre, float detail, u32 most) {
    HairLod lod;
    if (strands == 0) return lod;
    const u32 all = most == 0 ? strands : std::min(strands, most);
    const float ppm = safe(pixels_per_metre, 0.0f, 1.0e9f, 0.0f);
    // As many as make each drawn strand 1 / detail of a pixel wide.
    const float share = std::clamp(safe(width, 0.0f, 1.0f, 0.0f) * ppm * safe(detail, 0.5f, 16.0f, 3.0f), 0.0f, 1.0f);
    const u32 wanted = static_cast<u32>(std::ceil(static_cast<double>(strands) * static_cast<double>(share)));
    lod.strands = std::clamp(wanted, std::min(all, k_hair_least_strands), all);
    lod.widen = static_cast<float>(strands) / static_cast<float>(lod.strands);
    const float piece = safe(longest, 0.0f, 1.0e6f, 0.0f) / static_cast<float>(geometry::k_strand_points - 1) * ppm;
    lod.stride = 15;
    for (u32 stride : {1u, 3u, 5u}) {
        if (piece * static_cast<float>(stride) >= k_hair_piece_pixels) {
            lod.stride = stride;
            break;
        }
    }
    return lod;
}

GroomId Grooms::add(PackedGroom groom) {
    u32 index = 0;
    if (!m_free.empty()) {
        index = m_free.back();
        m_free.pop_back();
    } else {
        if (m_slots.size() >= k_max_grooms) return k_no_groom;
        index = static_cast<u32>(m_slots.size());
        m_slots.emplace_back();
    }
    GroomSlot& slot = m_slots[index];
    slot.used = true;
    slot.size = groom.bytes.size();
    slot.uploaded = 0;
    slot.buffer = {};
    slot.groom = std::move(groom);
    m_waiting.push_back(index);
    ++m_count;
    return index | slot.generation << 20;
}

const GroomSlot* Grooms::find(GroomId id) const {
    const u32 index = id & k_groom_slot_mask;
    if (id == k_no_groom || index >= m_slots.size()) return nullptr;
    const GroomSlot& slot = m_slots[index];
    return slot.used && slot.generation == id >> 20 ? &slot : nullptr;
}

GroomSlot* Grooms::find(GroomId id) { return const_cast<GroomSlot*>(std::as_const(*this).find(id)); }

void Grooms::remove(GroomId id) {
    GroomSlot* slot = find(id);
    if (slot == nullptr) return;
    const u32 index = id & k_groom_slot_mask;
    slot->used = false;
    slot->groom = {};
    slot->buffer = {};
    slot->size = 0;
    slot->uploaded = 0;
    slot->generation = slot->generation + 1 >= k_groom_generations ? 1 : slot->generation + 1;
    m_waiting.erase(std::remove(m_waiting.begin(), m_waiting.end(), index), m_waiting.end());
    m_free.push_back(index);
    --m_count;
}

void plan_hair(const Grooms& grooms, std::span<const HairDraw> draws, const HairView& view, HairPlan& plan) {
    plan.grooms.clear();
    plan.strands = 0;
    plan.segments = 0;
    plan.left_out = 0;
    for (usize i = 0; i < draws.size(); ++i) {
        const HairDraw& draw = draws[i];
        const GroomSlot* slot = grooms.find(draw.groom);
        const Quat& q = draw.rotation;
        const float turn = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        const bool placed = std::isfinite(draw.position.x) && std::isfinite(draw.position.y) &&
                            std::isfinite(draw.position.z) && std::isfinite(turn) && turn > 1e-12f &&
                            std::isfinite(draw.scale) && draw.scale > 0.0f;
        if (slot == nullptr || slot->uploaded < slot->size || !slot->buffer.valid() || !placed) {
            ++plan.left_out;
            continue;
        }
        const PackedGroom& g = slot->groom;
        const Vec3 middle = g.bounds_min + g.bounds_size * 0.5f;
        // Its guides as given: all of them, all numbers (else at rest).
        // Blown about, they may reach past the groom's bounds at rest.
        bool posed = draw.guides.size() == static_cast<usize>(g.guides) * geometry::k_strand_points && g.guides > 0;
        float reach2 = 0.25f * length_squared(g.bounds_size);
        for (usize p = 0; p < draw.guides.size() && posed; ++p) {
            const Vec3& at = draw.guides[p];
            posed = std::abs(at.x) <= geometry::k_max_groom_reach && std::abs(at.y) <= geometry::k_max_groom_reach &&
                    std::abs(at.z) <= geometry::k_max_groom_reach; // (false for no number)
            reach2 = std::max(reach2, length_squared(at - middle));
        }
        if (!posed) reach2 = 0.25f * length_squared(g.bounds_size);
        // In view: the ball round it against the frustum.
        const float radius = std::sqrt(reach2) * draw.scale;
        const Vec3 centre = to_camera_relative(draw.position, view.position) + rotate(normalize(q), middle) * draw.scale;
        bool seen = std::isfinite(centre.x + centre.y + centre.z);
        for (const Vec4& plane : view.planes) {
            seen = seen && plane.x * centre.x + plane.y * centre.y + plane.z * centre.z + plane.w >= -radius;
        }
        if (!seen) continue; // (out of view: not a draw left out)
        const float away = std::max(length(centre), 1e-3f);
        const float ppm = view.pixels_per_metre_at_one / away;
        if (2.0f * radius * ppm < k_hair_least_pixels) continue;
        HairPlanned planned;
        planned.draw = static_cast<u32>(i);
        planned.slot = draw.groom & k_groom_slot_mask;
        planned.distance = static_cast<double>(away);
        const float width = safe(draw.look.width, 1e-5f, 0.01f, HairLook{}.width) * draw.scale;
        planned.lod = hair_lod(g.strands, width, g.longest * draw.scale, ppm, view.detail, draw.strands);
        // (A groom cut to what the renderer keeps: each kept strand stands for several.)
        planned.lod.widen *= static_cast<float>(std::max(g.authored, g.strands)) / static_cast<float>(g.strands);
        planned.posed = posed;
        plan.grooms.push_back(planned);
    }
    // The nearest, nearest first (ties as given, so the same frame is the same plan).
    std::sort(plan.grooms.begin(), plan.grooms.end(), [](const HairPlanned& a, const HairPlanned& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.draw < b.draw;
    });
    if (plan.grooms.size() > k_max_hair_draws) {
        plan.left_out += static_cast<u32>(plan.grooms.size() - k_max_hair_draws);
        plan.grooms.resize(k_max_hair_draws);
    }
    // Too many strands in all: every groom thinned alike.
    u64 wanted = 0;
    for (const HairPlanned& p : plan.grooms) wanted += p.lod.strands;
    const double keep = wanted > k_max_hair_strands ? static_cast<double>(k_max_hair_strands) / static_cast<double>(wanted) : 1.0;
    u32 first = 0;
    usize posed_bytes = 0;
    for (HairPlanned& p : plan.grooms) {
        if (p.posed) {
            const usize bytes = draws[p.draw].guides.size() * 12;
            p.posed = posed_bytes + bytes <= k_hair_posed_bytes;
            if (p.posed) posed_bytes += bytes;
        }
        if (keep < 1.0) {
            const u32 was = p.lod.strands;
            p.lod.strands = std::max(static_cast<u32>(static_cast<double>(was) * keep), 1u);
            p.lod.widen *= static_cast<float>(was) / static_cast<float>(p.lod.strands);
        }
        p.lod.strands = std::min(p.lod.strands, k_max_hair_strands - first);
        p.first = first;
        first += p.lod.strands;
        plan.segments += p.lod.strands * ((geometry::k_strand_points - 1) / p.lod.stride);
    }
    plan.strands = first;
    // (None left of a groom once the frame's strands ran out: not drawn.)
    std::erase_if(plan.grooms, [](const HairPlanned& p) { return p.lod.strands == 0; });
}

gpu::HairGroom to_gpu(const HairPlanned& planned, const GroomSlot& slot, const HairDraw& draw, const DVec3& camera) {
    gpu::HairGroom g{};
    const Quat q = normalize(draw.rotation);
    const Vec3 x = rotate(q, {1.0f, 0.0f, 0.0f}) * draw.scale;
    const Vec3 y = rotate(q, {0.0f, 1.0f, 0.0f}) * draw.scale;
    const Vec3 z = rotate(q, {0.0f, 0.0f, 1.0f}) * draw.scale;
    const Vec3 at = to_camera_relative(draw.position, camera);
    const float rows[12] = {x.x, y.x, z.x, at.x, x.y, y.y, z.y, at.y, x.z, y.z, z.z, at.z};
    std::memcpy(g.to_world, rows, sizeof(rows));
    const PackedGroom& p = slot.groom;
    g.bounds_min[0] = p.bounds_min.x;
    g.bounds_min[1] = p.bounds_min.y;
    g.bounds_min[2] = p.bounds_min.z;
    g.bounds_size[0] = p.bounds_size.x;
    g.bounds_size[1] = p.bounds_size.y;
    g.bounds_size[2] = p.bounds_size.z;
    g.points_offset = p.points_offset;
    g.first = planned.first;
    const Vec3 color = hair_color(draw.look);
    g.color[0] = color.x;
    g.color[1] = color.y;
    g.color[2] = color.z;
    g.roughness = safe(draw.look.roughness, 0.05f, 1.0f, HairLook{}.roughness);
    g.half_width = 0.5f * safe(draw.look.width, 1e-5f, 0.01f, HairLook{}.width) * draw.scale * planned.lod.widen;
    g.tip_width = safe(draw.look.tip_width, 0.0f, 1.0f, HairLook{}.tip_width);
    g.variation = safe(draw.look.variation, 0.0f, 1.0f, 0.0f);
    g.count = planned.lod.strands;
    g.stride = planned.lod.stride;
    return g;
}

} // namespace giant::render
