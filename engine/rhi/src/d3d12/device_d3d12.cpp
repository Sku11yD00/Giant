// Direct3D 12 backend (Agility SDK runtime, shader model 6.6).
//
// Layout: one global root signature (128 bytes of root constants at b0, and
// direct indexing into one shader-visible CBV/SRV/UAV heap and one sampler
// heap), so every pipeline shares it and draws only push indices.
// Resources are committed allocations for now; placed allocation from
// larger heaps arrives with streaming (N1 part 4).
#include "../backends.h"
#include "../pool.h"

#include "rhi_shaders.h"

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

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace giant::rhi {

namespace {

using Microsoft::WRL::ComPtr;

constexpr u32 k_frames = Device::k_frames_in_flight;
constexpr u32 k_max_back_buffers = 3;
constexpr DWORD k_gpu_wait_timeout_ms = 10'000;
constexpr u32 k_max_shader_descriptors = 1u << 16;
constexpr u32 k_max_rtvs = 1024;
constexpr u32 k_max_dsvs = 256;
constexpr u32 k_sampler_count = 4;
constexpr u32 k_push_dwords = k_max_push_constant_bytes / 4;
constexpr u32 k_transient_chunk_bytes = 4u << 20;
// Flip-model swap chains cannot be created as sRGB, but their views can:
// the renderer writes linear colour and the view encodes it for the screen.
constexpr DXGI_FORMAT k_swap_chain_format = DXGI_FORMAT_R8G8B8A8_UNORM;

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

std::wstring widen(const char* text) {
    std::wstring out;
    for (const char* c = text; c != nullptr && *c != '\0'; ++c) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    }
    return out;
}

// --- Formats -----------------------------------------------------------------

// What the resource is created as: typeless where views differ in type
// (sRGB and UNORM views of colour, depth and float views of depth).
DXGI_FORMAT resource_format(Format f) {
    switch (f) {
    case Format::rgba8_unorm:
    case Format::rgba8_srgb: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case Format::rgba16_float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::r32_float: return DXGI_FORMAT_R32_FLOAT;
    case Format::r32_uint: return DXGI_FORMAT_R32_UINT;
    case Format::rg32_uint: return DXGI_FORMAT_R32G32_UINT;
    case Format::depth32_float: return DXGI_FORMAT_R32_TYPELESS;
    case Format::unknown: break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

// The view format for sampling and render targets.
DXGI_FORMAT view_format(Format f) {
    switch (f) {
    case Format::rgba8_unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::rgba8_srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::depth32_float: return DXGI_FORMAT_R32_FLOAT; // for sampling
    default: return resource_format(f);
    }
}

// Unordered access cannot write sRGB; it writes the raw UNORM bytes.
DXGI_FORMAT write_view_format(Format f) {
    return f == Format::rgba8_srgb ? DXGI_FORMAT_R8G8B8A8_UNORM : view_format(f);
}

DXGI_FORMAT target_format(Format f) {
    return f == Format::depth32_float ? DXGI_FORMAT_D32_FLOAT : view_format(f);
}

D3D12_RESOURCE_STATES to_d3d(ResourceState s) {
    switch (s) {
    case ResourceState::common: return D3D12_RESOURCE_STATE_COMMON;
    case ResourceState::shader_read:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case ResourceState::shader_write: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case ResourceState::render_target: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case ResourceState::depth_write: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case ResourceState::depth_read: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case ResourceState::index_buffer: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
    case ResourceState::indirect_argument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case ResourceState::copy_source: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case ResourceState::copy_dest: return D3D12_RESOURCE_STATE_COPY_DEST;
    case ResourceState::present: return D3D12_RESOURCE_STATE_PRESENT;
    }
    return D3D12_RESOURCE_STATE_COMMON;
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

D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

// --- Descriptor slots ----------------------------------------------------------

class DescriptorHeap {
public:
    bool init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 count, bool shader_visible,
              const char* what) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = count;
        desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                    : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (!check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)), what)) {
            return false;
        }
        m_stride = device->GetDescriptorHandleIncrementSize(type);
        m_capacity = count;
        return true;
    }

    // k_no_descriptor when full (logged).
    u32 allocate() {
        if (!m_free.empty()) {
            const u32 slot = m_free.back();
            m_free.pop_back();
            return slot;
        }
        if (m_next >= m_capacity) {
            log_error("rhi", "d3d12: out of descriptors");
            return k_no_descriptor;
        }
        return m_next++;
    }
    void free(u32 slot) {
        if (slot != k_no_descriptor) {
            m_free.push_back(slot);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(u32 slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(slot) * m_stride;
        return h;
    }
    ID3D12DescriptorHeap* heap() const { return m_heap.Get(); }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    u32 m_stride = 0;
    u32 m_capacity = 0;
    u32 m_next = 0;
    std::vector<u32> m_free;
};

// --- Resources -------------------------------------------------------------------

struct DxBuffer {
    ComPtr<ID3D12Resource> resource;
    BufferDesc desc;
    ResourceState state = ResourceState::common;
    u32 read = k_no_descriptor;
    u32 write = k_no_descriptor;
    u8* mapped = nullptr;
};

struct DxTexture {
    ComPtr<ID3D12Resource> resource;
    TextureDesc desc;
    ResourceState state = ResourceState::common;
    u32 read = k_no_descriptor;
    u32 write = k_no_descriptor;
    u32 rtv = k_no_descriptor;    // in the format's own view (sRGB for rgba8_srgb)
    u32 ui_rtv = k_no_descriptor; // UNORM view of rgba8 targets, for gamma-space UI
    u32 dsv = k_no_descriptor;
};

struct DxPipeline {
    ComPtr<ID3D12PipelineState> state;
    bool compute = false;
    std::array<Format, k_max_color_targets> color_formats{};
    u32 color_count = 0;
    Format depth_format = Format::unknown;
};

// What a destroyed resource leaves behind until the GPU is done with it.
struct Retired {
    u64 fence = 0;
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<u32, 3> shader_slots{k_no_descriptor, k_no_descriptor, k_no_descriptor};
    std::array<u32, 2> rtv_slots{k_no_descriptor, k_no_descriptor};
    u32 dsv_slot = k_no_descriptor;
};

struct TransientChunk {
    BufferHandle buffer;
    u32 size = 0;
    u32 used = 0;
};

struct Frame {
    ComPtr<ID3D12CommandAllocator> allocator;
    u64 fence = 0; // signalled when the GPU finished this frame's work
    std::vector<TransientChunk> chunks;
};

class D3D12Device;

class D3D12SwapChain final : public SwapChain {
public:
    ~D3D12SwapChain() override;
    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }
    TextureHandle current_texture() const override { return textures[current]; }
    void resize(u32 width, u32 height) override;

    D3D12Device* device = nullptr;
    ComPtr<IDXGISwapChain3> swap_chain;
    std::array<TextureHandle, k_max_back_buffers> textures{};
    u32 buffer_count = 0;
    u32 current = 0;
    bool vsync = true;
    bool allow_tearing = false;
    bool valid = false; // false after a failed resize: frames are skipped
    u32 m_width = 0;
    u32 m_height = 0;

    bool acquire_buffers();
    void release_buffers();
};

class D3D12CommandList final : public CommandList {
public:
    explicit D3D12CommandList(D3D12Device& device) : m_device(device) {}

