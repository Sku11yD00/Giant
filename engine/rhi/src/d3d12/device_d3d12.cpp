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
    bool valid = false; // false after a failed resize: frames are skipped
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
    bool begin_frame(SwapChain& swap_chain) override;
    void clear(SwapChain& swap_chain, const Color& color) override;
    void end_frame(SwapChain& swap_chain) override;
    Color clear_offscreen_and_read_back(const Color& color) override;
    void wait_idle() override;
    u64 frames_presented() const override { return m_frames; }
    bool device_lost() const override { return m_lost; }
    u32 validation_errors() const override;
    bool validation_enabled() const override { return m_info_queue != nullptr; }

    ID3D12Device* device() const { return m_device.Get(); }

    // Checks an HRESULT; device removal is recorded once with its reason.
    bool check_gpu(HRESULT hr, const char* what);

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
    bool m_recording = false; // a frame is between begin_frame and end_frame
    bool m_lost = false;
    mutable u32 m_validation_errors = 0; // running total, see validation_errors()
    ComPtr<ID3D12InfoQueue> m_info_queue; // set when the debug layer runs

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
    return true;
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
    heap_desc.NumDescriptors = k_max_back_buffers;
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

} // namespace

std::unique_ptr<Device> create_d3d12_device(const DeviceDesc& desc) {
    auto device = std::make_unique<D3D12Device>();
    if (!device->init(desc)) {
        return nullptr;
    }
    return device;
}

} // namespace giant::rhi
