// Fonts: TrueType outlines to signed distance glyphs (stb_truetype), packed
// into one atlas (stb_rect_pack). Both are compiled here, static, apart
// from the copies ImGui compiles for the editor.
#include "giant/ui/font.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(_MSC_VER)
// stb's functions are static here, most unused: MSVC says so at the end of the file (as ImGui's imgui_draw.cpp).
#pragma warning(disable : 4505)
#pragma warning(push, 0)
#endif
#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "imstb_rectpack.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace giant::ui {

namespace embedded {
std::span<const std::uint8_t> default_font();
} // namespace embedded

namespace {

constexpr char32_t k_low = 0x180; // Basic Latin, Latin-1 and Latin Extended-A by index
constexpr u32 k_max_height = 4096;
constexpr int k_gap = 1; // pixels between glyphs (no bleeding when sampled)
constexpr int k_solid = 4;

// What the atlas holds.
std::vector<char32_t> charset() {
    std::vector<char32_t> out;
    for (char32_t c = 0x20; c < 0x7F; ++c) out.push_back(c);
    for (char32_t c = 0xA0; c < k_low; ++c) out.push_back(c);
    for (const char32_t c : {U'\u2013', U'\u2014', U'\u2018', U'\u2019', U'\u201C', U'\u201D', U'\u2022', U'\u2026', U'\u20AC', U'\u2122', U'\u2190',
                             U'\u2191', U'\u2192', U'\u2193', U'\u2212', U'\u00D7'}) {
        out.push_back(c);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

u64 pair_key(char32_t a, char32_t b) { return (static_cast<u64>(a) << 32) | static_cast<u64>(b); }

} // namespace

std::span<const u8> default_font_ttf() { return embedded::default_font(); }

Font::Font() = default;
Font::~Font() = default;
Font::Font(Font&&) noexcept = default;
Font& Font::operator=(Font&&) noexcept = default;

std::unique_ptr<Font> Font::load_default(std::string& error, const FontDesc& desc) {
    return load(default_font_ttf(), error, desc);
}

std::unique_ptr<Font> Font::load(std::span<const u8> ttf, std::string& error, const FontDesc& desc) {
    error.clear();
    if (!(desc.pixel_size >= 8.0f && desc.pixel_size <= 256.0f) || desc.spread < 1 || desc.spread > 64 ||
        desc.atlas_width < 64 || desc.atlas_width > 8192) {
        error = "pixel_size 8 to 256, spread 1 to 64, atlas_width 64 to 8192";
        return nullptr;
    }
    if (ttf.size() < 12 || ttf.size() > (64u << 20)) {
        error = "not a font";
        return nullptr;
    }
    // stb_truetype reads from where the bytes are: kept for the load only.
    stbtt_fontinfo info{};
    const int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
    if (offset < 0 || stbtt_InitFont(&info, ttf.data(), offset) == 0) {
        error = "not a font (TrueType or OpenType with TrueType outlines)";
        return nullptr;
    }
    std::unique_ptr<Font> font(new Font());
    Font& f = *font;
    f.m_desc = desc;
    f.m_scale = stbtt_ScaleForPixelHeight(&info, desc.pixel_size);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    f.m_ascent = static_cast<float>(ascent) * f.m_scale;
    f.m_descent = static_cast<float>(-descent) * f.m_scale;
    f.m_line_height = static_cast<float>(ascent - descent + gap) * f.m_scale;

    // Each glyph's distance field (as stb makes it: 128 on the outline, `spread` pixels to 0 or 255).
    struct Made {
        char32_t c = 0;
        int index = 0; // the font's glyph
        Glyph glyph;
        int w = 0, h = 0;
        unsigned char* pixels = nullptr;
    };
    std::vector<Made> made;
    const float per_pixel = 128.0f / static_cast<float>(desc.spread);
    for (const char32_t c : charset()) {
        const int index = stbtt_FindGlyphIndex(&info, static_cast<int>(c));
        if (index == 0 && c != 0x20) continue; // (not in the font)
        Made m;
        m.c = c;
        m.index = index;
        int advance = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&info, index, &advance, &bearing);
        m.glyph.advance = static_cast<float>(advance) * f.m_scale;
        int xoff = 0, yoff = 0;
        m.pixels = stbtt_GetGlyphSDF(&info, f.m_scale, index, desc.spread, 128, per_pixel, &m.w, &m.h, &xoff, &yoff);
        if (m.pixels != nullptr && m.w > 0 && m.h > 0) {
            m.glyph.empty = false;
            m.glyph.x0 = static_cast<float>(xoff);
            m.glyph.y0 = static_cast<float>(yoff);
            m.glyph.x1 = static_cast<float>(xoff + m.w);
            m.glyph.y1 = static_cast<float>(yoff + m.h);
        } else {
            m.w = m.h = 0;
        }
        made.push_back(m);
    }
    const auto free_all = [&] {
        for (Made& m : made) {
            if (m.pixels != nullptr) stbtt_FreeSDF(m.pixels, nullptr);
            m.pixels = nullptr;
        }
    };
    if (made.empty()) {
        error = "the font has none of the characters";
        return nullptr;
    }

    // Packed: the glyphs with pixels, and the solid block (last).
    std::vector<stbrp_rect> rects;
    for (usize i = 0; i < made.size(); ++i) {
        if (made[i].w == 0) continue;
        stbrp_rect r{};
        r.id = static_cast<int>(i);
        r.w = made[i].w + k_gap;
        r.h = made[i].h + k_gap;
        rects.push_back(r);
    }
    {
        stbrp_rect r{};
        r.id = -1;
        r.w = k_solid + k_gap;
        r.h = k_solid + k_gap;
        rects.push_back(r);
    }
    const int width = static_cast<int>(desc.atlas_width);
    std::vector<stbrp_node> nodes(static_cast<usize>(width));
    stbrp_context packer{};
    stbrp_init_target(&packer, width, static_cast<int>(k_max_height), nodes.data(), static_cast<int>(nodes.size()));
    if (stbrp_pack_rects(&packer, rects.data(), static_cast<int>(rects.size())) == 0) {
        free_all();
        error = "the glyphs do not fit an atlas " + std::to_string(width) + " wide and " +
                std::to_string(k_max_height) + " high";
        return nullptr;
    }
    int used = 0;
    for (const stbrp_rect& r : rects) used = std::max(used, r.y + r.h);
    u32 height = 64;
    while (height < static_cast<u32>(used)) height *= 2;
    f.m_width = desc.atlas_width;
    f.m_height = height;
    f.m_atlas.assign(static_cast<usize>(f.m_width) * f.m_height * 4, 0);
    for (usize i = 0; i < f.m_atlas.size(); i += 4) f.m_atlas[i] = f.m_atlas[i + 1] = f.m_atlas[i + 2] = 255;
    const auto put = [&](int x, int y, u8 a) {
        f.m_atlas[(static_cast<usize>(y) * f.m_width + static_cast<usize>(x)) * 4 + 3] = a;
    };
    const float iw = 1.0f / static_cast<float>(f.m_width), ih = 1.0f / static_cast<float>(f.m_height);
    for (const stbrp_rect& r : rects) {
        if (r.id < 0) {
            for (int y = 0; y < k_solid; ++y)
                for (int x = 0; x < k_solid; ++x) put(r.x + x, r.y + y, 255);
            f.m_solid_u = (static_cast<float>(r.x) + 0.5f * k_solid) * iw;
            f.m_solid_v = (static_cast<float>(r.y) + 0.5f * k_solid) * ih;
            continue;
        }
        Made& m = made[static_cast<usize>(r.id)];
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) put(r.x + x, r.y + y, m.pixels[y * m.w + x]);
        m.glyph.u0 = static_cast<float>(r.x) * iw;
        m.glyph.v0 = static_cast<float>(r.y) * ih;
        m.glyph.u1 = static_cast<float>(r.x + m.w) * iw;
        m.glyph.v1 = static_cast<float>(r.y + m.h) * ih;
    }
    free_all();

