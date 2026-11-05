// The thin graphics layer. Everything above it (render, editor) talks to the
// GPU only through these types, so a Vulkan backend can be added later.
//
// N0 scope: create a device, check the GPU against the minimum spec, open a
// swap chain on a window, clear and present. Command lists, buffers,
// textures, pipelines and bindless resources arrive with N1, which will also
// move frames in flight onto the device (a ring of per-frame allocators per
// recording thread) so several swap chains and headless frames work; see
// docs/architecture.md, "Graphics layer plan".
#pragma once

#include "giant/core/types.h"

#include <memory>
#include <string>

namespace giant::rhi {

enum class Backend {
    null,  // no GPU: records calls, for tests and headless tools
    d3d12, // Windows
};

const char* to_string(Backend backend);

// The best backend this build supports on this platform.
Backend default_backend();

struct DeviceDesc {
    Backend backend = default_backend();
    bool debug_layer = false;      // GPU validation; slower, for development
    bool software_adapter = false; // D3D12: use WARP (CPU) instead of a GPU
};

struct AdapterInfo {
    std::string name;
    bool software = false;
    u64 dedicated_video_memory = 0; // bytes
    bool mesh_shaders = false;
    bool shader_model_6_6 = false;

    // The engine's minimum spec (DirectX 12 Ultimate class). Ray tracing is
    // deliberately not required.
    bool meets_minimum_spec() const { return mesh_shaders && shader_model_6_6; }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct SwapChainDesc {
    void* native_window = nullptr; // HWND on Windows; ignored by the null backend
    u32 width = 0;
    u32 height = 0;
    u32 buffer_count = 3; // 2 or 3
    bool vsync = true;
};

// Destroy every swap chain before the device that created it.
class SwapChain {
public:
    virtual ~SwapChain() = default;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
    // Waits for the GPU and resizes the back buffers. A zero size (minimised
    // window) is ignored.
    virtual void resize(u32 width, u32 height) = 0;
};

class Device {
public:
    virtual ~Device() = default;

    virtual Backend backend() const = 0;
    virtual const AdapterInfo& adapter() const = 0;

    // Returns null (and logs why) on failure.
    virtual std::unique_ptr<SwapChain> create_swap_chain(const SwapChainDesc& desc) = 0;

    // One frame: begin_frame, any number of clears, end_frame presents.
    // begin_frame returns false when the frame must be skipped (device lost,
    // or the swap chain could not be resized); do not call clear/end_frame then.
    // Only one frame may be recording on a device at a time.
    virtual bool begin_frame(SwapChain& swap_chain) = 0;
    virtual void clear(SwapChain& swap_chain, const Color& color) = 0;
    virtual void end_frame(SwapChain& swap_chain) = 0;

    // Clears a small offscreen texture on the GPU and reads one pixel back.
    // Proves the device really renders; used by tests and startup checks.
    // Must not be called while a frame is recording.
    virtual Color clear_offscreen_and_read_back(const Color& color) = 0;

    // Blocks until the GPU has finished all submitted work.
    virtual void wait_idle() = 0;

    // Frames presented since the device was created.
    virtual u64 frames_presented() const = 0;

    // True once the GPU was removed or reset (driver crash, update, TDR).
    // The device cannot recover; the caller must recreate it.
    virtual bool device_lost() const = 0;

    // Errors reported by the GPU validation layer since the device was
    // created. Always 0 when the debug layer is off or unavailable.
    virtual u32 validation_errors() const = 0;
    // Whether the validation layer is really running.
    virtual bool validation_enabled() const = 0;
};

// Returns null (and logs why) if the backend is unavailable or fails.
std::unique_ptr<Device> create_device(const DeviceDesc& desc);

} // namespace giant::rhi