    void barrier(BufferHandle buffer, ResourceState state) override;
    void barrier(TextureHandle texture, ResourceState state) override;
    void begin_render_pass(const RenderPassDesc& desc) override;
    void end_render_pass() override;
    void set_pipeline(PipelineHandle pipeline) override;
    void set_push_constants(const void* data, u32 bytes) override;
    void set_scissor(u32 x, u32 y, u32 width, u32 height) override;
    void draw(u32 vertex_count, u32 instance_count, u32 first_vertex) override;
    void set_index_buffer(BufferHandle buffer, u64 offset) override;
    void draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                      i32 vertex_offset) override;
    void dispatch(u32 x, u32 y, u32 z) override;
    void copy_buffer(BufferHandle dst, u64 dst_offset, BufferHandle src, u64 src_offset,
                     u64 bytes) override;
    TransientMemory allocate(u32 bytes) override;
    void draw_ui(TextureHandle target, const UiFrame& ui) override;

    // Starts recording into `list` (already reset) for frame `frame`.
    void begin(ID3D12GraphicsCommandList* list, Frame* frame);
    void end();
    ID3D12GraphicsCommandList* list() const { return m_list; }

private:
    void flush_push(bool compute);

    D3D12Device& m_device;
    ID3D12GraphicsCommandList* m_list = nullptr;
    Frame* m_frame = nullptr;
    bool m_in_pass = false;
    u32 m_pass_width = 0;
    u32 m_pass_height = 0;
    PipelineHandle m_pipeline;
    bool m_pipeline_compute = false;
    std::array<u32, k_push_dwords> m_push{};
    u32 m_push_dwords = 0;
};

class D3D12Device final : public Device {
public:
    D3D12Device() : m_commands(*this) {}
    ~D3D12Device() override;

    bool init(const DeviceDesc& desc);

    Backend backend() const override { return Backend::d3d12; }
    const AdapterInfo& adapter() const override { return m_info; }

    std::unique_ptr<SwapChain> create_swap_chain(const SwapChainDesc& desc) override;
    BufferHandle create_buffer(const BufferDesc& desc, std::span<const u8> initial) override;
    TextureHandle create_texture(const TextureDesc& desc, std::span<const u8> initial) override;
    PipelineHandle create_pipeline(const GraphicsPipelineDesc& desc) override;
    PipelineHandle create_pipeline(const ComputePipelineDesc& desc) override;
    void destroy(BufferHandle buffer) override;
    void destroy(TextureHandle texture) override;
    void destroy(PipelineHandle pipeline) override;

    u32 read_descriptor(BufferHandle h) const override {
        const DxBuffer* b = m_buffers.get(h);
        return b != nullptr ? b->read : k_no_descriptor;
    }
    u32 write_descriptor(BufferHandle h) const override {
        const DxBuffer* b = m_buffers.get(h);
        return b != nullptr ? b->write : k_no_descriptor;
    }
    u32 read_descriptor(TextureHandle h) const override {
        const DxTexture* t = m_textures.get(h);
        return t != nullptr ? t->read : k_no_descriptor;
    }
    u32 write_descriptor(TextureHandle h) const override {
        const DxTexture* t = m_textures.get(h);
        return t != nullptr ? t->write : k_no_descriptor;
    }
    const BufferDesc* describe(BufferHandle h) const override {
        const DxBuffer* b = m_buffers.get(h);
        return b != nullptr ? &b->desc : nullptr;
    }
    const TextureDesc* describe(TextureHandle h) const override {
        const DxTexture* t = m_textures.get(h);
        return t != nullptr ? &t->desc : nullptr;
    }
    u8* mapped(BufferHandle h) override {
        DxBuffer* b = m_buffers.get(h);
        GIANT_ASSERT(b != nullptr && b->mapped != nullptr,
                     "mapped: needs a live upload or readback buffer");
        return b != nullptr ? b->mapped : nullptr;
    }

    CommandList* begin_frame(SwapChain* swap_chain) override;
    void end_frame() override;
    const FrameStats& last_frame_stats() const override { return m_last_stats; }
    std::vector<u8> read_texture(TextureHandle texture) override;
    std::vector<u8> read_buffer(BufferHandle buffer) override;
    void wait_idle() override;
    u64 frames_completed() const override { return m_frames; }
    bool device_lost() const override { return m_lost; }
    u32 validation_errors() const override;
    bool validation_enabled() const override { return m_info_queue != nullptr; }

    // For the command list and swap chains.
    ID3D12Device* device() const { return m_device.Get(); }
    DxBuffer* buffer(BufferHandle h) { return m_buffers.get(h); }
    DxTexture* texture(TextureHandle h) { return m_textures.get(h); }
    DxPipeline* pipeline(PipelineHandle h) { return m_pipelines.get(h); }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(u32 slot) const { return m_rtv_heap.cpu(slot); }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(u32 slot) const { return m_dsv_heap.cpu(slot); }
    PipelineHandle ui_pipeline() const { return m_ui_pipeline; }
    FrameStats& stats() { return m_stats; }
    bool recording() const { return m_recording; }
    bool check_gpu(HRESULT hr, const char* what);
    void bind_globals(ID3D12GraphicsCommandList* list);
    TextureHandle add_swap_chain_texture(ComPtr<ID3D12Resource> resource, u32 width, u32 height);
    void remove_swap_chain_texture(TextureHandle texture);
    BufferHandle create_transient_chunk(u32 size);
    // Buffers decay to COMMON when the list that used them finishes (D3D12's
    // implicit state rules), so the tracked state follows after each submit.
    void track_decay(BufferHandle buffer) { m_decaying.push_back(buffer); }

private:
    void decay_buffer_states();
    bool pick_adapter(bool software, ComPtr<IDXGIAdapter1>& out);
    bool init_globals();
    u64 signal();
    void wait_for(u64 value);
    void retire(Retired retired);
    void free_retired();
    // One-off work on its own list; submit waits until the GPU finished it.
    ID3D12GraphicsCommandList* begin_immediate();
    bool submit_immediate();
    ComPtr<ID3D12Resource> create_committed_buffer(D3D12_HEAP_TYPE heap, u64 size,
                                                   D3D12_RESOURCE_STATES state,
                                                   D3D12_RESOURCE_FLAGS flags, const char* name);

    AdapterInfo m_info;
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fence_event = nullptr;
    u64 m_next_fence_value = 1;
    ComPtr<ID3D12InfoQueue> m_info_queue; // set when the debug layer runs
    mutable u32 m_validation_errors = 0;
    bool m_tearing_supported = false;
    bool m_lost = false;

    ComPtr<ID3D12RootSignature> m_root;
    DescriptorHeap m_shader_heap;  // CBV/SRV/UAV, shader-visible
    DescriptorHeap m_sampler_heap; // shader-visible, the fixed samplers
    DescriptorHeap m_rtv_heap;
    DescriptorHeap m_dsv_heap;

    Pool<DxBuffer, BufferTag> m_buffers;
    Pool<DxTexture, TextureTag> m_textures;
    Pool<DxPipeline, PipelineTag> m_pipelines;
    std::deque<Retired> m_retired; // in fence order
    // Destroyed while a frame records: stamped with that frame's fence in
    // end_frame (an immediate submit mid-frame signals an earlier value).
    std::vector<Retired> m_retired_this_frame;

