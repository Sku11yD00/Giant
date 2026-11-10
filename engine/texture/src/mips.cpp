// Mip chains for each kind of texture, filtered in float.
#include "giant/texture/texture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>

namespace giant::texture {

namespace {

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

u8 to_u8(float f) { return static_cast<u8>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); }
float to_unit(u8 b) { return static_cast<float>(b) / 255.0f; }

// A float image of `channels` values a texel.
struct Plane {
    u32 width = 0;
    u32 height = 0;
    u32 channels = 0;
    std::vector<float> v;

    Plane() = default;
    Plane(u32 w, u32 h, u32 c) : width(w), height(h), channels(c), v(usize{w} * h * c, 0.0f) {}
    float* at(u32 x, u32 y) { return v.data() + (usize{y} * width + x) * channels; }
    const float* at(u32 x, u32 y) const { return v.data() + (usize{y} * width + x) * channels; }
};

struct Tap {
    u32 index;
    float weight;
};

// For each of `dst` texels along an axis, the `src` texels it reads and
// their weights (summing to 1). Shrinking averages each texel's footprint
// (a 2x2 box when halving); growing interpolates linearly.
std::vector<std::vector<Tap>> axis_taps(u32 src, u32 dst) {
    std::vector<std::vector<Tap>> taps(dst);
    const double scale = static_cast<double>(src) / static_cast<double>(dst);
    for (u32 i = 0; i < dst; ++i) {
        std::vector<Tap>& t = taps[i];
        if (scale >= 1.0) {
            const double lo = i * scale;
            const double hi = (i + 1) * scale;
            const u32 first = static_cast<u32>(std::floor(lo));
            const u32 end = std::min(src, static_cast<u32>(std::ceil(hi)));
            for (u32 s = first; s < end; ++s) {
                const double w = std::min(hi, s + 1.0) - std::max(lo, static_cast<double>(s));
                if (w > 1e-9) t.push_back({s, static_cast<float>(w)});
            }
        } else {
            const double c = (i + 0.5) * scale - 0.5;
            const double base = std::floor(c);
            const float f = static_cast<float>(c - base);
            const auto clamped = [src](double s) {
                return static_cast<u32>(std::clamp(s, 0.0, static_cast<double>(src - 1)));
            };
            t.push_back({clamped(base), 1.0f - f});
            t.push_back({clamped(base + 1.0), f});
        }
        float sum = 0.0f;
        for (const Tap& tap : t) sum += tap.weight;
        if (t.empty() || sum <= 0.0f) {
            t.assign(1, Tap{std::min(i, src - 1), 1.0f});
            continue;
        }
        for (Tap& tap : t) tap.weight /= sum;
    }
    return taps;
}

Plane resample(Plane in, u32 width, u32 height) {
    if (in.width == width && in.height == height) return in;
    const auto tx = axis_taps(in.width, width);
    const auto ty = axis_taps(in.height, height);
    Plane across(width, in.height, in.channels);
    for (u32 y = 0; y < in.height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            float* out = across.at(x, y);
            for (const Tap& t : tx[x]) {
                const float* src = in.at(t.index, y);
                for (u32 c = 0; c < in.channels; ++c) out[c] += src[c] * t.weight;
            }
        }
    }
    Plane out(width, height, in.channels);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            float* o = out.at(x, y);
            for (const Tap& t : ty[y]) {
                const float* src = across.at(x, t.index);
                for (u32 c = 0; c < in.channels; ++c) o[c] += src[c] * t.weight;
            }
        }
    }
    return out;
}

// The chain from `top` down to 1x1, each level half the last (rounded
// down, at least 1).
std::vector<Plane> chain(Plane top) {
    std::vector<Plane> levels;
    levels.push_back(std::move(top));
    while (levels.back().width > 1 || levels.back().height > 1) {
        const Plane& last = levels.back();
        levels.push_back(resample(last, std::max(1u, last.width / 2), std::max(1u, last.height / 2)));
    }
    return levels;
}

// A unit normal from a texel, green up (flipped from green down if asked).
// Only x and y are trusted: z is rebuilt, as BC5 does.
void unit_normal(const u8* texel, bool flip_green, float n[3]) {
    const float x = to_unit(texel[0]) * 2.0f - 1.0f;
    const float y = (to_unit(texel[1]) * 2.0f - 1.0f) * (flip_green ? -1.0f : 1.0f);
    const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
    const float len = std::sqrt(x * x + y * y + z * z);
    n[0] = x / len;
    n[1] = y / len;
    n[2] = z / len;
}

// Unit normals as 3 channels; their averages in smaller mips shorten.
Plane normal_plane(const Image& image, bool flip_green) {
    Plane p(image.width, image.height, 3);
    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            unit_normal(&image.rgba[(usize{y} * image.width + x) * 4], flip_green, p.at(x, y));
        }
    }
    return p;
}

