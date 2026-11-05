// Direct3D 12 backend.
#include "../backends.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace giant::rhi {

namespace {

using Microsoft::WRL::ComPtr;

constexpr u32 k_max_back_buffers = 3;
constexpr DWORD k_gpu_wait_timeout_ms = 10'000;
constexpr DXGI_FORMAT k_back_buffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
// Flip-model swap chains cannot be created as sRGB, but their views can:
// the renderer writes linear colour and the view encodes it for the screen.
constexpr DXGI_FORMAT k_back_buffer_view_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
// UI is gamma-space, so it draws through a plain UNORM view of the same buffer.
constexpr DXGI_FORMAT k_ui_view_format = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr u32 k_max_ui_textures = 256;

// The UI shader: textured, vertex-coloured triangles in pixel coordinates.
constexpr const char* k_ui_shader = R"(
cbuffer constants : register(b0) { float4x4 projection; };
struct VsIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
struct PsIn { float4 pos : SV_POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
PsIn vs_main(VsIn i) {
    PsIn o;
    o.pos = mul(projection, float4(i.pos, 0.0, 1.0));
    o.color = i.color;
    o.uv = i.uv;
    return o;
}
SamplerState linear_clamp : register(s0);
Texture2D ui_texture : register(t0);
float4 ps_main(PsIn i) : SV_Target { return i.color * ui_texture.Sample(linear_clamp, i.uv); }
)";

// Upload-heap vertex and index buffers for UI, grown on demand. One set per
// back buffer: the frame fence for that buffer guards reuse.
struct UiBuffers {
    ComPtr<ID3D12Resource> vertices;
    ComPtr<ID3D12Resource> indices;
    u64 vertex_capacity = 0; // bytes
    u64 index_capacity = 0;
    u8* vertex_data = nullptr; // persistently mapped
    u8* index_data = nullptr;
};

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) {
        return true;
    }
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    log_error("rhi", std::string("d3d12: ") + what + " failed (" + code + ")");
    return false;
}

std::string narrow(const wchar_t* wide) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<usize>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    return out;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

class D3D12SwapChain final : public SwapChain {
public:
    ~D3D12SwapChain() override;
    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }
    void resize(u32 width, u32 height) override;

    class D3D12Device* device = nullptr;
    ComPtr<IDXGISwapChain3> swap_chain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    std::array<ComPtr<ID3D12Resource>, k_max_back_buffers> buffers;
    std::array<UiBuffers, k_max_back_buffers> ui_buffers;
    u32 rtv_stride = 0;
    u32 buffer_count = 0;
    u32 current = 0; // back buffer being recorded
    bool vsync = true;
    bool allow_tearing = false;
    bool in_frame = false;
    bool valid = false; // false after a failed resize: frames are skipped
    u32 m_width = 0;
    u32 m_height = 0;

    bool acquire_buffers();
    // sRGB view (linear colour in) for the scene and clears.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(u32 index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * rtv_stride;
        return handle;
    }
    // UNORM view (gamma-space colour in) for UI.
    D3D12_CPU_DESCRIPTOR_HANDLE ui_rtv(u32 index) const { return rtv(k_max_back_buffers + index); }
};

class D3D12Device final : public Device {
public:
    ~D3D12Device() override {
        if (m_queue != nullptr && m_fence != nullptr) {
            wait_idle();
        }
        if (m_fence_event != nullptr) {
            CloseHandle(m_fence_event);
        }
    }

    bool init(const DeviceDesc& desc);

    Backend backend() const override { return Backend::d3d12; }
    const AdapterInfo& adapter() const override { return m_info; }

    std::unique_ptr<SwapChain> create_swap_chain(const SwapChainDesc& desc) override;
    bool begin_frame(SwapChain& swap_chain) override;
    void clear(SwapChain& swap_chain, const Color& color) override;
    void end_frame(SwapChain& swap_chain) override;
    Color clear_offscreen_and_read_back(const Color& color) override;
    void wait_idle() override;
    u64 frames_presented() const override { return m_frames; }
    bool device_lost() const override { return m_lost; }
    u32 validation_errors() const override;
    bool validation_enabled() const override { return m_info_queue != nullptr; }