    std::array<Frame, k_frames> m_frame_slots;
    u64 m_frame_index = 0; // frames begun
    ComPtr<ID3D12GraphicsCommandList> m_list;
    D3D12CommandList m_commands;
    D3D12SwapChain* m_swap_chain = nullptr; // of the frame being recorded
    bool m_recording = false;
    u64 m_frames = 0;
    FrameStats m_stats;
    FrameStats m_last_stats;

    ComPtr<ID3D12CommandAllocator> m_immediate_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_immediate_list;

    PipelineHandle m_ui_pipeline;
    std::vector<BufferHandle> m_decaying; // gpu buffers moved out of COMMON

    friend class D3D12SwapChain;
};

// --- Device setup ------------------------------------------------------------------

bool D3D12Device::pick_adapter(bool software, ComPtr<IDXGIAdapter1>& out) {
    if (software) {
        return check(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&out)), "EnumWarpAdapter");
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (FAILED(m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         IID_PPV_ARGS(&candidate)))) {
            break; // DXGI_ERROR_NOT_FOUND ends the list
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
            log_warning("rhi", "d3d12: GPU validation layer NOT enabled (d3d12SDKLayers.dll "
                               "missing from the D3D12 folder); continuing without it");
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
        m_info_queue->SetMessageCountLimit(UINT64_MAX);
        log_info("rhi", "d3d12: GPU validation layer enabled");
    }

    // Which D3D12 runtime loaded: ours (exe folder\D3D12\D3D12Core.dll), or
    // Windows' own when the D3D12 folder is missing or its version differs.
    if (HMODULE core = GetModuleHandleW(L"D3D12Core.dll")) {
        wchar_t core_path[MAX_PATH] = {};
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(core, core_path, MAX_PATH);
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const std::filesystem::path core_file(core_path);
        const std::filesystem::path exe_file(exe_path);
        m_info.agility_sdk_runtime =
            core_file.parent_path().filename() == L"D3D12" &&
            core_file.parent_path().parent_path() == exe_file.parent_path();
    }
    if (!m_info.agility_sdk_runtime) {
        log_warning("rhi", "d3d12: using the Windows D3D12 runtime, not the bundled Agility SDK "
                           "(is the D3D12 folder next to the exe?)");
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
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (!m_info.shader_model_6_6 || options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
        log_error("rhi", "d3d12: this GPU lacks shader model 6.6 bindless resources (resource "
                         "binding tier 3); the engine needs them. Update the graphics driver.");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!check(m_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&m_queue)),
               "CreateCommandQueue")) {
        return false;
    }
    for (Frame& frame : m_frame_slots) {
        if (!check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&frame.allocator)),
                   "CreateCommandAllocator")) {
            return false;
        }
    }
    if (!check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&m_immediate_allocator)),
               "CreateCommandAllocator") ||
        !check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           m_frame_slots[0].allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&m_list)),
               "CreateCommandList") ||
        !check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           m_immediate_allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&m_immediate_list)),
               "CreateCommandList")) {
        return false;
    }
    m_list->Close();
    m_immediate_list->Close();
    if (!check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)),
               "CreateFence")) {
        return false;
    }
    m_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_fence_event == nullptr) {
        log_error("rhi", "d3d12: CreateEvent failed");
        return false;
    }
    return init_globals();
}

bool D3D12Device::init_globals() {
    if (!m_shader_heap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                            k_max_shader_descriptors, true, "CreateDescriptorHeap(shader)") ||
        !m_sampler_heap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, k_sampler_count,
                             true, "CreateDescriptorHeap(sampler)") ||
        !m_rtv_heap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, k_max_rtvs, false,
                         "CreateDescriptorHeap(rtv)") ||
        !m_dsv_heap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, k_max_dsvs, false,
                         "CreateDescriptorHeap(dsv)")) {
        return false;
    }

    // The fixed samplers, at the indices rhi::Sampler names.
    const auto sampler = [](D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address, UINT aniso) {
        D3D12_SAMPLER_DESC s{};
        s.Filter = filter;
        s.AddressU = s.AddressV = s.AddressW = address;
        s.MaxAnisotropy = aniso;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        return s;
    };
    const D3D12_SAMPLER_DESC samplers[k_sampler_count] = {
        sampler(D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 1),
        sampler(D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 1),
        sampler(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 1),
        sampler(D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 8),
    };
    for (u32 i = 0; i < k_sampler_count; ++i) {
        const u32 slot = m_sampler_heap.allocate();
        m_device->CreateSampler(&samplers[i], m_sampler_heap.cpu(slot));
    }

    // The one root signature: push constants, and the heaps indexed directly.
    D3D12_ROOT_PARAMETER1 push{};
    push.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    push.Constants.ShaderRegister = 0;
    push.Constants.RegisterSpace = 0;
    push.Constants.Num32BitValues = k_push_dwords;
    push.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_desc.Desc_1_1.NumParameters = 1;
    root_desc.Desc_1_1.pParameters = &push;
    root_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                               D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeVersionedRootSignature(&root_desc, &blob, &errors))) {
        log_error("rhi", std::string("d3d12: root signature: ") +
                             (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?"));
        return false;
    }
    if (!check(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_root)),
               "CreateRootSignature")) {
        return false;
    }

    GraphicsPipelineDesc ui;
    ui.vertex = shaders::ui_vs();
    ui.pixel = shaders::ui_ps();
    ui.color_formats[0] = Format::rgba8_unorm;
    ui.color_count = 1;
    ui.cull = CullMode::none;
    ui.blend = BlendMode::alpha;
    ui.name = "UI";
    m_ui_pipeline = create_pipeline(ui);
    return m_ui_pipeline.valid();
}

D3D12Device::~D3D12Device() {
    if (m_queue != nullptr && m_fence != nullptr) {
        wait_idle();
    }
    // Everything is idle: release in any order.
    for (Frame& frame : m_frame_slots) {
        for (const TransientChunk& chunk : frame.chunks) {
            destroy(chunk.buffer);
        }
        frame.chunks.clear();
    }
    destroy(m_ui_pipeline);
    m_retired.clear();
    if (m_fence_event != nullptr) {
        CloseHandle(m_fence_event);
    }
}

bool D3D12Device::check_gpu(HRESULT hr, const char* what) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        if (!m_lost) {
            m_lost = true;
            char reason[16];
            std::snprintf(reason, sizeof(reason), "0x%08lX",
                          static_cast<unsigned long>(m_device->GetDeviceRemovedReason()));
            log_error("rhi", std::string("d3d12: GPU device lost during ") + what + " (reason " +
                                 reason + ")");
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
    m_info_queue->ClearStoredMessages();
    return m_validation_errors;
}

// --- Fences and retirement ------------------------------------------------------

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

void D3D12Device::wait_idle() {
    GIANT_ASSERT(!m_recording, "wait_idle inside a frame");
    wait_for(signal());
    free_retired();
}

void D3D12Device::retire(Retired retired) {
    if (m_recording) {
        // The frame being recorded may use it; it signals in end_frame.
        m_retired_this_frame.push_back(std::move(retired));
        return;
    }
    // Everything submitted so far signals at most this value when done.
    retired.fence = m_next_fence_value;
    m_retired.push_back(std::move(retired));
}

void D3D12Device::free_retired() {
    // A lost device never completes its fences; nothing it holds can be
    // freed safely, and nothing new runs, so keep it all until destruction.
    const u64 done = m_lost ? 0 : m_fence->GetCompletedValue();
    while (!m_retired.empty() && m_retired.front().fence <= done) {
        Retired& r = m_retired.front();
        for (u32 slot : r.shader_slots) {
            m_shader_heap.free(slot);
        }
        for (u32 slot : r.rtv_slots) {
            m_rtv_heap.free(slot);
        }
        m_dsv_heap.free(r.dsv_slot);
        m_retired.pop_front();
    }
}

