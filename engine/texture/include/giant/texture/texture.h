// Material textures, cooked offline: images decoded, filtered into mips and
// block compressed, then saved as .gtex files the engine loads straight into
// rhi textures.
//
//   texture::Image albedo;
//   std::string error;
//   texture::decode_image(png_bytes, albedo, error);
//   texture::Cooked cooked;
//   texture::cook(albedo, {.kind = texture::Kind::color}, cooked, error);
//   save(texture::write_file(cooked));
//   ...
//   texture::read_file(file_bytes, cooked, error);
//   device.create_texture(cooked.desc, cooked.contents);
//
// Each kind of texture is filtered the way its data means:
//   - colour: in linear light, weighted by alpha so cut-out edges do not
//     darken; BC7 sRGB (or BC1 sRGB, half the size, without alpha).
//   - normal: the average of the vectors, renormalised; BC5 holds x and y,
//     shaders rebuild z.
//   - orm (glTF occlusion R, roughness G, metallic B): roughness filtered as
//     GGX alpha squared, then widened by the normal detail that smaller mips
//     average away (Toksvig), so distant bumpy surfaces do not sparkle; BC7.
//   - mask: one linear channel (R); BC4.
#pragma once

#include "giant/core/types.h"
#include "giant/rhi/rhi.h"

#include <span>
#include <string>
#include <vector>

namespace giant {
class JobSystem;
}

namespace giant::texture {

enum class Kind : u8 {
    color,
    normal,
    orm,
    mask,
};

// RGBA8, rows top to bottom.
struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba; // width * height * 4 bytes
};

// Decodes a PNG, JPEG, TGA or BMP file (any channel count, 8 or 16 bits)
// into RGBA8. Images over 16384 texels a side are refused. False with a
// reason in `error` if the file cannot be read.
bool decode_image(std::span<const u8> file, Image& out, std::string& error);

struct CookOptions {
    Kind kind = Kind::color;
    // Colour only: BC1 instead of BC7, half the memory; alpha is dropped.
    bool small = false;
    // Normal maps authored for DirectX (green pointing down the image) are
    // flipped to the engine's green-up convention.
    bool flip_green = false;
    // Orm only: the material's normal map, whose detail lost in smaller mips
    // widens the roughness there. Any size; null for none.
    const Image* normal = nullptr;
    // The largest side kept; bigger images are filtered down to fit.
    u32 max_size = 16384;
    // Spreads block encoding over these workers; null encodes on the caller.
    JobSystem* jobs = nullptr;
};

// A texture ready for rhi::Device::create_texture: its description and its
// contents (every mip, largest first, as rhi lays them out).
struct Cooked {
    Kind kind = Kind::color;
    rhi::TextureDesc desc;
    std::vector<u8> contents;
};

// The mips a cook encodes, as RGBA8 before compression (largest first; the
// top one resized to a multiple of 4 that fits `max_size`). Colour stays
// sRGB-encoded; normals are stored x, y, z in [0, 255] for [-1, 1]; orm and
// mask are linear. Empty with a reason if the image or options are bad.
std::vector<Image> make_mips(const Image& image, const CookOptions& options, std::string& error);

// Cooks one image into a block-compressed texture with a full mip chain.
bool cook(const Image& image, const CookOptions& options, Cooked& out, std::string& error);

// Decodes a block-compressed level (`width` x `height` texels, blocks as
// rhi lays them out) back to RGBA8, for checks and tools. BC4 fills R, G and
// B; BC5 fills R and G (B 0). Empty for other formats or a size mismatch.
std::vector<u8> decode_blocks(rhi::Format format, u32 width, u32 height, std::span<const u8> blocks);

// Where a level starts in a texture's contents, and its size.
u64 level_offset(const rhi::TextureDesc& desc, u32 layer, u32 mip);

// The .gtex file: a 64-byte header (magic "GTEX", version, kind, format,
// size, mips, layers, contents length and hash), then the contents as rhi
// lays them out, so a level can be read from its offset.
std::vector<u8> write_file(const Cooked& texture);
// False with a reason if the file is not a .gtex this engine reads, is cut
// short, fails its hash, or describes a texture rhi would refuse.
bool read_file(std::span<const u8> file, Cooked& out, std::string& error);

inline constexpr u32 k_file_header_bytes = 64;
inline constexpr u32 k_file_version = 1;

} // namespace giant::texture