    void draw_ui(SwapChain& swap_chain, const UiFrame& ui) override;
    TextureId create_ui_texture(u32 width, u32 height, std::span<const u8> pixels) override;
    void destroy_ui_texture(TextureId texture) override;
    std::vector<u8> render_ui_offscreen(const UiFrame& ui, u32 width, u32 height,
                                        const Color& color) override;
    u32 last_ui_draw_calls() const override { return m_ui_draw_calls; }

    ID3D12Device* device() const { return m_device.Get(); }

    // Checks an HRESULT; device removal is recorded once with its reason.
    bool check_gpu(HRESULT hr, const char* what);

private:
    bool pick_adapter(bool software, ComPtr<IDXGIAdapter1>& out);
    u64 signal();
    void wait_for(u64 value);

    bool init_ui();
    // Starts and submits one-off work on the immediate allocator; submit
    // waits until the GPU finished it.
    void begin_immediate();
    void submit_immediate();
    ComPtr<ID3D12Resource> create_buffer(D3D12_HEAP_TYPE heap, u64 size,
                                         D3D12_RESOURCE_STATES state, const char* what);
    bool reserve_ui_buffers(UiBuffers& buffers, u64 vertex_bytes, u64 index_bytes);
    // Records UI drawing into m_list. Returns the number of draws.
    u32 record_ui(UiBuffers& buffers, D3D12_CPU_DESCRIPTOR_HANDLE target, u32 width, u32 height,
                  const UiFrame& ui);

    AdapterInfo m_info;
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    // One allocator per back buffer, plus one for immediate (offscreen) work.
    std::array<ComPtr<ID3D12CommandAllocator>, k_max_back_buffers + 1> m_allocators;
    std::array<u64, k_max_back_buffers> m_frame_fence{};
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fence_event = nullptr;
    u64 m_next_fence_value = 1;
    u64 m_frames = 0;
    bool m_tearing_supported = false;
    bool m_recording = false; // a frame is between begin_frame and end_frame
    bool m_lost = false;
    mutable u32 m_validation_errors = 0; // running total, see validation_errors()
    ComPtr<ID3D12InfoQueue> m_info_queue; // set when the debug layer runs

    struct UiTexture {
        ComPtr<ID3D12Resource> resource;
        u32 slot = 0; // in m_srv_heap
    };
    ComPtr<ID3D12RootSignature> m_ui_root;
    ComPtr<ID3D12PipelineState> m_ui_pipeline;
    ComPtr<ID3D12DescriptorHeap> m_srv_heap; // shader-visible, one slot per UI texture
    u32 m_srv_stride = 0;
    std::unordered_map<TextureId, UiTexture> m_ui_textures;
    std::vector<u32> m_free_srv_slots;
    TextureId m_next_texture = 1;
    UiBuffers m_offscreen_ui; // only used between waits for idle
    u32 m_ui_draw_calls = 0;

    friend class D3D12SwapChain;
};

bool D3D12Device::pick_adapter(bool software, ComPtr<IDXGIAdapter1>& out) {
    if (software) {
        return check(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&out)), "EnumWarpAdapter");
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (FAILED(m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         IID_PPV_ARGS(&candidate)))) {
            break; // DXGI_ERROR_NOT_FOUND ends the list; anything else is fatal
        }
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            out = candidate;
            return true;
        }
    }
    log_error("rhi", "d3d12: no GPU supports feature level 12_0");
    return false;
}