ID3D12GraphicsCommandList* D3D12Device::begin_immediate() {
    m_immediate_allocator->Reset();
    m_immediate_list->Reset(m_immediate_allocator.Get(), nullptr);
    bind_globals(m_immediate_list.Get());
    return m_immediate_list.Get();
}

void D3D12Device::decay_buffer_states() {
    for (const BufferHandle handle : m_decaying) {
        if (DxBuffer* b = m_buffers.get(handle)) {
            b->state = ResourceState::common;
        }
    }
    m_decaying.clear();
}

bool D3D12Device::submit_immediate() {
    if (!check_gpu(m_immediate_list->Close(), "Close(immediate)")) {
        return false;
    }
    ID3D12CommandList* lists[] = {m_immediate_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    wait_for(signal());
    return !m_lost;
}

void D3D12Device::bind_globals(ID3D12GraphicsCommandList* list) {
    ID3D12DescriptorHeap* heaps[] = {m_shader_heap.heap(), m_sampler_heap.heap()};
    list->SetDescriptorHeaps(2, heaps); // before the root signature, for direct indexing
    list->SetGraphicsRootSignature(m_root.Get());
    list->SetComputeRootSignature(m_root.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// --- Buffers -------------------------------------------------------------------------

ComPtr<ID3D12Resource> D3D12Device::create_committed_buffer(D3D12_HEAP_TYPE heap_type, u64 size,
                                                            D3D12_RESOURCE_STATES state,
                                                            D3D12_RESOURCE_FLAGS flags,
                                                            const char* name) {
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
    desc.Flags = flags;
    ComPtr<ID3D12Resource> buffer;
    if (!check_gpu(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                     nullptr, IID_PPV_ARGS(&buffer)),
                   name)) {
        return nullptr;
    }
    buffer->SetName(widen(name).c_str());
    return buffer;
}

BufferHandle D3D12Device::create_buffer(const BufferDesc& desc, std::span<const u8> initial) {
    GIANT_ASSERT(desc.size > 0 && desc.size % 4 == 0, "buffer size must be a positive multiple of 4");
    GIANT_ASSERT(initial.size() <= desc.size, "initial data larger than the buffer");
    GIANT_ASSERT(desc.memory == Memory::gpu || !has(desc.usage, BufferUsage::shader_write),
                 "only gpu-memory buffers can be written by shaders");
    if (m_lost) {
        return {};
    }
    DxBuffer b;
    b.desc = desc;
    D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (desc.memory == Memory::upload) {
        heap = D3D12_HEAP_TYPE_UPLOAD;
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if (desc.memory == Memory::readback) {
        heap = D3D12_HEAP_TYPE_READBACK;
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    const D3D12_RESOURCE_FLAGS flags = has(desc.usage, BufferUsage::shader_write)
                                           ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                           : D3D12_RESOURCE_FLAG_NONE;
    b.resource = create_committed_buffer(heap, desc.size, state, flags, desc.name);
    if (b.resource == nullptr) {
        return {};
    }
    if (desc.memory != Memory::gpu) {
        void* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        if (!check(b.resource->Map(0, desc.memory == Memory::readback ? nullptr : &no_read, &mapped),
                   "Map(buffer)")) {
            return {};
        }
        b.mapped = static_cast<u8*>(mapped);
    }

    if (!initial.empty()) {
        if (b.mapped != nullptr) {
            std::memcpy(b.mapped, initial.data(), initial.size());
        } else {
            ComPtr<ID3D12Resource> staging =
                create_committed_buffer(D3D12_HEAP_TYPE_UPLOAD, initial.size(),
                                        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                                        "staging");
            void* mapped = nullptr;
            const D3D12_RANGE no_read{0, 0};
            if (staging == nullptr || !check(staging->Map(0, &no_read, &mapped), "Map(staging)")) {
                return {};
            }
            std::memcpy(mapped, initial.data(), initial.size());
            staging->Unmap(0, nullptr);
            ID3D12GraphicsCommandList* list = begin_immediate();
            // Buffers start in COMMON, which decays to COPY_DEST for a copy.
            list->CopyBufferRegion(b.resource.Get(), 0, staging.Get(), 0, initial.size());
            if (!submit_immediate()) {
                return {};
            }
            // Back in COMMON after the queue finished: that is our state.
        }
    }

    if (has(desc.usage, BufferUsage::shader_read)) {
        b.read = m_shader_heap.allocate();
        if (b.read == k_no_descriptor) {
            return {};
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = static_cast<UINT>(desc.size / 4);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_device->CreateShaderResourceView(b.resource.Get(), &srv, m_shader_heap.cpu(b.read));
    }
    if (has(desc.usage, BufferUsage::shader_write)) {
        b.write = m_shader_heap.allocate();
        if (b.write == k_no_descriptor) {
            m_shader_heap.free(b.read);
            return {};
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = static_cast<UINT>(desc.size / 4);
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        m_device->CreateUnorderedAccessView(b.resource.Get(), nullptr, &uav,
                                            m_shader_heap.cpu(b.write));
    }
    return m_buffers.add(std::move(b));
}

void D3D12Device::destroy(BufferHandle handle) {
    if (!handle.valid()) {
        return;
    }
    DxBuffer b = m_buffers.take(handle);
    Retired r;
    r.resource = std::move(b.resource);
    r.shader_slots = {b.read, b.write, k_no_descriptor};
    retire(std::move(r));
}

BufferHandle D3D12Device::create_transient_chunk(u32 size) {
    BufferDesc desc;
    desc.size = size;
    desc.memory = Memory::upload;
    desc.usage = BufferUsage::shader_read;
    desc.name = "transient";
    return create_buffer(desc, {});
}

// --- Textures ------------------------------------------------------------------------

TextureHandle D3D12Device::create_texture(const TextureDesc& desc, std::span<const u8> initial) {
    GIANT_ASSERT(desc.width > 0 && desc.height > 0 && desc.format != Format::unknown,
                 "texture needs a size and a format");
    GIANT_ASSERT(desc.usage != TextureUsage::none, "texture needs a usage");
    GIANT_ASSERT(is_depth(desc.format) == has(desc.usage, TextureUsage::depth_target),
                 "depth formats are for depth targets and depth targets need depth formats");
    GIANT_ASSERT(!(is_depth(desc.format) && (has(desc.usage, TextureUsage::render_target) ||
                                             has(desc.usage, TextureUsage::shader_write))),
                 "a depth texture cannot be a colour target or shader-written");
    const u32 pixel_bytes = bytes_per_pixel(desc.format);
    const usize size = static_cast<usize>(desc.width) * desc.height * pixel_bytes;
    GIANT_ASSERT(initial.empty() || initial.size() == size, "initial pixels have the wrong size");
    if (m_lost) {
        return {};
    }

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc.width;
    rd.Height = desc.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = resource_format(desc.format);
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (has(desc.usage, TextureUsage::render_target)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (has(desc.usage, TextureUsage::depth_target)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!has(desc.usage, TextureUsage::sampled)) {
            rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }
    }
    if (has(desc.usage, TextureUsage::shader_write)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    DxTexture t;
    t.desc = desc;
    if (!check_gpu(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&t.resource)),
                   "CreateCommittedResource(texture)")) {
        return {};
    }
    t.resource->SetName(widen(desc.name).c_str());

    if (!initial.empty()) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 total = 0;
        m_device->GetCopyableFootprints(&rd, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> staging = create_committed_buffer(
            D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ,
            D3D12_RESOURCE_FLAG_NONE, "texture staging");
        void* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        if (staging == nullptr || !check(staging->Map(0, &no_read, &mapped), "Map(staging)")) {
            return {};
        }
        const usize row = static_cast<usize>(desc.width) * pixel_bytes;
        for (u32 y = 0; y < desc.height; ++y) {
            std::memcpy(static_cast<u8*>(mapped) + footprint.Offset +
                            static_cast<usize>(y) * footprint.Footprint.RowPitch,
                        initial.data() + static_cast<usize>(y) * row, row);
        }
        staging->Unmap(0, nullptr);
        ID3D12GraphicsCommandList* list = begin_immediate();
        const auto to_copy = transition(t.resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = t.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const auto back = transition(t.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &back);
        if (!submit_immediate()) {
            return {};
        }
    }

    bool ok = true;
    if (has(desc.usage, TextureUsage::sampled)) {
        t.read = m_shader_heap.allocate();
        ok = ok && t.read != k_no_descriptor;
        if (ok) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = view_format(desc.format);
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(t.resource.Get(), &srv, m_shader_heap.cpu(t.read));
        }
    }
    if (ok && has(desc.usage, TextureUsage::shader_write)) {
        t.write = m_shader_heap.allocate();
        ok = t.write != k_no_descriptor;
        if (ok) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = write_view_format(desc.format);
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            m_device->CreateUnorderedAccessView(t.resource.Get(), nullptr, &uav,
                                                m_shader_heap.cpu(t.write));
        }
    }
    if (ok && has(desc.usage, TextureUsage::render_target)) {
        t.rtv = m_rtv_heap.allocate();
        ok = t.rtv != k_no_descriptor;
        if (ok) {
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = target_format(desc.format);
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            m_device->CreateRenderTargetView(t.resource.Get(), &rtv, m_rtv_heap.cpu(t.rtv));
            if (desc.format == Format::rgba8_srgb) {
                t.ui_rtv = m_rtv_heap.allocate();
                ok = t.ui_rtv != k_no_descriptor;
                if (ok) {
                    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    m_device->CreateRenderTargetView(t.resource.Get(), &rtv,
                                                     m_rtv_heap.cpu(t.ui_rtv));
                }
            } else {
                t.ui_rtv = t.rtv; // shared slot: freed once, see destroy
            }
        }
    }
    if (ok && has(desc.usage, TextureUsage::depth_target)) {
        t.dsv = m_dsv_heap.allocate();
        ok = t.dsv != k_no_descriptor;
        if (ok) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = DXGI_FORMAT_D32_FLOAT;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            m_device->CreateDepthStencilView(t.resource.Get(), &dsv, m_dsv_heap.cpu(t.dsv));
        }
    }
    const TextureHandle handle = m_textures.add(std::move(t));
    if (!ok) {
        destroy(handle);
        return {};
    }
    return handle;
}

void D3D12Device::destroy(TextureHandle handle) {
    if (!handle.valid()) {
        return;
    }
    DxTexture t = m_textures.take(handle);
    Retired r;
    r.resource = std::move(t.resource);
    r.shader_slots = {t.read, t.write, k_no_descriptor};
    r.rtv_slots = {t.rtv, t.ui_rtv != t.rtv ? t.ui_rtv : k_no_descriptor};
    r.dsv_slot = t.dsv;
    retire(std::move(r));
}

TextureHandle D3D12Device::add_swap_chain_texture(ComPtr<ID3D12Resource> resource, u32 width,
                                                  u32 height) {
    DxTexture t;
    t.desc.width = width;
    t.desc.height = height;
    t.desc.format = Format::rgba8_srgb;
    t.desc.usage = TextureUsage::render_target;
    t.desc.name = "back buffer";
    t.state = ResourceState::present;
    t.rtv = m_rtv_heap.allocate();
    t.ui_rtv = m_rtv_heap.allocate();
    if (t.rtv == k_no_descriptor || t.ui_rtv == k_no_descriptor) {
        m_rtv_heap.free(t.rtv);
        m_rtv_heap.free(t.ui_rtv);
        return {};
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(resource.Get(), &rtv, m_rtv_heap.cpu(t.rtv));
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_device->CreateRenderTargetView(resource.Get(), &rtv, m_rtv_heap.cpu(t.ui_rtv));
    t.resource = std::move(resource);
    return m_textures.add(std::move(t));
}

void D3D12Device::remove_swap_chain_texture(TextureHandle handle) {
    // Called after a wait for idle: free at once, the swap chain needs its
    // buffers released before ResizeBuffers.
    if (!handle.valid() || m_textures.get(handle) == nullptr) {
        return;
    }
    DxTexture t = m_textures.take(handle);
    m_rtv_heap.free(t.rtv);
    m_rtv_heap.free(t.ui_rtv);
}

// --- Pipelines -----------------------------------------------------------------------

PipelineHandle D3D12Device::create_pipeline(const GraphicsPipelineDesc& desc) {
    GIANT_ASSERT(desc.color_count <= k_max_color_targets, "too many colour targets");
    GIANT_ASSERT(desc.depth == DepthMode::none || is_depth(desc.depth_format),
                 "depth testing needs a depth format");
    if (desc.vertex.empty() || desc.pixel.empty()) {
        log_error("rhi", std::string("d3d12: pipeline '") + desc.name +
                             "' has no shader code (built without GIANT_COMPILE_SHADERS?)");
        return {};
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_root.Get();
    pso.VS = {desc.vertex.data(), desc.vertex.size()};
    pso.PS = {desc.pixel.data(), desc.pixel.size()};
    for (u32 i = 0; i < desc.color_count; ++i) {
        auto& blend = pso.BlendState.RenderTarget[i];
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        if (desc.blend == BlendMode::alpha) {
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        pso.RTVFormats[i] = target_format(desc.color_formats[i]);
    }
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = desc.cull == CullMode::none   ? D3D12_CULL_MODE_NONE
                                   : desc.cull == CullMode::back ? D3D12_CULL_MODE_BACK
                                                                 : D3D12_CULL_MODE_FRONT;
    pso.RasterizerState.FrontCounterClockwise = TRUE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = desc.depth != DepthMode::none;
    pso.DepthStencilState.DepthWriteMask = desc.depth == DepthMode::test_write
                                               ? D3D12_DEPTH_WRITE_MASK_ALL
                                               : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reverse-Z
    pso.DSVFormat = desc.depth_format != Format::unknown ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = desc.color_count;
    pso.SampleDesc.Count = 1;
    DxPipeline p;
    if (!check_gpu(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&p.state)),
                   desc.name)) {
        return {};
    }
    p.state->SetName(widen(desc.name).c_str());
    p.color_formats = desc.color_formats;
    p.color_count = desc.color_count;
    p.depth_format = desc.depth_format;
    return m_pipelines.add(std::move(p));
}

PipelineHandle D3D12Device::create_pipeline(const ComputePipelineDesc& desc) {
    if (desc.compute.empty()) {
        log_error("rhi", std::string("d3d12: pipeline '") + desc.name + "' has no shader code");
        return {};
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_root.Get();
    pso.CS = {desc.compute.data(), desc.compute.size()};
    DxPipeline p;
    p.compute = true;
    if (!check_gpu(m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&p.state)), desc.name)) {
        return {};
    }
    p.state->SetName(widen(desc.name).c_str());
    return m_pipelines.add(std::move(p));
}

void D3D12Device::destroy(PipelineHandle handle) {
    if (!handle.valid()) {
        return;
    }
    DxPipeline p = m_pipelines.take(handle);
    Retired r;
    r.pipeline = std::move(p.state);
    retire(std::move(r));
}

// --- Swap chains ---------------------------------------------------------------------

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
    sc_desc.Format = k_swap_chain_format;
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
    if (!check(sc1.As(&sc->swap_chain), "IDXGISwapChain3") || !sc->acquire_buffers()) {
        return nullptr;
    }
    sc->valid = true;
    return sc;
}

D3D12SwapChain::~D3D12SwapChain() {
    // The GPU may still be presenting from these buffers.
    if (device != nullptr) {
        device->wait_idle();
        release_buffers();
    }
}

bool D3D12SwapChain::acquire_buffers() {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    swap_chain->GetDesc1(&desc);
    m_width = desc.Width;
    m_height = desc.Height;
    for (u32 i = 0; i < buffer_count; ++i) {
        ComPtr<ID3D12Resource> buffer;
        if (!check(swap_chain->GetBuffer(i, IID_PPV_ARGS(&buffer)), "GetBuffer")) {
            return false;
        }
        textures[i] = device->add_swap_chain_texture(std::move(buffer), m_width, m_height);
        if (!textures[i].valid()) {
            return false;
        }
    }
    return true;
}

void D3D12SwapChain::release_buffers() {
    for (TextureHandle& texture : textures) {
        device->remove_swap_chain_texture(texture);
        texture = {};
    }
}

void D3D12SwapChain::resize(u32 width, u32 height) {
    GIANT_ASSERT(!device->recording(), "resize inside a frame");
    // Same size is a no-op, unless the last resize failed: then retry.
    if (width == 0 || height == 0 || (valid && width == m_width && height == m_height)) {
        return;
    }
    device->wait_idle();
    if (device->device_lost()) {
        valid = false; // the GPU may still hold the buffers: leave them alone
        return;
    }
    release_buffers();
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

// --- Frames --------------------------------------------------------------------------

CommandList* D3D12Device::begin_frame(SwapChain* swap_chain) {
    GIANT_ASSERT(!m_recording, "begin_frame while another frame is recording");
    auto* sc = static_cast<D3D12SwapChain*>(swap_chain);
    if (m_lost || (sc != nullptr && !sc->valid)) {
        return nullptr;
    }
    Frame& frame = m_frame_slots[m_frame_index % k_frames];
    // This slot's allocator and transient memory may still be in use.
    wait_for(frame.fence);
    if (m_lost) {
        return nullptr;
    }
    free_retired();
    for (TransientChunk& chunk : frame.chunks) {
        chunk.used = 0;
    }
    frame.allocator->Reset();
    m_list->Reset(frame.allocator.Get(), nullptr);
    bind_globals(m_list.Get());
    if (sc != nullptr) {
        sc->current = sc->swap_chain->GetCurrentBackBufferIndex();
    }
    m_swap_chain = sc;
    m_recording = true;
    m_stats = {};
    m_commands.begin(m_list.Get(), &frame);
    return &m_commands;
}

void D3D12Device::end_frame() {
    GIANT_ASSERT(m_recording, "end_frame without begin_frame");
    if (m_swap_chain != nullptr) {
        m_commands.barrier(m_swap_chain->current_texture(), ResourceState::present);
    }
    m_commands.end();
    m_recording = false;
    Frame& frame = m_frame_slots[m_frame_index % k_frames];
    ++m_frame_index;
    if (check_gpu(m_list->Close(), "Close")) {
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
    }
    decay_buffer_states();
    if (m_swap_chain != nullptr) {
        const UINT interval = m_swap_chain->vsync ? 1 : 0;
        const UINT flags = m_swap_chain->allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        check_gpu(m_swap_chain->swap_chain->Present(interval, flags), "Present");
    }
    frame.fence = signal();
    for (Retired& r : m_retired_this_frame) {
        // A failed signal (0, device lost) keeps them until destruction.
        r.fence = frame.fence != 0 ? frame.fence : ~u64{0};
        m_retired.push_back(std::move(r));
    }
    m_retired_this_frame.clear();
    m_swap_chain = nullptr;
    m_last_stats = m_stats;
    ++m_frames;
}

std::vector<u8> D3D12Device::read_texture(TextureHandle handle) {
    GIANT_ASSERT(!m_recording, "read_texture inside a frame");
    DxTexture* t = m_textures.get(handle);
    GIANT_ASSERT(t != nullptr, "read_texture: dead texture");
    if (t == nullptr || m_lost) {
        return {};
    }
    wait_idle(); // the texture's last writes must be done
    const D3D12_RESOURCE_DESC rd = t->resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    m_device->GetCopyableFootprints(&rd, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> readback = create_committed_buffer(
        D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
        "texture readback");
    if (readback == nullptr) {
        return {};
    }
    ID3D12GraphicsCommandList* list = begin_immediate();
    const D3D12_RESOURCE_STATES before = to_d3d(t->state);
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        const auto b = transition(t->resource.Get(), before, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = t->resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        const auto b = transition(t->resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, before);
        list->ResourceBarrier(1, &b);
    }
    if (!submit_immediate()) {
        return {};
    }
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total)};
    if (!check(readback->Map(0, &read_range, &mapped), "Map(readback)")) {
        return {};
    }
    const usize row = static_cast<usize>(t->desc.width) * bytes_per_pixel(t->desc.format);
    std::vector<u8> pixels(row * t->desc.height);
    for (u32 y = 0; y < t->desc.height; ++y) {
        std::memcpy(pixels.data() + static_cast<usize>(y) * row,
                    static_cast<const u8*>(mapped) + footprint.Offset +
                        static_cast<usize>(y) * footprint.Footprint.RowPitch,
                    row);
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return pixels;
}

std::vector<u8> D3D12Device::read_buffer(BufferHandle handle) {
    GIANT_ASSERT(!m_recording, "read_buffer inside a frame");
    DxBuffer* b = m_buffers.get(handle);
    GIANT_ASSERT(b != nullptr, "read_buffer: dead buffer");
    if (b == nullptr || m_lost) {
        return {};
    }
    wait_idle();
    if (b->mapped != nullptr) {
        return {b->mapped, b->mapped + b->desc.size};
    }
    ComPtr<ID3D12Resource> readback = create_committed_buffer(
        D3D12_HEAP_TYPE_READBACK, b->desc.size, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "buffer readback");
    if (readback == nullptr) {
        return {};
    }
    ID3D12GraphicsCommandList* list = begin_immediate();
    const D3D12_RESOURCE_STATES before = to_d3d(b->state);
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE && before != D3D12_RESOURCE_STATE_COMMON) {
        const auto t = transition(b->resource.Get(), before, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &t);
    }
    list->CopyBufferRegion(readback.Get(), 0, b->resource.Get(), 0, b->desc.size);
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE && before != D3D12_RESOURCE_STATE_COMMON) {
        const auto t = transition(b->resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, before);
        list->ResourceBarrier(1, &t);
    }
    if (!submit_immediate()) {
        return {};
    }
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(b->desc.size)};
    if (!check(readback->Map(0, &read_range, &mapped), "Map(readback)")) {
        return {};
    }
    std::vector<u8> bytes(static_cast<const u8*>(mapped),
                          static_cast<const u8*>(mapped) + b->desc.size);
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return bytes;
}

