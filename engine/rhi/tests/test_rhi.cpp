#include "giant/rhi/rhi.h"

#include "giant_test.h"

#include <array>
#include <cstdlib>
#include <vector>

using namespace giant;
using namespace giant::rhi;

namespace {
void check_color_near(const Color& got, const Color& want) {
    // 8-bit render targets: allow one step of rounding.
    const double tolerance = 1.0 / 255.0 + 1e-6;
    GIANT_CHECK_NEAR(got.r, want.r, tolerance);
    GIANT_CHECK_NEAR(got.g, want.g, tolerance);
    GIANT_CHECK_NEAR(got.b, want.b, tolerance);
    GIANT_CHECK_NEAR(got.a, want.a, tolerance);
}
// A UI frame with one rectangle, for the UI drawing tests.
struct UiQuad {
    std::vector<UiVertex> vertices;
    std::vector<u32> indices{0, 1, 2, 0, 2, 3};
    std::vector<UiDrawCommand> commands;
    std::vector<UiDrawList> lists;

    UiQuad(float x0, float y0, float x1, float y1, u32 color, TextureId texture,
           float clip_x1 = 1e9f) {
        vertices = {{x0, y0, 0.0f, 0.0f, color},
                    {x1, y0, 1.0f, 0.0f, color},
                    {x1, y1, 1.0f, 1.0f, color},
                    {x0, y1, 0.0f, 1.0f, color}};
        UiDrawCommand cmd;
        cmd.index_count = 6;
        cmd.clip_max_x = clip_x1;
        cmd.clip_max_y = 1e9f;
        cmd.texture = texture;
        commands = {cmd};
        lists = {{vertices, indices, commands}};
    }
    UiFrame frame() const { return {lists}; }
};

constexpr u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

// One RGBA8 pixel of an image `width` pixels wide.
std::array<int, 4> pixel(const std::vector<u8>& image, u32 width, u32 x, u32 y) {
    const usize i = (static_cast<usize>(y) * width + x) * 4;
    return {image[i], image[i + 1], image[i + 2], image[i + 3]};
}

bool near(const std::array<int, 4>& got, const std::array<int, 4>& want, int tolerance = 2) {
    for (usize i = 0; i < 4; ++i) {
        if (std::abs(got[i] - want[i]) > tolerance) {
            return false;
        }
    }
    return true;
}
} // namespace

GIANT_TEST("rhi: the null device renders frames in order") {
    DeviceDesc desc;
    desc.backend = Backend::null;
    auto device = create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device == nullptr) {
        return;
    }
    GIANT_CHECK(device->backend() == Backend::null);

    SwapChainDesc sc_desc;
    sc_desc.width = 640;
    sc_desc.height = 360;
    auto swap_chain = device->create_swap_chain(sc_desc);
    GIANT_CHECK(swap_chain != nullptr);
    if (swap_chain == nullptr) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        GIANT_CHECK(device->begin_frame(*swap_chain));
        device->clear(*swap_chain, {0.2f, 0.4f, 0.6f, 1.0f});
        device->end_frame(*swap_chain);
    }
    GIANT_CHECK(device->frames_presented() == 3);
    GIANT_CHECK(!device->device_lost());

    swap_chain->resize(0, 0); // minimised: ignored
    GIANT_CHECK(swap_chain->width() == 640);
    swap_chain->resize(800, 600);
    GIANT_CHECK(swap_chain->width() == 800 && swap_chain->height() == 600);
}

GIANT_TEST("rhi: the null device reads back what it cleared") {
    DeviceDesc desc;
    desc.backend = Backend::null;
    auto device = create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device != nullptr) {
        check_color_near(device->clear_offscreen_and_read_back({1.0f, 0.5f, 0.0f, 1.0f}),
                         {1.0f, 0.5f, 0.0f, 1.0f});
    }
}

GIANT_TEST("rhi: the null device checks and counts UI draws") {
    DeviceDesc desc;
    desc.backend = Backend::null;
    auto device = create_device(desc);
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    const std::vector<u8> white(4, 255);
    const TextureId texture = device->create_ui_texture(1, 1, white);
    GIANT_CHECK(texture != 0);

    UiQuad quad(0, 0, 10, 10, rgba(255, 0, 0, 255), texture);
    // A second command with an empty clip rectangle is skipped.
    UiDrawCommand hidden = quad.commands[0];
    hidden.clip_max_x = 0.0f;
    quad.commands.push_back(hidden);
    quad.lists = {{quad.vertices, quad.indices, quad.commands}};

    SwapChainDesc sc_desc;
    sc_desc.width = 64;
    sc_desc.height = 64;
    auto swap_chain = device->create_swap_chain(sc_desc);
    GIANT_CHECK(device->begin_frame(*swap_chain));
    device->clear(*swap_chain, {});
    device->draw_ui(*swap_chain, quad.frame());
    device->end_frame(*swap_chain);
    GIANT_CHECK(device->last_ui_draw_calls() == 1);

    const auto image = device->render_ui_offscreen(quad.frame(), 8, 4, {1.0f, 0.5f, 0.0f, 1.0f});
    GIANT_CHECK(image.size() == 8 * 4 * 4);
    GIANT_CHECK(near(pixel(image, 8, 7, 3), {255, 128, 0, 255}, 1));
    device->destroy_ui_texture(texture);
}