bool D3D12Device::init(const DeviceDesc& desc) {
    UINT factory_flags = 0;
    if (desc.debug_layer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
        } else {
            log_warning("rhi", "d3d12: GPU validation layer NOT enabled: install the Windows "
                               "optional feature \"Graphics Tools\"; continuing without it");
        }
    }
    if (!check(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2")) {
        return false;
    }

    BOOL tearing = FALSE;
    if (SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                 sizeof(tearing)))) {
        m_tearing_supported = tearing == TRUE;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (!pick_adapter(desc.software_adapter, adapter)) {
        return false;
    }
    if (!check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)),
               "D3D12CreateDevice")) {
        return false;
    }

    if ((factory_flags & DXGI_CREATE_FACTORY_DEBUG) != 0 &&
        SUCCEEDED(m_device.As(&m_info_queue))) {
        // No storage filter, so warnings still reach the debugger output.
        // validation_errors() counts the errors and clears the queue; the
        // editor calls it regularly so stored messages never pile up.
        m_info_queue->SetMessageCountLimit(UINT64_MAX);
        log_info("rhi", "d3d12: GPU validation layer enabled");
    }

    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    m_info.name = narrow(adapter_desc.Description);
    m_info.software = (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    m_info.dedicated_video_memory = adapter_desc.DedicatedVideoMemory;

    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_6};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model,
                                                sizeof(shader_model)))) {
        m_info.shader_model_6_6 = shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                                sizeof(options7)))) {
        m_info.mesh_shaders = options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!check(m_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&m_queue)),
               "CreateCommandQueue")) {
        return false;
    }
    for (auto& allocator : m_allocators) {
        if (!check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&allocator)),
                   "CreateCommandAllocator")) {
            return false;
        }
    }
    if (!check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_list)),
               "CreateCommandList")) {
        return false;
    }
    m_list->Close();

    if (!check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)),
               "CreateFence")) {
        return false;
    }
    m_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_fence_event == nullptr) {
        log_error("rhi", "d3d12: CreateEvent failed");
        return false;
    }
    return init_ui();
}

bool D3D12Device::check_gpu(HRESULT hr, const char* what) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        if (!m_lost) {
            m_lost = true;
            char reason[16];
            std::snprintf(reason, sizeof(reason), "0x%08lX",
                          static_cast<unsigned long>(m_device->GetDeviceRemovedReason()));
            log_error("rhi", std::string("d3d12: GPU device lost during ") + what +
                                 " (reason " + reason + ")");
        }
        return false;
    }
    return check(hr, what);
}

u32 D3D12Device::validation_errors() const {
    if (m_info_queue == nullptr) {
        return 0;
    }
    const UINT64 count = m_info_queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        m_info_queue->GetMessage(i, nullptr, &size);
        // u64 storage keeps the D3D12_MESSAGE header correctly aligned.
        std::vector<u64> storage((size + sizeof(u64) - 1) / sizeof(u64));
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(m_info_queue->GetMessage(i, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
             message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
            log_error("rhi", std::string("d3d12 validation: ") + message->pDescription);
            ++m_validation_errors;
        }
    }
    // Each message is logged and counted once; the total keeps growing.
    m_info_queue->ClearStoredMessages();
    return m_validation_errors;
}

u64 D3D12Device::signal() {
    const u64 value = m_next_fence_value++;
    if (!check_gpu(m_queue->Signal(m_fence.Get(), value), "Signal")) {
        // Fence tracking is broken from here on: waits could no longer tell
        // when the GPU is done with memory, so stop using the device.
        if (!m_lost) {
            log_error("rhi", "d3d12: Signal failed; treating the device as lost");
            m_lost = true;
        }
        return 0;
    }
    return value;
}

void D3D12Device::wait_for(u64 value) {
    // Once lost, waiting again would only add more timeouts at shutdown.
    if (m_lost || value == 0 || m_fence->GetCompletedValue() >= value) {
        return;
    }
    if (!check(m_fence->SetEventOnCompletion(value, m_fence_event), "SetEventOnCompletion")) {
        return;
    }
    // A healthy GPU finishes a frame in milliseconds. A hung one is treated
    // as lost so the editor can close instead of freezing.
    if (WaitForSingleObject(m_fence_event, k_gpu_wait_timeout_ms) != WAIT_OBJECT_0) {
        log_error("rhi", "d3d12: the GPU did not finish its work in time; treating it as lost");
        m_lost = true;
    }
}

void D3D12Device::wait_idle() { wait_for(signal()); }