// --- Command list --------------------------------------------------------------------

void D3D12CommandList::begin(ID3D12GraphicsCommandList* list, Frame* frame) {
    m_list = list;
    m_frame = frame;
    m_in_pass = false;
    m_pipeline = {};
    m_push_dwords = 0;
}

void D3D12CommandList::end() {
    GIANT_ASSERT(!m_in_pass, "end_frame inside a render pass");
    m_list = nullptr;
    m_frame = nullptr;
}

void D3D12CommandList::barrier(BufferHandle handle, ResourceState state) {
    GIANT_ASSERT(!m_in_pass, "barriers belong outside render passes");
    DxBuffer* b = m_device.buffer(handle);
    GIANT_ASSERT(b != nullptr, "barrier on a dead buffer");
    if (b == nullptr || b->desc.memory != Memory::gpu) {
        GIANT_ASSERT(b == nullptr, "upload and readback buffers keep their state");
        return;
    }
    if (b->state == state) {
        if (state == ResourceState::shader_write) {
            const auto uav = uav_barrier(b->resource.Get());
            m_list->ResourceBarrier(1, &uav);
            ++m_device.stats().barriers;
        }
        return;
    }
    const auto t = transition(b->resource.Get(), to_d3d(b->state), to_d3d(state));
    m_list->ResourceBarrier(1, &t);
    if (b->state == ResourceState::common) {
        m_device.track_decay(handle);
    }
    b->state = state;
    ++m_device.stats().barriers;
}

