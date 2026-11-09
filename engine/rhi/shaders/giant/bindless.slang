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
// For a descriptor that differs between threads of a wave (e.g. each pixel
// reading the mesh it shows): the GPU must not assume it is uniform.
ByteAddressBuffer read_buffer_nonuniform(uint descriptor) {
    return ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
}
Texture2D<float4> read_texture(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
// A texture with more than one layer (TextureDesc::layers > 1).
Texture2DArray<float4> read_texture_array(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
RWTexture2D<float4> write_texture(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
// Integer formats (r32_uint, rg32_uint: e.g. a visibility buffer) need
// integer views; reading them through the float4 helpers is invalid.
Texture2D<uint> read_texture_uint(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
Texture2D<uint2> read_texture_uint2(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
RWTexture2D<uint> write_texture_uint(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
RWTexture2D<uint2> write_texture_uint2(uint descriptor) { return ResourceDescriptorHeap[descriptor]; }
Texture2D<float> read_texture_float(uint descriptor) { return ResourceDescriptorHeap[descriptor]; } // depth, r32_float
RWTexture2D<float> write_texture_float(uint descriptor) { return ResourceDescriptorHeap[descriptor]; } // r32_float

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