std::unique_ptr<SwapChain> D3D12Device::create_swap_chain(const SwapChainDesc& desc) {
    GIANT_ASSERT(desc.buffer_count >= 2 && desc.buffer_count <= k_max_back_buffers,
                 "2 or 3 back buffers");
    if (desc.native_window == nullptr) {
        log_error("rhi", "d3d12: a swap chain needs a window handle");
        return nullptr;
    }
    auto sc = std::make_unique<D3D12SwapChain>();
    sc->device = this;
    sc->buffer_count = desc.buffer_count;
    sc->vsync = desc.vsync;
    sc->allow_tearing = m_tearing_supported && !desc.vsync;

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = desc.width;
    sc_desc.Height = desc.height;
    sc_desc.Format = k_back_buffer_format;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = desc.buffer_count;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Flags = sc->allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    const HWND hwnd = static_cast<HWND>(desc.native_window);
    ComPtr<IDXGISwapChain1> sc1;
    if (!check(m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &sc_desc, nullptr, nullptr,
                                                 &sc1),
               "CreateSwapChainForHwnd")) {
        return nullptr;
    }
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (!check(sc1.As(&sc->swap_chain), "IDXGISwapChain3")) {
        return nullptr;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 2 * k_max_back_buffers; // sRGB and UI views
    if (!check(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&sc->rtv_heap)),
               "CreateDescriptorHeap")) {
        return nullptr;
    }
    sc->rtv_stride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!sc->acquire_buffers()) {
        return nullptr;
    }
    sc->valid = true;
    return sc;
}

D3D12SwapChain::~D3D12SwapChain() {
    // The GPU may still be presenting from these buffers.
    if (device != nullptr) {
        device->wait_idle();
    }
}

bool D3D12SwapChain::acquire_buffers() {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    swap_chain->GetDesc1(&desc);
    m_width = desc.Width;
    m_height = desc.Height;
    for (u32 i = 0; i < buffer_count; ++i) {
        if (!check(swap_chain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])), "GetBuffer")) {
            return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = k_back_buffer_view_format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->device()->CreateRenderTargetView(buffers[i].Get(), &view, rtv(i));
        view.Format = k_ui_view_format;
        device->device()->CreateRenderTargetView(buffers[i].Get(), &view, ui_rtv(i));
    }
    return true;
}

void D3D12SwapChain::resize(u32 width, u32 height) {
    GIANT_ASSERT(!in_frame, "resize inside a frame");
    // Same size is a no-op, unless the last resize failed: then retry.
    if (width == 0 || height == 0 || (valid && width == m_width && height == m_height)) {
        return;
    }
    device->wait_idle();
    if (device->device_lost()) {
        valid = false; // the GPU may still hold the buffers: leave them alone
        return;
    }
    for (auto& buffer : buffers) {
        buffer.Reset();
    }
    const UINT flags = allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    valid = device->check_gpu(
                swap_chain->ResizeBuffers(buffer_count, width, height, DXGI_FORMAT_UNKNOWN, flags),
                "ResizeBuffers") &&
            acquire_buffers();
    if (!valid) {
        log_error("rhi", "d3d12: swap chain resize failed; frames are skipped until the next "
                         "successful resize");
    }
}

bool D3D12Device::begin_frame(SwapChain& swap_chain) {
    auto& sc = static_cast<D3D12SwapChain&>(swap_chain);
    GIANT_ASSERT(!m_recording, "begin_frame while another frame is recording");
    if (m_lost || !sc.valid) {
        return false;
    }
    sc.current = sc.swap_chain->GetCurrentBackBufferIndex();

    // The allocator for this back buffer may still be in use by the GPU.
    wait_for(m_frame_fence[sc.current]);
    if (m_lost) {
        return false;
    }
    sc.in_frame = true;
    m_recording = true;
    ID3D12CommandAllocator* allocator = m_allocators[sc.current].Get();
    allocator->Reset();
    m_list->Reset(allocator, nullptr);

    const auto barrier = transition(sc.buffers[sc.current].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_list->ResourceBarrier(1, &barrier);
    return true;
}

void D3D12Device::clear(SwapChain& swap_chain, const Color& color) {
    auto& sc = static_cast<D3D12SwapChain&>(swap_chain);
    GIANT_ASSERT(sc.in_frame, "clear outside a frame");
    const float rgba[4] = {color.r, color.g, color.b, color.a};
    m_list->ClearRenderTargetView(sc.rtv(sc.current), rgba, 0, nullptr);
}

void D3D12Device::end_frame(SwapChain& swap_chain) {
    auto& sc = static_cast<D3D12SwapChain&>(swap_chain);
    GIANT_ASSERT(sc.in_frame, "end_frame without begin_frame");
    sc.in_frame = false;
    m_recording = false;

    const auto barrier = transition(sc.buffers[sc.current].Get(),
                                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PRESENT);
    m_list->ResourceBarrier(1, &barrier);
    m_list->Close();
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);

    const UINT interval = sc.vsync ? 1 : 0;
    const UINT flags = sc.allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    check_gpu(sc.swap_chain->Present(interval, flags), "Present");
    m_frame_fence[sc.current] = signal();
    ++m_frames;
}

