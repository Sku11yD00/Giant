// The null backend: no GPU. Validates call order so tests catch misuse that
// a real backend would turn into a GPU crash.
#include "../backends.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

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

    bool begin_frame(SwapChain& swap_chain) override {
        auto& sc = static_cast<NullSwapChain&>(swap_chain);
        GIANT_ASSERT(!m_recording, "begin_frame while another frame is recording");
        sc.in_frame = true;
        m_recording = true;
        return true;
    }

    void clear(SwapChain& swap_chain, const Color&) override {
        GIANT_ASSERT(static_cast<NullSwapChain&>(swap_chain).in_frame, "clear outside a frame");
    }

    void draw_ui(SwapChain& swap_chain, const UiFrame& ui) override {
        GIANT_ASSERT(static_cast<NullSwapChain&>(swap_chain).in_frame, "draw_ui outside a frame");
        m_ui_draw_calls = check_ui(ui, swap_chain.width(), swap_chain.height());
    }

    TextureId create_ui_texture(u32 width, u32 height, std::span<const u8> pixels) override {
        GIANT_ASSERT(!m_recording, "texture creation while a frame is recording");
        GIANT_ASSERT(width > 0 && height > 0, "empty texture");
        GIANT_ASSERT(pixels.size() == static_cast<usize>(width) * height * 4, "pixel count");
        const TextureId id = m_next_texture++;
        m_textures.insert(id);
        return id;
    }

    void destroy_ui_texture(TextureId texture) override {
        GIANT_ASSERT(!m_recording, "texture destruction while a frame is recording");
        const usize erased = m_textures.erase(texture);
        GIANT_ASSERT(erased == 1, "destroying a texture that does not exist");
    }

    std::vector<u8> render_ui_offscreen(const UiFrame& ui, u32 width, u32 height,
                                        const Color& color) override {
        GIANT_ASSERT(!m_recording, "offscreen work while a frame is recording");
        m_ui_draw_calls = check_ui(ui, width, height);
        const auto to_byte = [](float v) {
            return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        };
        std::vector<u8> pixels(static_cast<usize>(width) * height * 4);
        for (usize i = 0; i < pixels.size(); i += 4) {
            pixels[i + 0] = to_byte(color.r);
            pixels[i + 1] = to_byte(color.g);
            pixels[i + 2] = to_byte(color.b);
            pixels[i + 3] = to_byte(color.a);
        }
        return pixels;
    }

    u32 last_ui_draw_calls() const override { return m_ui_draw_calls; }

    void end_frame(SwapChain& swap_chain) override {
        auto& sc = static_cast<NullSwapChain&>(swap_chain);
        GIANT_ASSERT(sc.in_frame, "end_frame without begin_frame");
        sc.in_frame = false;
        m_recording = false;
        ++m_frames;
    }

    Color clear_offscreen_and_read_back(const Color& color) override {
        GIANT_ASSERT(!m_recording, "offscreen work while a frame is recording");
        return color;
    }
    void wait_idle() override {}
    u64 frames_presented() const override { return m_frames; }
    bool device_lost() const override { return false; }
    u32 validation_errors() const override { return 0; }
    bool validation_enabled() const override { return false; }

private:
    // Checks everything a GPU would trip over, and counts the draws a real
    // backend would submit (clip rectangles clamped to the target, as D3D12
    // does, so both backends count the same).
    u32 check_ui(const UiFrame& ui, u32 target_width, u32 target_height) const {
        const float w = static_cast<float>(target_width);
        const float h = static_cast<float>(target_height);
        u32 draws = 0;
        for (const UiDrawList& list : ui.lists) {
            for (const UiDrawCommand& cmd : list.commands) {
                GIANT_ASSERT(cmd.index_count % 3 == 0, "UI commands draw whole triangles");
                GIANT_ASSERT(static_cast<u64>(cmd.first_index) + cmd.index_count <=
                                 list.indices.size(),
                             "UI command reads past its indices");
                GIANT_ASSERT(m_textures.count(cmd.texture) == 1, "UI command uses a dead texture");
                for (u32 i = 0; i < cmd.index_count && cmd.first_index + i < list.indices.size();
                     ++i) {
                    GIANT_ASSERT(static_cast<u64>(list.indices[cmd.first_index + i]) +
                                         cmd.vertex_offset <
                                     list.vertices.size(),
                                 "UI index points past the vertices");
                }
                const float x0 = std::max(cmd.clip_min_x, 0.0f);
                const float y0 = std::max(cmd.clip_min_y, 0.0f);
                const float x1 = std::min(cmd.clip_max_x, w);
                const float y1 = std::min(cmd.clip_max_y, h);
                if (cmd.index_count > 0 && x1 > x0 && y1 > y0) {
                    ++draws;
                }
            }
        }
        return draws;
    }

    AdapterInfo m_info;
    std::unordered_set<TextureId> m_textures;
    TextureId m_next_texture = 1;
    u32 m_ui_draw_calls = 0;
    u64 m_frames = 0;
    bool m_recording = false;
};

} // namespace

std::unique_ptr<Device> create_null_device() { return std::make_unique<NullDevice>(); }

} // namespace giant::rhi
