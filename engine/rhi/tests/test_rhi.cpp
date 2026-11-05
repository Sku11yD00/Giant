#include "giant/rhi/rhi.h"

#include "giant_test.h"

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