// Toksvig: how spread the normals averaged into a texel are, from how
// short their average is.
float normal_variance(const float* mean) {
    const float len = std::sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
    const float l = std::clamp(len, 1e-4f, 1.0f);
    return (1.0f - l) / l;
}

u32 round_up4(u32 v) { return (v + 3u) & ~3u; }

bool image_problem(const Image& image, const char* what, std::string& error) {
    if (image.width == 0 || image.height == 0 || image.width > 16384 || image.height > 16384) {
        error = std::string(what) + " needs a size of 1 to 16384 texels a side";
        return true;
    }
    if (image.rgba.size() != usize{image.width} * image.height * 4) {
        error = std::string(what) + "'s pixels do not match its size";
        return true;
    }
    return false;
}

} // namespace

std::vector<Image> make_mips(const Image& image, const CookOptions& options, std::string& error) {
    if (image_problem(image, "the image", error)) return {};
    if (options.kind == Kind::roughness_metal && options.normal != nullptr &&
        image_problem(*options.normal, "the normal map", error)) {
        return {};
    }
    if (options.kind != Kind::color && options.kind != Kind::normal && options.kind != Kind::roughness_metal &&
        options.kind != Kind::mask) {
        error = "unknown texture kind";
        return {};
    }
    if (options.mask_channel > 3) {
        error = "a mask comes from channel 0 to 3";
        return {};
    }

    // The top level: within max_size (a multiple of 4, at least 4), its
    // sides rounded up to multiples of 4 as block compression needs.
    const u32 limit = std::max(4u, std::min(options.max_size, 16384u) & ~3u);
    const double fit = std::min(1.0, static_cast<double>(limit) / std::max(image.width, image.height));
    const auto side = [&](u32 s) {
        const u32 scaled = static_cast<u32>(std::max(1l, std::lround(s * fit)));
        return std::min(limit, round_up4(scaled));
    };
    const u32 top_w = side(image.width);
    const u32 top_h = side(image.height);

    // Linear-space data to filter, per kind.
    Plane source;
    switch (options.kind) {
    case Kind::color:
        // Alpha-weighted colour, alpha, and plain colour for texels that
        // end up fully transparent.
        source = Plane(image.width, image.height, 7);
        for (u32 y = 0; y < image.height; ++y) {
            for (u32 x = 0; x < image.width; ++x) {
                const u8* t = &image.rgba[(usize{y} * image.width + x) * 4];
                float* p = source.at(x, y);
                const float a = to_unit(t[3]);
                for (u32 c = 0; c < 3; ++c) {
                    const float lin = srgb_to_linear(to_unit(t[c]));
                    p[c] = lin * a;
                    p[4 + c] = lin;
                }
                p[3] = a;
            }
        }
        break;
    case Kind::normal:
        source = normal_plane(image, options.flip_green);
        break;
    case Kind::roughness_metal:
        // From a glTF metallic-roughness image (G roughness, B metallic):
        // GGX alpha squared (roughness^4, what adds up when surfaces are
        // averaged) and metallic.
        source = Plane(image.width, image.height, 2);
        for (u32 y = 0; y < image.height; ++y) {
            for (u32 x = 0; x < image.width; ++x) {
                const u8* t = &image.rgba[(usize{y} * image.width + x) * 4];
                float* p = source.at(x, y);
                const float r = to_unit(t[1]);
                p[0] = r * r * r * r;
                p[1] = to_unit(t[2]);
            }
        }
        break;
    case Kind::mask:
        source = Plane(image.width, image.height, 1);
        for (usize i = 0; i < source.v.size(); ++i) source.v[i] = to_unit(image.rgba[i * 4 + options.mask_channel]);
        break;
    }

    // The normal map's own chain, to read how much detail each level has
    // lost (only these float levels are kept; the texture's own are turned
    // into bytes one at a time).
    std::vector<Plane> normals;
    if (options.kind == Kind::roughness_metal && options.normal != nullptr) {
        normals = chain(normal_plane(*options.normal, options.flip_green));
    }
    // The normal level whose texels are about the size of a level's.
    const auto normal_level = [&normals](u32 width, u32 height) -> const Plane* {
        if (normals.empty()) return nullptr;
        const auto steps = [](u32 normal_side, u32 level_side) {
            return static_cast<int>(std::lround(std::log2(static_cast<double>(normal_side) / level_side)));
        };
        const int k = std::clamp(std::max(steps(normals[0].width, width), steps(normals[0].height, height)), 0,
                                 static_cast<int>(normals.size()) - 1);
        return &normals[static_cast<usize>(k)];
    };

    // Colour: the share of a level's texels whose alpha reaches the cutoff.
    const float cutoff = options.coverage_cutoff;
    const auto coverage = [cutoff](const Plane& level) {
        usize kept = 0;
        const usize texels = usize{level.width} * level.height;
        for (usize i = 0; i < texels; ++i) kept += level.v[i * level.channels + 3] >= cutoff ? 1u : 0u;
        return static_cast<double>(kept) / static_cast<double>(std::max<usize>(texels, 1));
    };
    // The alpha scale that gives `level` the coverage `wanted`, in one pass:
    // the alpha that many texels reach, lifted to the cutoff (at most 4x: an
    // alpha needing more is noise). Of the two counts of texels around the
    // wanted share, the nearer.
    std::vector<float> alphas;
    const auto coverage_scale = [&](const Plane& level, double wanted) {
        const usize texels = usize{level.width} * level.height;
        alphas.resize(texels);
        for (usize i = 0; i < texels; ++i) alphas[i] = level.v[i * level.channels + 3];
        const double exact = wanted * static_cast<double>(texels);
        const auto below = static_cast<usize>(std::floor(exact));
        const usize kept = std::clamp<usize>(exact - static_cast<double>(below) >= 0.5 ? below + 1 : below, 1, texels);
        // The kept-th largest alpha.
        std::nth_element(alphas.begin(), alphas.begin() + static_cast<std::ptrdiff_t>(kept - 1), alphas.end(),
                         std::greater<float>());
        const float reached = alphas[kept - 1];
        // Aimed at the alpha that rounds to the first byte at or over the
        // cutoff, so the stored level keeps it.
        const float byte = std::ceil(cutoff * 255.0f - 1e-3f);
        const float target = (byte - 0.49f) / 255.0f;
        return reached > target / 4.0f ? target / reached : 4.0f;
    };

    float alpha_scale = 1.0f;
    const auto to_image = [&](const Plane& level) {
        Image img;
        img.width = level.width;
        img.height = level.height;
        img.rgba.resize(usize{level.width} * level.height * 4);
        const Plane* n = options.kind == Kind::roughness_metal ? normal_level(level.width, level.height) : nullptr;
        for (u32 y = 0; y < level.height; ++y) {
            for (u32 x = 0; x < level.width; ++x) {
                const float* p = level.at(x, y);
                u8* t = &img.rgba[(usize{y} * level.width + x) * 4];
                switch (options.kind) {
                case Kind::color: {
                    const float a = p[3];
                    for (u32 c = 0; c < 3; ++c) {
                        // Below about half an 8-bit step of alpha the
                        // weighted colour is noise; use the plain one.
                        const float lin = a > 1.0f / 512.0f ? p[c] / a : p[4 + c];
                        t[c] = to_u8(linear_to_srgb(lin));
                    }
                    t[3] = to_u8(a * alpha_scale);
                    break;
                }
                case Kind::normal: {
                    const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                    const float u[3] = {len > 1e-6f ? p[0] / len : 0.0f, len > 1e-6f ? p[1] / len : 0.0f,
                                        len > 1e-6f ? p[2] / len : 1.0f};
                    for (u32 c = 0; c < 3; ++c) t[c] = to_u8(u[c] * 0.5f + 0.5f);
                    t[3] = 255;
                    break;
                }
                case Kind::roughness_metal: {
                    float variance = 0.0f;
                    if (n != nullptr) {
                        const u32 nx = std::min(n->width - 1, static_cast<u32>((x + 0.5) / level.width * n->width));
                        const u32 ny =
                            std::min(n->height - 1, static_cast<u32>((y + 0.5) / level.height * n->height));
                        variance = normal_variance(n->at(nx, ny));
                    }
                    const float alpha2 = std::min(1.0f, p[0] + 2.0f * variance);
                    t[0] = to_u8(std::sqrt(std::sqrt(alpha2)));
                    t[1] = to_u8(p[1]);
                    t[2] = 0;
                    t[3] = 255;
                    break;
                }
                case Kind::mask:
                    t[0] = t[1] = t[2] = to_u8(p[0]);
                    t[3] = 255;
                    break;
                }
            }
        }
        return img;
    };

    // Each float level lives only until the next is made from it.
    std::vector<Image> out;
    Plane level = resample(std::move(source), top_w, top_h);
    out.push_back(to_image(level));
    // A cut-out (some texels each side of the cutoff) keeps its coverage.
    const bool cut_out = options.kind == Kind::color && !options.small && cutoff > 0.0f && cutoff < 1.0f;
    const double top_coverage = cut_out ? coverage(level) : 0.0;
    const bool keep_coverage = cut_out && top_coverage > 0.0 && top_coverage < 1.0;
    while (level.width > 1 || level.height > 1) {
        const u32 w = std::max(1u, level.width / 2);
        const u32 h = std::max(1u, level.height / 2);
        level = resample(std::move(level), w, h);
        alpha_scale = keep_coverage ? coverage_scale(level, top_coverage) : 1.0f;
        out.push_back(to_image(level));
    }
    return out;
}

} // namespace giant::texture
