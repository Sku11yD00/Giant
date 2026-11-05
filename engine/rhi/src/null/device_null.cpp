// The null backend: no GPU. Validates call order so tests catch misuse that
// a real backend would turn into a GPU crash.
#include "../backends.h"

#include "giant/core/assert.h"

namespace giant::rhi {

namespace {

class NullSwapChain final : public SwapChain {
public:
    NullSwapChain(u32 width, u32 height) : m_width(width), m_height(height) {}
    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }
    void resize(u32 width, u32 height) override {
        if (width == 0 || height == 0) {
            return;
        }
        m_width = width;
        m_height = height;
    }
    bool in_frame = false;

private:
    u32 m_width;
    u32 m_height;
};

class NullDevice final : public Device {
public:
    NullDevice() { m_info.name = "null device"; }

    Backend backend() const override { return Backend::null; }
    const AdapterInfo& adapter() const override { return m_info; }

    std::unique_ptr<SwapChain> create_swap_chain(const SwapChainDesc& desc) override {
        GIANT_ASSERT(desc.buffer_count >= 2 && desc.buffer_count <= 3, "2 or 3 back buffers");
        return std::make_unique<NullSwapChain>(desc.width, desc.height);
    }

    void begin_frame(SwapChain& swap_chain) override {
        auto& sc = static_cast<NullSwapChain&>(swap_chain);
        GIANT_ASSERT(!sc.in_frame, "begin_frame called twice");
        sc.in_frame = true;
    }

    void clear(SwapChain& swap_chain, const Color&) override {
        GIANT_ASSERT(static_cast<NullSwapChain&>(swap_chain).in_frame, "clear outside a frame");
    }

    void end_frame(SwapChain& swap_chain) override {
        auto& sc = static_cast<NullSwapChain&>(swap_chain);
        GIANT_ASSERT(sc.in_frame, "end_frame without begin_frame");
        sc.in_frame = false;
        ++m_frames;
    }

    Color clear_offscreen_and_read_back(const Color& color) override { return color; }
    void wait_idle() override {}
    u64 frames_presented() const override { return m_frames; }

private:
    AdapterInfo m_info;
    u64 m_frames = 0;
};

} // namespace

std::unique_ptr<Device> create_null_device() { return std::make_unique<NullDevice>(); }

} // namespace giant::rhi