#if GIANT_RHI_HAS_D3D12
// Runs on every Windows CI build, which has no GPU: WARP is Microsoft's
// software rasterizer, so this proves real D3D12 command submission works.
GIANT_TEST("rhi: d3d12 clears and reads back on the software adapter") {
    DeviceDesc desc;
    desc.backend = Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device == nullptr) {
        return;
    }
    GIANT_CHECK(device->adapter().software);
#if defined(GIANT_REQUIRE_VALIDATION)
    GIANT_CHECK(device->validation_enabled());
#endif
    GIANT_CHECK(!device->adapter().name.empty());
    check_color_near(device->clear_offscreen_and_read_back({0.25f, 0.5f, 0.75f, 1.0f}),
                     {0.25f, 0.5f, 0.75f, 1.0f});
    // A second run reuses the immediate allocator.
    check_color_near(device->clear_offscreen_and_read_back({1.0f, 0.0f, 0.0f, 1.0f}),
                     {1.0f, 0.0f, 0.0f, 1.0f});
    GIANT_CHECK(!device->device_lost());
    // When the validation layer runs, it must have nothing to complain about.
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 draws UI with clipping and gamma-space blending") {
    DeviceDesc desc;
    desc.backend = Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device == nullptr) {
        return;
    }
    const std::vector<u8> white(4, 255);
    const TextureId texture = device->create_ui_texture(1, 1, white);
    GIANT_CHECK(texture != 0);

    // A red rectangle over the whole image, clipped to the left half.
    const UiQuad red(0, 0, 64, 32, rgba(255, 0, 0, 255), texture, 32.0f);
    const auto image = device->render_ui_offscreen(red.frame(), 64, 32, {0.0f, 0.0f, 1.0f, 1.0f});
    GIANT_CHECK(image.size() == 64 * 32 * 4);
    if (image.size() == 64 * 32 * 4) {
        GIANT_CHECK(near(pixel(image, 64, 10, 10), {255, 0, 0, 255}));
        GIANT_CHECK(near(pixel(image, 64, 50, 10), {0, 0, 255, 255})); // clipped away
    }
    GIANT_CHECK(device->last_ui_draw_calls() == 1);

    // Half-transparent white over black blends to the halfway byte value,
    // as UI colours are picked, not the sRGB-encoded linear halfway point.
    const UiQuad grey(0, 0, 16, 16, rgba(255, 255, 255, 128), texture);
    const auto blended = device->render_ui_offscreen(grey.frame(), 16, 16, {0.0f, 0.0f, 0.0f, 1.0f});
    if (blended.size() == 16 * 16 * 4) {
        const auto p = pixel(blended, 16, 8, 8);
        GIANT_CHECK(std::abs(p[0] - 128) <= 2);
    } else {
        GIANT_CHECK(false);
    }

    // A texture is sampled: a 2x1 texture, black then white.
    const std::vector<u8> ramp = {0, 0, 0, 255, 255, 255, 255, 255};
    const TextureId ramp_texture = device->create_ui_texture(2, 1, ramp);
    const UiQuad textured(0, 0, 32, 8, rgba(255, 255, 255, 255), ramp_texture);
    const auto sampled = device->render_ui_offscreen(textured.frame(), 32, 8, {1.0f, 0.0f, 0.0f, 1.0f});
    if (sampled.size() == 32 * 8 * 4) {
        GIANT_CHECK(pixel(sampled, 32, 1, 4)[0] < 40);   // black side
        GIANT_CHECK(pixel(sampled, 32, 30, 4)[1] > 215); // white side
    } else {
        GIANT_CHECK(false);
    }
    device->destroy_ui_texture(ramp_texture);
    device->destroy_ui_texture(texture);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 refuses a swap chain without a window") {
    DeviceDesc desc;
    desc.backend = Backend::d3d12;
    desc.software_adapter = true;
    auto device = create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device != nullptr) {
        SwapChainDesc sc_desc;
        sc_desc.width = 64;
        sc_desc.height = 64;
        GIANT_CHECK(device->create_swap_chain(sc_desc) == nullptr);
    }
}
#endif