Color D3D12Device::clear_offscreen_and_read_back(const Color& color) {
    GIANT_ASSERT(!m_recording, "offscreen work while a frame is recording");
    if (m_lost) {
        return {-1.0f, -1.0f, -1.0f, -1.0f};
    }
    wait_idle(); // the immediate allocator and list must be free

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = 4;
    tex_desc.Height = 4;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = k_back_buffer_format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = k_back_buffer_format;
    clear_value.Color[0] = color.r;
    clear_value.Color[1] = color.g;
    clear_value.Color[2] = color.b;
    clear_value.Color[3] = color.a;

    ComPtr<ID3D12Resource> texture;
    if (!check(m_device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                                                 IID_PPV_ARGS(&texture)),
               "CreateCommittedResource(texture)")) {
        return {-1.0f, -1.0f, -1.0f, -1.0f};
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total_bytes = 0;
    m_device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                    &total_bytes);

    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = total_bytes;
    buf_desc.Height = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels = 1;
    buf_desc.Format = DXGI_FORMAT_UNKNOWN;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    if (!check(m_device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&readback)),
               "CreateCommittedResource(readback)")) {
        return {-1.0f, -1.0f, -1.0f, -1.0f};
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (!check(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)),
               "CreateDescriptorHeap")) {
        return {-1.0f, -1.0f, -1.0f, -1.0f};
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(texture.Get(), nullptr, rtv);

    ID3D12CommandAllocator* allocator = m_allocators[k_max_back_buffers].Get();
    allocator->Reset();
    m_list->Reset(allocator, nullptr);
    const float rgba[4] = {color.r, color.g, color.b, color.a};
    m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
    const auto barrier = transition(texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = texture.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    m_list->Close();
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    wait_idle();

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, 4};
    if (!check(readback->Map(0, &read_range, &mapped), "Map")) {
        return {-1.0f, -1.0f, -1.0f, -1.0f};
    }
    const auto* bytes = static_cast<const u8*>(mapped);
    const auto unorm = [bytes](int i) { return static_cast<float>(bytes[i]) / 255.0f; };
    const Color result{unorm(0), unorm(1), unorm(2), unorm(3)};
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return result;
}

// --- UI drawing --------------------------------------------------------------

bool D3D12Device::init_ui() {
    // Root signature: the projection as root constants, one texture, one
    // fixed linear-clamp sampler.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = params;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                      D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                      D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                      D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                                           &errors))) {
        log_error("rhi", std::string("d3d12: UI root signature: ") +
                             (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?"));
        return false;
    }
    if (!check(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_ui_root)),
               "CreateRootSignature(UI)")) {
        return false;
    }

    // Shaders compile at start-up with the system's d3dcompiler_47.dll. The
    // N1 renderer moves to Slang and precompiled shaders.
    const auto compile = [](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        ComPtr<ID3DBlob> shader_errors;
        const HRESULT hr = D3DCompile(k_ui_shader, std::strlen(k_ui_shader), "ui.hlsl", nullptr,
                                      nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                      &out, &shader_errors);
        if (FAILED(hr)) {
            log_error("rhi", std::string("d3d12: UI shader ") + entry + ": " +
                                 (shader_errors ? static_cast<const char*>(
                                                      shader_errors->GetBufferPointer())
                                                : "compile failed"));
            return false;
        }
        return true;
    };
    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> ps;
    if (!compile("vs_main", "vs_5_0", vs) || !compile("ps_main", "ps_5_0", ps)) {
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
    };
    static_assert(sizeof(UiVertex) == 20, "the input layout above matches UiVertex");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_ui_root.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.InputLayout = {layout, static_cast<UINT>(std::size(layout))};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = k_ui_view_format;
    pso.SampleDesc.Count = 1;
    if (!check(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_ui_pipeline)),
               "CreateGraphicsPipelineState(UI)")) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = k_max_ui_textures;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!check(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_srv_heap)),
               "CreateDescriptorHeap(UI textures)")) {
        return false;
    }
    m_srv_stride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (u32 slot = k_max_ui_textures; slot > 0; --slot) {
        m_free_srv_slots.push_back(slot - 1); // hand out slot 0 first
    }
    return true;
}

