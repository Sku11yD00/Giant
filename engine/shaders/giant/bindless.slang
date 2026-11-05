// Bindless access for every engine shader (see engine/rhi/include/giant/rhi/rhi.h).
//
// Resources are reached by descriptor index, through shader model 6.6's
// descriptor heaps; draws pass the indices in push constants, declared as
//     ConstantBuffer<MyPush> push : register(b0);
// Keep heap access in these helpers, so the mapping to another backend is
// one file.
#pragma once

ByteAddressBuffer read_buffer(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
RWByteAddressBuffer write_buffer(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
Texture2D<float4> read_texture(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
RWTexture2D<float4> write_texture(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }

// Fixed samplers, matching rhi::Sampler.
static const uint k_sampler_linear_clamp = 0;
static const uint k_sampler_linear_wrap = 1;
static const uint k_sampler_point_clamp = 2;
static const uint k_sampler_anisotropic_wrap = 3;

SamplerState get_sampler(uint index) { return SamplerDescriptorHeap[index]; }

// A column-major 4x4 matrix as four float4 columns (as rhi stores Mat4).
float4 transform(float4 c0, float4 c1, float4 c2, float4 c3, float4 v) {
    return c0 * v.x + c1 * v.y + c2 * v.z + c3 * v.w;
}

float4 unpack_rgba8(uint c) {
    return float4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu),
                  float(c >> 24)) / 255.0;
}
