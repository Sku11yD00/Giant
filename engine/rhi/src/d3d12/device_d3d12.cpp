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
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace giant::rhi {

namespace {

using Microsoft::WRL::ComPtr;

constexpr u32 k_max_back_buffers = 3;
constexpr DXGI_FORMAT k_back_buffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;

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
    u32 rtv_stride = 0;
    u32 buffer_count = 0;
    u32 current = 0; // back buffer being recorded
    bool vsync = true;
    bool allow_tearing = false;
    bool in_frame = false;
    u32 m_width = 0;
    u32 m_height = 0;

    bool acquire_buffers();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(u32 index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * rtv_stride;
        return handle;
    }
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
    void begin_frame(SwapChain& swap_chain) override;
    void clear(SwapChain& swap_chain, const Color& color) override;
    void end_frame(SwapChain& swap_chain) override;
    Color clear_offscreen_and_read_back(const Color& color) override;
    void wait_idle() override;
    u64 frames_presented() const override { return m_frames; }

    ID3D12Device* device() const { return m_device.Get(); }

private:
    bool pick_adapter(bool software, ComPtr<IDXGIAdapter1>& out);
    u64 signal();
    void wait_for(u64 value);

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

    friend class D3D12SwapChain;
};

bool D3D12Device::pick_adapter(bool software, ComPtr<IDXGIAdapter1>& out) {
    if (software) {
        return check(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&out)), "EnumWarpAdapter");
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                  IID_PPV_ARGS(&candidate)) ==
            DXGI_ERROR_NOT_FOUND) {
            break;
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
            log_warning("rhi", "d3d12: debug layer not installed (Windows optional feature "
                               "\"Graphics Tools\"); continuing without it");
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
    return true;
}

u64 D3D12Device::signal() {
    const u64 value = m_next_fence_value++;
    check(m_queue->Signal(m_fence.Get(), value), "Signal");
    return value;
}

void D3D12Device::wait_for(u64 value) {
    if (value == 0 || m_fence->GetCompletedValue() >= value) {
        return;
    }
    if (check(m_fence->SetEventOnCompletion(value, m_fence_event), "SetEventOnCompletion")) {
        WaitForSingleObject(m_fence_event, INFINITE);
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
    heap_desc.NumDescriptors = k_max_back_buffers;
    if (!check(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&sc->rtv_heap)),
               "CreateDescriptorHeap")) {
        return nullptr;
    }
    sc->rtv_stride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!sc->acquire_buffers()) {
        return nullptr;
    }
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
        device->device()->CreateRenderTargetView(buffers[i].Get(), nullptr, rtv(i));
    }
    return true;
}

void D3D12SwapChain::resize(u32 width, u32 height) {
    GIANT_ASSERT(!in_frame, "resize inside a frame");
    if (width == 0 || height == 0 || (width == m_width && height == m_height)) {
        return;
    }
    device->wait_idle();
    for (auto& buffer : buffers) {
        buffer.Reset();
    }
    const UINT flags = allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (check(swap_chain->ResizeBuffers(buffer_count, width, height, DXGI_FORMAT_UNKNOWN, flags),
              "ResizeBuffers")) {
        acquire_buffers();
    }
}

void D3D12Device::begin_frame(SwapChain& swap_chain) {
    auto& sc = static_cast<D3D12SwapChain&>(swap_chain);
    GIANT_ASSERT(!sc.in_frame, "begin_frame called twice");
    sc.in_frame = true;
    sc.current = sc.swap_chain->GetCurrentBackBufferIndex();

    // The allocator for this back buffer may still be in use by the GPU.
    wait_for(m_frame_fence[sc.current]);
    ID3D12CommandAllocator* allocator = m_allocators[sc.current].Get();
    allocator->Reset();
    m_list->Reset(allocator, nullptr);

    const auto barrier = transition(sc.buffers[sc.current].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_list->ResourceBarrier(1, &barrier);
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

    const auto barrier = transition(sc.buffers[sc.current].Get(),
                                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PRESENT);
    m_list->ResourceBarrier(1, &barrier);
    m_list->Close();
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);

    const UINT interval = sc.vsync ? 1 : 0;
    const UINT flags = sc.allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    check(sc.swap_chain->Present(interval, flags), "Present");
    m_frame_fence[sc.current] = signal();
    ++m_frames;
}

Color D3D12Device::clear_offscreen_and_read_back(const Color& color) {
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

} // namespace

std::unique_ptr<Device> create_d3d12_device(const DeviceDesc& desc) {
    auto device = std::make_unique<D3D12Device>();
    if (!device->init(desc)) {
        return nullptr;
    }
    return device;
}

} // namespace giant::rhi