void D3D12Device::begin_immediate() {
    wait_idle(); // the immediate allocator and the list must be free
    ID3D12CommandAllocator* allocator = m_allocators[k_max_back_buffers].Get();
    allocator->Reset();
    m_list->Reset(allocator, nullptr);
}

void D3D12Device::submit_immediate() {
    m_list->Close();
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    wait_idle();
}

ComPtr<ID3D12Resource> D3D12Device::create_buffer(D3D12_HEAP_TYPE heap_type, u64 size,
                                                  D3D12_RESOURCE_STATES state, const char* what) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (!check_gpu(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                     nullptr, IID_PPV_ARGS(&buffer)),
                   what)) {
        return nullptr;
    }
    return buffer;
}

bool D3D12Device::reserve_ui_buffers(UiBuffers& buffers, u64 vertex_bytes, u64 index_bytes) {
    const auto grow = [this](ComPtr<ID3D12Resource>& buffer, u64& capacity, u8*& data, u64 needed,
                             const char* what) {
        if (needed <= capacity) {
            return true;
        }
        u64 size = std::max<u64>(capacity, 64 * 1024);
        while (size < needed) {
            size *= 2;
        }
        // The caller made sure the GPU is done with the old buffer.
        buffer = create_buffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ, what);
        capacity = 0;
        data = nullptr;
        if (buffer == nullptr) {
            return false;
        }
        const D3D12_RANGE no_read{0, 0};
        void* mapped = nullptr;
        if (!check(buffer->Map(0, &no_read, &mapped), "Map(UI buffer)")) {
            buffer.Reset();
            return false;
        }
        data = static_cast<u8*>(mapped);
        capacity = size;
        return true;
    };
    return grow(buffers.vertices, buffers.vertex_capacity, buffers.vertex_data, vertex_bytes,
                "UI vertex buffer") &&
           grow(buffers.indices, buffers.index_capacity, buffers.index_data, index_bytes,
                "UI index buffer");
}

