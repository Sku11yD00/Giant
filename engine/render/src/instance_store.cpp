#include "instance_store.h"

#include "cells.h"
#include "giant/core/log.h"
#include "gpu_layout.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace giant::render {

namespace {

constexpr u32 k_upload_chunk = 16u << 20; // bytes per upload

// `in` as the GPU keeps it: translation from `anchor`, the anchor's
// camera-relative position at byte `anchor_at` of the same buffer.
gpu::Instance to_gpu(const Instance& in, const DVec3& anchor, u32 anchor_at, u32 material_count) {
    gpu::Instance out{};
    const Vec3 t = to_camera_relative(in.position, anchor);
    // Columns of R * S are the rotated axes, scaled.
    const Vec3 axes[3] = {rotate(in.rotation, {1, 0, 0}), rotate(in.rotation, {0, 1, 0}),
                          rotate(in.rotation, {0, 0, 1})};
    const float scale[3] = {in.scale.x, in.scale.y, in.scale.z};
    const float translation[3] = {t.x, t.y, t.z};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const Vec3& a = axes[col];
            const float component = row == 0 ? a.x : row == 1 ? a.y : a.z;
            out.model[row * 4 + col] = component * scale[col];
        }
        out.model[row * 4 + 3] = translation[row];
    }
    out.mesh = in.mesh;
    out.material = in.material < material_count ? in.material : material_count; // the default, last
    out.max_scale = std::max({std::fabs(in.scale.x), std::fabs(in.scale.y), std::fabs(in.scale.z)});
    out.anchor = anchor_at;
    return out;
}

} // namespace

InstanceStore::~InstanceStore() {
    if (m_buffer.valid()) {
        m_device.destroy(m_buffer);
    }
}

bool InstanceStore::ensure(u32 instances, u32 cells, bool& fresh) {
    fresh = false;
    if (m_buffer.valid() && instances <= m_instance_capacity && cells <= m_cell_capacity) {
        return true;
    }
    fresh = true;
    // Room to grow by half, so a world filling up does not reupload every frame.
    const u32 icap = std::max<u32>(1024, instances + instances / 2);
    // Anchors are 16 bytes: plenty of room, so cells and holes coming and
    // going rarely make a new buffer (which uploads everything again).
    const u32 ccap = std::max<u32>(4096, cells * 2 + 1);
    const u64 anchors = u64{icap} * sizeof(gpu::Instance);
    const u64 skip = anchors + u64{ccap} * 16;
    const u64 size = skip + u64{(icap + 31) / 32} * 4;
    if (size > 0xFFFFFFF0ull) {
        return false; // byte offsets are 32-bit in the shaders
    }
    rhi::BufferDesc desc;
    desc.size = size;
    desc.usage = rhi::BufferUsage::shader_read;
    desc.name = "instances";
    const rhi::BufferHandle grown = m_device.create_buffer(desc);
    if (!grown.valid()) {
        return false;
    }
    if (m_buffer.valid()) {
        m_device.destroy(m_buffer); // destruction waits for frames in flight
    }
    m_buffer = grown;
    m_instance_capacity = icap;
    m_cell_capacity = ccap;
    m_anchor_offset = static_cast<u32>(anchors);
    m_skip_offset = static_cast<u32>(skip);
    return true;
}

bool InstanceStore::upload(rhi::CommandList& cmd, std::span<const Instance> instances, const CellRange& cell,
                           u32 slot, u32 material_count) {
    const DVec3 anchor = m_anchors[slot];
    const u32 anchor_at = m_anchor_offset + slot * 16;
    const u32 per_chunk = k_upload_chunk / sizeof(gpu::Instance);
    for (u32 done = 0; done < cell.count;) {
        const u32 n = std::min(per_chunk, cell.count - done);
        rhi::TransientMemory up = cmd.allocate(n * static_cast<u32>(sizeof(gpu::Instance)));
        if (up.data == nullptr) {
            return false;
        }
        for (u32 i = 0; i < n; ++i) {
            const gpu::Instance g = to_gpu(instances[cell.first + done + i], anchor, anchor_at, material_count);
            std::memcpy(up.data + i * sizeof(g), &g, sizeof(g));
        }
        cmd.copy_buffer(m_buffer, u64{cell.first + done} * sizeof(gpu::Instance), up.buffer, up.offset,
                        u64{n} * sizeof(gpu::Instance));
        done += n;
        m_uploaded += n;
    }
    return true;
}

