// The frame's instances, kept on the GPU from frame to frame by world cell
// (FrameInput::cells): a frame uploads only the cells that changed, plus
// each cell's anchor (one float4) and a bit per instance saying whether it
// is drawn. Instance translations are from their cell's anchor (its first
// instance's position when uploaded), so they stay precise in float
// however far the world reaches; the anchors, rewritten each frame
// relative to the camera, put them back in camera space (common.slang
// load_instance). Without cells, everything is uploaded each frame, from
// one anchor at the camera.
//
// The buffer: instances (64 bytes each, in FrameInput order), then the
// anchors, then the not-drawn bits.
#pragma once

#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"

#include <span>
#include <vector>

namespace giant::render {

class InstanceStore {
public:
    explicit InstanceStore(rhi::Device& device) : m_device(device) {}
    ~InstanceStore();
    InstanceStore(const InstanceStore&) = delete;
    InstanceStore& operator=(const InstanceStore&) = delete;

    // Inside a frame, before anything reads the instances. `drawn` lists
    // the indices of the instances drawn this frame, ascending. False when
    // out of memory (then nothing should be drawn).
    bool update(rhi::CommandList& cmd, std::span<const Instance> instances, std::span<const CellRange> cells,
                u32 material_count, const DVec3& camera, std::span<const u32> drawn);

    rhi::BufferHandle buffer() const { return m_buffer; }
    u32 skip_offset() const { return m_skip_offset; }
    u32 uploaded() const { return m_uploaded; } // instances uploaded by the last update

private:
    bool ensure(u32 instances, u32 cells);
    bool upload(rhi::CommandList& cmd, std::span<const Instance> instances, const CellRange& cell, u32 slot,
                u32 material_count);

    rhi::Device& m_device;
    rhi::BufferHandle m_buffer;
    u32 m_instance_capacity = 0;
    u32 m_cell_capacity = 0;
    u32 m_anchor_offset = 0; // bytes
    u32 m_skip_offset = 0;
    std::vector<CellRange> m_seen;    // the cells as uploaded
    std::vector<CellRange> m_changed; // scratch
    std::vector<CellRange> m_previous; // m_seen before this update (scratch)
    std::vector<DVec3> m_anchors;     // per cell, as uploaded
    u32 m_material_count = ~0u;
    u32 m_uploaded = 0;
    std::vector<u8> m_scratch;
    std::vector<u32> m_bits;
    std::vector<float> m_anchor_data;
};

} // namespace giant::render