u32 D3D12Device::record_ui(UiBuffers& buffers, D3D12_CPU_DESCRIPTOR_HANDLE target, u32 width,
                           u32 height, const UiFrame& ui) {
    u64 vertex_count = 0;
    u64 index_count = 0;
    for (const UiDrawList& list : ui.lists) {
        vertex_count += list.vertices.size();
        index_count += list.indices.size();
    }
    if (vertex_count == 0 || index_count == 0 || width == 0 || height == 0) {
        return 0;
    }
    if (!reserve_ui_buffers(buffers, vertex_count * sizeof(UiVertex), index_count * sizeof(u32))) {
        return 0;
    }
    u64 vertex_at = 0;
    u64 index_at = 0;
    for (const UiDrawList& list : ui.lists) {
        if (!list.vertices.empty()) {
            std::memcpy(buffers.vertex_data + vertex_at * sizeof(UiVertex), list.vertices.data(),
                        list.vertices.size_bytes());
        }
        if (!list.indices.empty()) {
            std::memcpy(buffers.index_data + index_at * sizeof(u32), list.indices.data(),
                        list.indices.size_bytes());
        }
        vertex_at += list.vertices.size();
        index_at += list.indices.size();
    }

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    // Pixels to clip space (column-major, as the shader's mul expects).
    const float projection[16] = {2.0f / w, 0.0f,      0.0f, 0.0f, //
                                  0.0f,     -2.0f / h, 0.0f, 0.0f, //
                                  0.0f,     0.0f,      0.5f, 0.0f, //
                                  -1.0f,    1.0f,      0.5f, 1.0f};

    m_list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, w, h, 0.0f, 1.0f};
    m_list->RSSetViewports(1, &viewport);
    m_list->SetGraphicsRootSignature(m_ui_root.Get());
    m_list->SetPipelineState(m_ui_pipeline.Get());
    ID3D12DescriptorHeap* heaps[] = {m_srv_heap.Get()};
    m_list->SetDescriptorHeaps(1, heaps);
    m_list->SetGraphicsRoot32BitConstants(0, 16, projection, 0);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = buffers.vertices->GetGPUVirtualAddress();
    vbv.SizeInBytes = static_cast<UINT>(vertex_count * sizeof(UiVertex));
    vbv.StrideInBytes = sizeof(UiVertex);
    m_list->IASetVertexBuffers(0, 1, &vbv);
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = buffers.indices->GetGPUVirtualAddress();
    ibv.SizeInBytes = static_cast<UINT>(index_count * sizeof(u32));
    ibv.Format = DXGI_FORMAT_R32_UINT;
    m_list->IASetIndexBuffer(&ibv);

    u32 draws = 0;
    u64 list_vertex_base = 0;
    u64 list_index_base = 0;
    for (const UiDrawList& list : ui.lists) {
        for (const UiDrawCommand& cmd : list.commands) {
            if (static_cast<u64>(cmd.first_index) + cmd.index_count > list.indices.size()) {
                GIANT_ASSERT(false, "UI command reads past its indices");
                continue;
            }
            const auto texture = m_ui_textures.find(cmd.texture);
            if (texture == m_ui_textures.end()) {
                GIANT_ASSERT(false, "UI command uses a dead texture");
                continue;
            }
            const float x0 = std::max(cmd.clip_min_x, 0.0f);
            const float y0 = std::max(cmd.clip_min_y, 0.0f);
            const float x1 = std::min(cmd.clip_max_x, w);
            const float y1 = std::min(cmd.clip_max_y, h);
            if (cmd.index_count == 0 || x1 <= x0 || y1 <= y0) {
                continue;
            }
            const D3D12_RECT scissor{static_cast<LONG>(x0), static_cast<LONG>(y0),
                                     static_cast<LONG>(std::ceil(x1)),
                                     static_cast<LONG>(std::ceil(y1))};
            m_list->RSSetScissorRects(1, &scissor);
            D3D12_GPU_DESCRIPTOR_HANDLE srv = m_srv_heap->GetGPUDescriptorHandleForHeapStart();
            srv.ptr += static_cast<UINT64>(texture->second.slot) * m_srv_stride;
            m_list->SetGraphicsRootDescriptorTable(1, srv);
            m_list->DrawIndexedInstanced(cmd.index_count, 1,
                                         static_cast<UINT>(list_index_base + cmd.first_index),
                                         static_cast<INT>(list_vertex_base + cmd.vertex_offset), 0);
            ++draws;
        }
        list_vertex_base += list.vertices.size();
        list_index_base += list.indices.size();
    }
    return draws;
}

void D3D12Device::draw_ui(SwapChain& swap_chain, const UiFrame& ui) {
    auto& sc = static_cast<D3D12SwapChain&>(swap_chain);
    GIANT_ASSERT(sc.in_frame, "draw_ui outside a frame");
    // begin_frame waited for this back buffer's fence, so its UI buffers are free.
    m_ui_draw_calls = record_ui(sc.ui_buffers[sc.current], sc.ui_rtv(sc.current), sc.width(),
                                sc.height(), ui);
}