void D3D12CommandList::barrier(TextureHandle handle, ResourceState state) {
    GIANT_ASSERT(!m_in_pass, "barriers belong outside render passes");
    DxTexture* t = m_device.texture(handle);
    GIANT_ASSERT(t != nullptr, "barrier on a dead texture");
    if (t == nullptr) {
        return;
    }
    if (t->state == state) {
        if (state == ResourceState::shader_write) {
            const auto uav = uav_barrier(t->resource.Get());
            m_list->ResourceBarrier(1, &uav);
            ++m_device.stats().barriers;
        }
        return;
    }
    const auto b = transition(t->resource.Get(), to_d3d(t->state), to_d3d(state));
    m_list->ResourceBarrier(1, &b);
    t->state = state;
    ++m_device.stats().barriers;
}

void D3D12CommandList::begin_render_pass(const RenderPassDesc& desc) {
    GIANT_ASSERT(!m_in_pass, "render passes do not nest");
    GIANT_ASSERT(desc.color_count <= k_max_color_targets, "too many colour targets");
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, k_max_color_targets> rtvs{};
    m_pass_width = 0;
    m_pass_height = 0;
    for (u32 i = 0; i < desc.color_count; ++i) {
        DxTexture* t = m_device.texture(desc.colors[i].texture);
        GIANT_ASSERT(t != nullptr && t->rtv != k_no_descriptor,
                     "render pass needs live render targets");
        if (t == nullptr) {
            return;
        }
        barrier(desc.colors[i].texture, ResourceState::render_target);
        rtvs[i] = m_device.rtv(t->rtv);
        if (m_pass_width == 0) {
            m_pass_width = t->desc.width;
            m_pass_height = t->desc.height;
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    const bool has_depth = desc.depth.texture.valid();
    if (has_depth) {
        DxTexture* t = m_device.texture(desc.depth.texture);
        GIANT_ASSERT(t != nullptr && t->dsv != k_no_descriptor, "render pass needs a live depth target");
        if (t == nullptr) {
            return;
        }
        barrier(desc.depth.texture, ResourceState::depth_write);
        dsv = m_device.dsv(t->dsv);
        if (m_pass_width == 0) {
            m_pass_width = t->desc.width;
            m_pass_height = t->desc.height;
        }
    }
    m_list->OMSetRenderTargets(desc.color_count, rtvs.data(), FALSE, has_depth ? &dsv : nullptr);
    for (u32 i = 0; i < desc.color_count; ++i) {
        if (desc.colors[i].load == LoadOp::clear) {
            const Color& c = desc.colors[i].clear;
            const float rgba[4] = {c.r, c.g, c.b, c.a};
            m_list->ClearRenderTargetView(rtvs[i], rgba, 0, nullptr);
        }
    }
    if (has_depth && desc.depth.load == LoadOp::clear) {
        m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, desc.depth.clear_depth, 0, 0,
                                      nullptr);
    }
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_pass_width),
                                  static_cast<float>(m_pass_height), 0.0f, 1.0f};
    m_list->RSSetViewports(1, &viewport);
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_pass_width), static_cast<LONG>(m_pass_height)};
    m_list->RSSetScissorRects(1, &scissor);
    m_in_pass = true;
}