    f.m_low.assign(k_low, Glyph{});
    std::vector<bool> low_has(k_low, false);
    std::unordered_map<int, std::vector<char32_t>> by_index; // (some glyphs stand for several characters)
    for (const Made& m : made) {
        if (m.c < k_low) {
            f.m_low[m.c] = m.glyph;
            low_has[m.c] = true;
        } else {
            f.m_high[m.c] = m.glyph;
        }
        if (m.index != 0) by_index[m.index].push_back(m.c);
    }
    f.m_count = made.size();
    if (low_has[U'?']) f.m_fallback = f.m_low[U'?'];
    // Nothing held reads as the fallback (a space has no pixels but is held).
    for (char32_t c = 0; c < k_low; ++c) {
        if (!low_has[c]) f.m_low[c] = f.m_fallback;
    }
    f.m_held.assign(low_has.begin(), low_has.end());

    // Kerning: the kern table's pairs between held glyphs, and (GPOS too) Basic Latin's pairs.
    if (const int n = stbtt_GetKerningTableLength(&info); n > 0 && n <= (1 << 20)) {
        std::vector<stbtt_kerningentry> table(static_cast<usize>(n));
        const int got = stbtt_GetKerningTable(&info, table.data(), n);
        for (int i = 0; i < got; ++i) {
            const stbtt_kerningentry& e = table[static_cast<usize>(i)];
            const auto a = by_index.find(e.glyph1), b = by_index.find(e.glyph2);
            if (a == by_index.end() || b == by_index.end() || e.advance == 0) continue;
            for (const char32_t ca : a->second)
                for (const char32_t cb : b->second) f.m_kerning[pair_key(ca, cb)] = static_cast<float>(e.advance) * f.m_scale;
        }
    }
    for (char32_t a = 0x21; a < 0x7F; ++a) {
        if (!low_has[a]) continue;
        for (char32_t b = 0x20; b < 0x7F; ++b) {
            if (!low_has[b] || f.m_kerning.count(pair_key(a, b)) != 0) continue;
            const int k = stbtt_GetCodepointKernAdvance(&info, static_cast<int>(a), static_cast<int>(b));
            if (k != 0) f.m_kerning[pair_key(a, b)] = static_cast<float>(k) * f.m_scale;
        }
    }
    // Characters that share a glyph (a no-break space is a space) share its pairs.
    std::unordered_map<char32_t, int> index_of;
    for (const Made& m : made) index_of[m.c] = m.index;
    std::vector<std::pair<u64, float>> shared;
    for (const auto& [key, k] : f.m_kerning) {
        const auto ia = index_of.find(static_cast<char32_t>(key >> 32));
        const auto ib = index_of.find(static_cast<char32_t>(key & 0xFFFFFFFFu));
        if (ia == index_of.end() || ib == index_of.end()) continue;
        const auto as = by_index.find(ia->second), bs = by_index.find(ib->second);
        if (as == by_index.end() || bs == by_index.end()) continue;
        for (const char32_t ca : as->second)
            for (const char32_t cb : bs->second) shared.emplace_back(pair_key(ca, cb), k);
    }
    for (const auto& [key, k] : shared) f.m_kerning.emplace(key, k);
    return font;
}

const Glyph& Font::glyph(char32_t c) const {
    if (c < k_low) return m_low[c];
    const auto it = m_high.find(c);
    return it != m_high.end() ? it->second : m_fallback;
}

bool Font::has(char32_t c) const {
    if (c < k_low) return m_held[c];
    return m_high.count(c) != 0;
}

float Font::kerning(char32_t a, char32_t b) const {
    if (m_kerning.empty()) return 0.0f;
    const auto it = m_kerning.find(pair_key(a, b));
    return it != m_kerning.end() ? it->second : 0.0f;
}

} // namespace giant::ui