TextureId D3D12Device::create_ui_texture(u32 width, u32 height, std::span<const u8> pixels) {
    GIANT_ASSERT(!m_recording, "texture creation while a frame is recording");
    if (width == 0 || height == 0 || pixels.size() != static_cast<usize>(width) * height * 4) {
        log_error("rhi", "d3d12: create_ui_texture needs width * height * 4 bytes");
        return 0;
    }
    if (m_lost || m_free_srv_slots.empty()) {
        log_error("rhi", m_lost ? "d3d12: device lost" : "d3d12: too many UI textures");
        return 0;
    }

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = k_ui_view_format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    UiTexture texture;
    if (!check_gpu(m_device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&texture.resource)),
                   "CreateCommittedResource(UI texture)")) {
        return 0;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total_bytes = 0;
    m_device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
    ComPtr<ID3D12Resource> upload = create_buffer(D3D12_HEAP_TYPE_UPLOAD, total_bytes,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  "UI texture upload");
    if (upload == nullptr) {
        return 0;
    }
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    if (!check(upload->Map(0, &no_read, &mapped), "Map(UI texture upload)")) {
        return 0;
    }
    const usize row_bytes = static_cast<usize>(width) * 4;
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(static_cast<u8*>(mapped) + footprint.Offset +
                        static_cast<usize>(y) * footprint.Footprint.RowPitch,
                    pixels.data() + static_cast<usize>(y) * row_bytes, row_bytes);
    }
    upload->Unmap(0, nullptr);

    begin_immediate();
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto barrier = transition(texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_list->ResourceBarrier(1, &barrier);
    submit_immediate(); // the upload buffer may be released after this
    if (m_lost) {
        return 0;
    }

    texture.slot = m_free_srv_slots.back();
    m_free_srv_slots.pop_back();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = k_ui_view_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(texture.slot) * m_srv_stride;
    m_device->CreateShaderResourceView(texture.resource.Get(), &srv, handle);

    const TextureId id = m_next_texture++;
    m_ui_textures.emplace(id, std::move(texture));
    return id;
}

void D3D12Device::destroy_ui_texture(TextureId texture) {
    GIANT_ASSERT(!m_recording, "texture destruction while a frame is recording");
    const auto it = m_ui_textures.find(texture);
    if (it == m_ui_textures.end()) {
        GIANT_ASSERT(false, "destroying a texture that does not exist");
        return;
    }
    wait_idle(); // earlier frames may still sample it
    m_free_srv_slots.push_back(it->second.slot);
    m_ui_textures.erase(it);
}

std::vector<u8> D3D12Device::render_ui_offscreen(const UiFrame& ui, u32 width, u32 height,
                                                 const Color& color) {
    GIANT_ASSERT(!m_recording, "offscreen work while a frame is recording");
    if (m_lost || width == 0 || height == 0) {
        return {};
    }
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = k_ui_view_format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = k_ui_view_format;
    clear_value.Color[0] = color.r;
    clear_value.Color[1] = color.g;
    clear_value.Color[2] = color.b;
    clear_value.Color[3] = color.a;
    ComPtr<ID3D12Resource> target;
    if (!check_gpu(m_device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                     &clear_value, IID_PPV_ARGS(&target)),
                   "CreateCommittedResource(UI offscreen)")) {
        return {};
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total_bytes = 0;
    m_device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
    ComPtr<ID3D12Resource> readback = create_buffer(D3D12_HEAP_TYPE_READBACK, total_bytes,
                                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                                    "UI readback buffer");
    if (readback == nullptr) {
        return {};
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (!check(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)),
               "CreateDescriptorHeap(UI offscreen)")) {
        return {};
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    begin_immediate(); // waits for idle, so m_offscreen_ui is free too
    const float rgba[4] = {color.r, color.g, color.b, color.a};
    m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
    m_ui_draw_calls = record_ui(m_offscreen_ui, rtv, width, height, ui);
    const auto barrier = transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    submit_immediate();
    if (m_lost) {
        return {};
    }

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
    if (!check(readback->Map(0, &read_range, &mapped), "Map(UI readback)")) {
        return {};
    }
    std::vector<u8> pixels(static_cast<usize>(width) * height * 4);
    const usize row_bytes = static_cast<usize>(width) * 4;
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(pixels.data() + static_cast<usize>(y) * row_bytes,
                    static_cast<const u8*>(mapped) + footprint.Offset +
                        static_cast<usize>(y) * footprint.Footprint.RowPitch,
                    row_bytes);
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return pixels;
}

} // namespace

std::unique_ptr<Device> create_d3d12_device(const DeviceDesc& desc) {
    auto device = std::make_unique<D3D12Device>();
    if (!device->init(desc)) {
        return nullptr;
    }
    return device;
}

} // namespace giant::rhi