bool InstanceStore::update(rhi::CommandList& cmd, std::span<const Instance> instances,
                           std::span<const CellRange> cells, u32 material_count, const DVec3& camera,
                           std::span<const u32> drawn) {
    m_uploaded = 0;
    const u32 n = static_cast<u32>(instances.size());
    // Without (usable) cells: one cell of everything, anchored at the
    // camera and uploaded every frame.
    const CellRange whole{0, n, 0};
    bool usable = !cells.empty();
    if (usable) {
        u32 next = 0;
        for (const CellRange& c : cells) {
            usable = usable && c.first == next;
            next += c.count;
        }
        usable = usable && next == n;
    }
    const std::span<const CellRange> layout = usable ? cells : std::span<const CellRange>(&whole, 1);
    const u32 count = static_cast<u32>(layout.size());
    m_previous = m_seen;
    m_previous_slots = m_slots;
    const bool listed = usable && changed_cells(layout, n, m_seen, m_changed); // updates m_seen
    bool all = !listed || !m_buffer.valid() || material_count != m_material_count;
    m_material_count = material_count;
    // Each cell's anchor slot: a cell exactly as before keeps its own (and
    // its instances, which point at it); the others take free ones.
    m_slots.assign(count, ~0u);
    m_kept.assign(count, 0);
    if (!all) {
        m_claimed.assign(m_previous.size(), 0);
        usize j = 0;
        for (u32 i = 0; i < count; ++i) {
            const CellRange& c = layout[i];
            while (j < m_previous.size() && m_previous[j].first < c.first) ++j;
            for (usize k = j; k < m_previous.size() && m_previous[k].first == c.first; ++k) {
                if (m_claimed[k] == 0 && m_previous[k].count == c.count && m_previous[k].version == c.version) {
                    m_claimed[k] = 1;
                    m_slots[i] = m_previous_slots[k];
                    m_kept[i] = 1;
                    break;
                }
            }
        }
        for (usize k = 0; k < m_previous.size(); ++k) {
            if (m_claimed[k] == 0) m_free_slots.push_back(m_previous_slots[k]);
        }
        for (u32 i = 0; i < count; ++i) {
            if (m_slots[i] != ~0u) continue;
            if (m_free_slots.empty()) {
                m_slots[i] = m_slot_count++;
            } else {
                m_slots[i] = m_free_slots.back();
                m_free_slots.pop_back();
            }
        }
        bool fresh = false;
        if (!ensure(n, m_slot_count, fresh)) {
            log_error("render", "out of GPU memory for " + std::to_string(n) + " instances");
            m_seen.clear();
            return false;
        }
        all = fresh; // a new buffer holds nothing yet
    }
    if (all) {
        for (u32 i = 0; i < count; ++i) m_slots[i] = i;
        m_slot_count = count;
        m_free_slots.clear();
        bool fresh = false;
        if (!ensure(n, count, fresh)) {
            log_error("render", "out of GPU memory for " + std::to_string(n) + " instances");
            m_seen.clear();
            return false;
        }
    }
    cmd.barrier(m_buffer, rhi::ResourceState::copy_dest);
    // Anchors: the first instance of each cell, when (re)uploaded.
    m_anchors.resize(m_slot_count);
    const auto anchor_of = [&](const CellRange& c) {
        return !usable ? camera : c.count > 0 ? instances[c.first].position : DVec3{};
    };
    bool ok = true;
    if (all) {
        for (u32 i = 0; i < count && ok; ++i) {
            m_anchors[m_slots[i]] = anchor_of(layout[i]);
            ok = upload(cmd, instances, layout[i], m_slots[i], material_count);
        }
    } else {
        // The cells not as before (m_changed): uploaded, at their slot.
        for (u32 i = 0; i < count && ok; ++i) {
            if (m_kept[i] != 0) continue;
            m_anchors[m_slots[i]] = anchor_of(layout[i]);
            ok = upload(cmd, instances, layout[i], m_slots[i], material_count);
        }
    }
    if (!ok) {
        m_seen.clear(); // what was copied is incomplete: all again next time
        return false;
    }
    if (!usable) {
        m_seen.clear();
    }
    // Every anchor, camera-relative (positions get their precision here).
    m_anchor_data.assign(usize{m_slot_count} * 4, 0.0f);
    for (u32 slot = 0; slot < m_slot_count; ++slot) {
        const Vec3 a = to_camera_relative(m_anchors[slot], camera);
        m_anchor_data[slot * 4 + 0] = a.x;
        m_anchor_data[slot * 4 + 1] = a.y;
        m_anchor_data[slot * 4 + 2] = a.z;
    }
    // Not drawn: every bit set but those of `drawn`.
    m_bits.assign((n + 31) / 32 + 1, ~0u);
    for (const u32 i : drawn) {
        if (i < n) {
            m_bits[i >> 5] &= ~(1u << (i & 31u));
        }
    }
    const rhi::TransientMemory anchors = cmd.upload_array<float>(m_anchor_data);
    const rhi::TransientMemory bits = cmd.upload_array<u32>(m_bits);
    if (anchors.data == nullptr || bits.data == nullptr) {
        m_seen.clear();
        return false;
    }
    cmd.copy_buffer(m_buffer, m_anchor_offset, anchors.buffer, anchors.offset, u64{m_slot_count} * 16);
    const u64 bit_bytes = std::min<u64>(u64{m_bits.size()} * 4, u64{(m_instance_capacity + 31) / 32} * 4);
    cmd.copy_buffer(m_buffer, m_skip_offset, bits.buffer, bits.offset, bit_bytes);
    cmd.barrier(m_buffer, rhi::ResourceState::shader_read);
    return true;
}

} // namespace giant::render