void D3D12CommandList::end_render_pass() {
    GIANT_ASSERT(m_in_pass, "end_render_pass without begin_render_pass");
    m_in_pass = false;
}

void D3D12CommandList::set_pipeline(PipelineHandle handle) {
    DxPipeline* p = m_device.pipeline(handle);
    GIANT_ASSERT(p != nullptr, "set_pipeline: dead pipeline");
    if (p == nullptr) {
        return;
    }
    m_list->SetPipelineState(p->state.Get());
    m_pipeline = handle;
    m_pipeline_compute = p->compute;
}

void D3D12CommandList::set_push_constants(const void* data, u32 bytes) {
    GIANT_ASSERT(bytes <= k_max_push_constant_bytes && bytes % 4 == 0,
                 "push constants: at most 128 bytes, a multiple of 4");
    std::memcpy(m_push.data(), data, bytes);
    m_push_dwords = bytes / 4;
}

void D3D12CommandList::flush_push(bool compute) {
    if (m_push_dwords == 0) {
        return;
    }
    if (compute) {
        m_list->SetComputeRoot32BitConstants(0, m_push_dwords, m_push.data(), 0);
    } else {
        m_list->SetGraphicsRoot32BitConstants(0, m_push_dwords, m_push.data(), 0);
    }
}

void D3D12CommandList::set_scissor(u32 x, u32 y, u32 width, u32 height) {
    GIANT_ASSERT(m_in_pass, "set_scissor outside a render pass");
    const D3D12_RECT rect{static_cast<LONG>(x), static_cast<LONG>(y),
                          static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
    m_list->RSSetScissorRects(1, &rect);
}

void D3D12CommandList::draw(u32 vertex_count, u32 instance_count, u32 first_vertex) {
    GIANT_ASSERT(m_in_pass && m_pipeline.valid() && !m_pipeline_compute,
                 "draw needs a render pass and a graphics pipeline");
    flush_push(false);
    m_list->DrawInstanced(vertex_count, instance_count, first_vertex, 0);
    ++m_device.stats().draws;
}

void D3D12CommandList::set_index_buffer(BufferHandle handle, u64 offset) {
    DxBuffer* b = m_device.buffer(handle);
    GIANT_ASSERT(b != nullptr && has(b->desc.usage, BufferUsage::index),
                 "set_index_buffer needs a live buffer with BufferUsage::index");
    if (b == nullptr) {
        return;
    }
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = b->resource->GetGPUVirtualAddress() + offset;
    view.SizeInBytes = static_cast<UINT>(b->desc.size - offset);
    view.Format = DXGI_FORMAT_R32_UINT;
    m_list->IASetIndexBuffer(&view);
}

void D3D12CommandList::draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                                    i32 vertex_offset) {
    GIANT_ASSERT(m_in_pass && m_pipeline.valid() && !m_pipeline_compute,
                 "draw needs a render pass and a graphics pipeline");
    flush_push(false);
    m_list->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, 0);
    ++m_device.stats().draws;
}

void D3D12CommandList::dispatch(u32 x, u32 y, u32 z) {
    GIANT_ASSERT(!m_in_pass && m_pipeline.valid() && m_pipeline_compute,
                 "dispatch needs a compute pipeline, outside render passes");
    flush_push(true);
    m_list->Dispatch(x, y, z);
    ++m_device.stats().dispatches;
}

void D3D12CommandList::copy_buffer(BufferHandle dst, u64 dst_offset, BufferHandle src,
                                   u64 src_offset, u64 bytes) {
    GIANT_ASSERT(!m_in_pass, "copy inside a render pass");
    DxBuffer* d = m_device.buffer(dst);
    DxBuffer* s = m_device.buffer(src);
    GIANT_ASSERT(d != nullptr && s != nullptr, "copy_buffer: dead buffer");
    if (d == nullptr || s == nullptr) {
        return;
    }
    GIANT_ASSERT(dst_offset + bytes <= d->desc.size && src_offset + bytes <= s->desc.size,
                 "copy_buffer out of bounds");
    m_list->CopyBufferRegion(d->resource.Get(), dst_offset, s->resource.Get(), src_offset, bytes);
}

TransientMemory D3D12CommandList::allocate(u32 bytes) {
    GIANT_ASSERT(bytes > 0 && bytes <= k_max_transient_allocation,
                 "transient allocations are 1 byte to 256 MiB");
    if (bytes == 0 || bytes > k_max_transient_allocation) {
        return {};
    }
    const u32 aligned = (bytes + (k_transient_alignment - 1)) & ~(k_transient_alignment - 1);
    TransientChunk* chunk = nullptr;
    for (TransientChunk& c : m_frame->chunks) {
        if (u64{c.used} + aligned <= c.size) {
            chunk = &c;
            break;
        }
    }
    if (chunk == nullptr) {
        const u32 size = std::max(k_transient_chunk_bytes, aligned);
        const BufferHandle buffer = m_device.create_transient_chunk(size);
        if (!buffer.valid()) {
            return {};
        }
        m_frame->chunks.push_back({buffer, size, 0});
        chunk = &m_frame->chunks.back();
    }
    DxBuffer* b = m_device.buffer(chunk->buffer);
    TransientMemory memory;
    memory.buffer = chunk->buffer;
    memory.descriptor = b->read;
    memory.offset = chunk->used;
    memory.data = b->mapped + chunk->used;
    memory.size = bytes;
    chunk->used += aligned;
    m_device.stats().transient_bytes += aligned;
    return memory;
}

void D3D12CommandList::draw_ui(TextureHandle target, const UiFrame& ui) {
    GIANT_ASSERT(!m_in_pass, "draw_ui opens its own pass");
    DxTexture* t = m_device.texture(target);
    GIANT_ASSERT(t != nullptr && t->ui_rtv != k_no_descriptor &&
                     (t->desc.format == Format::rgba8_unorm || t->desc.format == Format::rgba8_srgb),
                 "draw_ui needs a live rgba8 render target");
    if (t == nullptr) {
        return;
    }
    u64 vertex_count = 0;
    u64 index_count = 0;
    for (const UiDrawList& list : ui.lists) {
        vertex_count += list.vertices.size();
        index_count += list.indices.size();
    }
    if (vertex_count == 0 || index_count == 0) {
        return;
    }
    const TransientMemory vertices = allocate(static_cast<u32>(vertex_count * sizeof(UiVertex)));
    const TransientMemory indices = allocate(static_cast<u32>(index_count * sizeof(u32)));
    if (vertices.data == nullptr || indices.data == nullptr) {
        return;
    }
    u64 v_at = 0;
    u64 i_at = 0;
    for (const UiDrawList& list : ui.lists) {
        if (!list.vertices.empty()) {
            std::memcpy(vertices.data + v_at * sizeof(UiVertex), list.vertices.data(),
                        list.vertices.size_bytes());
        }
        if (!list.indices.empty()) {
            std::memcpy(indices.data + i_at * sizeof(u32), list.indices.data(),
                        list.indices.size_bytes());
        }
        v_at += list.vertices.size();
        i_at += list.indices.size();
    }

    // Sampled textures must be readable before the pass (barriers belong
    // outside it); a texture drawn into earlier this frame is not yet.
    for (const UiDrawList& list : ui.lists) {
        for (const UiDrawCommand& cmd : list.commands) {
            const TextureHandle texture = texture_from_ui_id(cmd.texture);
            const DxTexture* tex = m_device.texture(texture);
            GIANT_ASSERT(tex != nullptr && tex->read != k_no_descriptor &&
                             tex->desc.format == Format::rgba8_unorm && !(texture == target),
                         "UI command needs a live, sampled rgba8_unorm texture (not the target)");
            if (tex != nullptr && tex->read != k_no_descriptor && !(texture == target) &&
                tex->state != ResourceState::shader_read) {
                barrier(texture, ResourceState::shader_read);
            }
        }
    }
    barrier(target, ResourceState::render_target);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_device.rtv(t->ui_rtv);
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float w = static_cast<float>(t->desc.width);
    const float h = static_cast<float>(t->desc.height);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, w, h, 0.0f, 1.0f};
    m_list->RSSetViewports(1, &viewport);
    set_pipeline(m_device.ui_pipeline());

    struct UiPush {
        float scale[2];
        float offset[2];
        u32 vertices;
        u32 vertex_base;
        u32 indices;
        u32 index_base;
        u32 texture;
        u32 pad[3];
    };
    static_assert(sizeof(UiVertex) == 20, "ui.slang reads 20-byte vertices");

    u32 draws = 0;
    u64 list_vertex_base = 0;
    u64 list_index_base = 0;
    for (const UiDrawList& list : ui.lists) {
        for (const UiDrawCommand& cmd : list.commands) {
            if (static_cast<u64>(cmd.first_index) + cmd.index_count > list.indices.size()) {
                GIANT_ASSERT(false, "UI command reads past its indices");
                continue;
            }
            const u32 texture = m_device.read_descriptor(texture_from_ui_id(cmd.texture));
            if (texture == k_no_descriptor) {
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
            UiPush push{};
            push.scale[0] = 2.0f / w;
            push.scale[1] = -2.0f / h;
            push.offset[0] = -1.0f;
            push.offset[1] = 1.0f;
            push.vertices = vertices.descriptor;
            push.vertex_base = static_cast<u32>(
                vertices.offset + (list_vertex_base + cmd.vertex_offset) * sizeof(UiVertex));
            push.indices = indices.descriptor;
            push.index_base =
                static_cast<u32>(indices.offset + (list_index_base + cmd.first_index) * sizeof(u32));
            push.texture = texture;
            set_push_constants(&push, sizeof(push));
            flush_push(false);
            m_list->DrawInstanced(cmd.index_count, 1, 0, 0);
            ++draws;
        }
        list_vertex_base += list.vertices.size();
        list_index_base += list.indices.size();
    }
    m_device.stats().ui_draws += draws;
    m_device.stats().draws += draws;
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
