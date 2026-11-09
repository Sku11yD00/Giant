// Screens from data: loaded from JSON (strictly), laid out each frame
// (sizes up from the leaves, places down from the root), drawn onto a
// canvas, pointed at and clicked.
#include "giant/ui/screen.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <set>

namespace giant::ui {

using Json = nlohmann::json;

namespace {

constexpr float k_max_units = 100000.0f;
constexpr usize k_max_text = 4096;

bool finite(float v) { return std::isfinite(v); }

// --- Values -----------------------------------------------------------------

std::optional<double> as_number(const Value& v) {
    if (const double* d = std::get_if<double>(&v)) return *d;
    if (const bool* b = std::get_if<bool>(&v)) return *b ? 1.0 : 0.0;
    const std::string& s = std::get<std::string>(v);
    double out = 0.0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc() || end != s.data() + s.size() || !std::isfinite(out)) return std::nullopt;
    return out;
}

std::string format_number(double v, int decimals) {
    if (!std::isfinite(v)) return "?";
    char buffer[400]; // (1e308 to 9 places)
    if (decimals >= 0) {
        std::snprintf(buffer, sizeof(buffer), "%.*f", std::min(decimals, 9), v);
        return buffer;
    }
    // As few places as show it (at most 3).
    std::snprintf(buffer, sizeof(buffer), "%.3f", v);
    std::string s = buffer;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

// --- Loading ----------------------------------------------------------------

struct Loader {
    std::string error;
    usize count = 0;
    std::set<std::string, std::less<>> ids;

    bool fail(const std::string& where, const std::string& why) {
        if (error.empty()) error = where + ": " + why;
        return false;
    }

    bool only(const Json& j, const std::string& where, std::initializer_list<const char*> keys) {
        for (const auto& [k, v] : j.items()) {
            (void)v;
            if (std::none_of(keys.begin(), keys.end(), [&](const char* n) { return k == n; }))
                return fail(where, "no key \"" + k + "\"");
        }
        return true;
    }

    bool number(const Json& j, const std::string& where, const char* key, float lo, float hi, float& out) {
        if (!j.contains(key)) return true;
        const Json& v = j[key];
        if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < lo || v.get<double>() > hi)
            return fail(where, std::string(key) + " is a number from " + format_number(lo, -1) + " to " +
                                   format_number(hi, -1));
        out = v.get<float>();
        return true;
    }

    bool pair(const Json& j, const std::string& where, const char* key, float lo, float hi, Vec2& out) {
        if (!j.contains(key)) return true;
        const Json& v = j[key];
        if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
            return fail(where, std::string(key) + " is [x, y]");
        const double x = v[0].get<double>(), y = v[1].get<double>();
        if (!std::isfinite(x) || !std::isfinite(y) || x < lo || x > hi || y < lo || y > hi)
            return fail(where, std::string(key) + "'s numbers are from " + format_number(lo, -1) + " to " +
                                   format_number(hi, -1));
        out = {static_cast<float>(x), static_cast<float>(y)};
        return true;
    }

    bool flag(const Json& j, const std::string& where, const char* key, bool& out) {
        if (!j.contains(key)) return true;
        if (!j[key].is_boolean()) return fail(where, std::string(key) + " is true or false");
        out = j[key].get<bool>();
        return true;
    }

    bool text(const Json& j, const std::string& where, const char* key, std::string& out, usize most = k_max_text) {
        if (!j.contains(key)) return true;
        if (!j[key].is_string() || j[key].get_ref<const std::string&>().size() > most)
            return fail(where, std::string(key) + " is text (at most " + std::to_string(most) + " bytes)");
        out = j[key].get<std::string>();
        return true;
    }

    bool color(const Json& j, const std::string& where, const char* key, u32& out) {
        if (!j.contains(key)) return true;
        const Json& v = j[key];
        if (v.is_string()) {
            if (const std::optional<u32> c = parse_color(v.get_ref<const std::string&>())) {
                out = *c;
                return true;
            }
        }
        return fail(where, std::string(key) + " is a colour \"#rrggbb\" or \"#rrggbbaa\"");
    }

    template <typename E>
    bool choice(const Json& j, const std::string& where, const char* key,
                std::initializer_list<std::pair<const char*, E>> options, E& out) {
        if (!j.contains(key)) return true;
        if (j[key].is_string()) {
            for (const auto& [name, e] : options) {
                if (j[key].get_ref<const std::string&>() == name) {
                    out = e;
                    return true;
                }
            }
        }
        std::string names;
        for (const auto& [name, e] : options) names += (names.empty() ? "" : ", ") + std::string(name);
        return fail(where, std::string(key) + " is one of " + names);
    }

    // A number, or a value's name.
    bool bound(const Json& j, const std::string& where, const char* key, double& number_out, std::string& name_out) {
        if (!j.contains(key)) return true;
        const Json& v = j[key];
        if (v.is_number() && std::isfinite(v.get<double>())) {
            number_out = v.get<double>();
            return true;
        }
        if (v.is_string() && !v.get_ref<const std::string&>().empty() && v.get_ref<const std::string&>().size() <= 128) {
            name_out = v.get<std::string>();
            return true;
        }
        return fail(where, std::string(key) + " is a number or a value's name");
    }

    bool widget(const Json& j, Widget& w, const std::string& where, usize depth) {
        if (depth > Screen::k_max_depth) return fail(where, "nested deeper than " + std::to_string(Screen::k_max_depth));
        if (++count > Screen::k_max_widgets) return fail(where, "more than " + std::to_string(Screen::k_max_widgets) + " widgets");
        if (!j.is_object()) return fail(where, "a widget is an object");
        if (!only(j, where,
                  {"type", "id", "anchor", "pivot", "offset", "size", "stretch", "layout", "gap", "padding", "clip",
                   "visible", "background", "border", "border_color", "color", "text", "font_size", "align", "wrap",
                   "outline", "outline_color", "shadow", "value", "max", "direction", "image", "hover", "span", "fade",
                   "children"}))
            return false;
        if (!j.contains("type")) return fail(where, "no \"type\"");
        if (!choice<WidgetType>(j, where, "type",
                                {{"panel", WidgetType::panel},
                                 {"text", WidgetType::text},
                                 {"bar", WidgetType::bar},
                                 {"image", WidgetType::image},
                                 {"button", WidgetType::button},
                                 {"compass", WidgetType::compass}},
                                w.type))
            return false;
        const std::string here = j.contains("id") && j["id"].is_string() ? where + " (\"" + j["id"].get<std::string>() + "\")" : where;
        if (!text(j, here, "id", w.id, 128)) return false;
        if (w.type == WidgetType::button && w.id.empty()) return fail(here, "a button needs an id (its clicks say it)");
        if (!w.id.empty() && !ids.insert(w.id).second) return fail(here, "another widget has this id");
        w.anchor = {};
        if (!pair(j, here, "anchor", -1.0f, 2.0f, w.anchor)) return false;
        w.pivot = w.anchor;
        if (!pair(j, here, "pivot", -1.0f, 2.0f, w.pivot) || !pair(j, here, "offset", -k_max_units, k_max_units, w.offset) ||
            !pair(j, here, "size", 0.0f, k_max_units, w.size))
            return false;
        if (j.contains("stretch")) {
            const Json& s = j["stretch"];
            if (!s.is_array() || s.size() != 2 || !s[0].is_boolean() || !s[1].is_boolean())
                return fail(here, "stretch is [bool, bool]");
            w.stretch_x = s[0].get<bool>();
            w.stretch_y = s[1].get<bool>();
        }
        if (!choice<Layout>(j, here, "layout", {{"none", Layout::none}, {"column", Layout::column}, {"row", Layout::row}},
                            w.layout) ||
            !number(j, here, "gap", 0.0f, k_max_units, w.gap) || !number(j, here, "padding", 0.0f, k_max_units, w.padding) ||
            !flag(j, here, "clip", w.clip) || !text(j, here, "visible", w.visible, 129) ||
            !color(j, here, "background", w.background) || !number(j, here, "border", 0.0f, 1000.0f, w.border) ||
            !color(j, here, "border_color", w.border_color) || !color(j, here, "color", w.color) ||
            !text(j, here, "text", w.text) || !number(j, here, "font_size", 1.0f, 1000.0f, w.font_size) ||
            !choice<Align>(j, here, "align", {{"left", Align::left}, {"center", Align::center}, {"right", Align::right}},
                           w.align) ||
            !flag(j, here, "wrap", w.wrap) || !number(j, here, "outline", 0.0f, 100.0f, w.outline) ||
            !color(j, here, "outline_color", w.outline_color) || !number(j, here, "shadow", 0.0f, 100.0f, w.shadow) ||
            !bound(j, here, "value", w.value, w.value_name) || !bound(j, here, "max", w.max, w.max_name) ||
            !choice<Direction>(j, here, "direction",
                               {{"right", Direction::right}, {"left", Direction::left}, {"up", Direction::up}, {"down", Direction::down}},
                               w.direction) ||
            !text(j, here, "image", w.image, 128) || !color(j, here, "hover", w.hover) ||
            !number(j, here, "span", 10.0f, 360.0f, w.span) || !number(j, here, "fade", 0.0f, 10.0f, w.fade))
            return false;
        if (w.visible == "!" || (w.visible.size() > 128 && w.visible[0] != '!')) return fail(here, "visible names a value");
        if (j.contains("children")) {
            const Json& c = j["children"];
            if (!c.is_array()) return fail(here, "children is a list");
            if (count + c.size() > Screen::k_max_widgets)
                return fail(here, "more than " + std::to_string(Screen::k_max_widgets) + " widgets");
            w.children.resize(c.size());
            for (usize i = 0; i < c.size(); ++i) {
                if (!widget(c[i], w.children[i], here + ".children[" + std::to_string(i) + "]", depth + 1)) return false;
            }
        }
        return true;
    }
};

// --- Layout -----------------------------------------------------------------

struct Layouter {
    const Font& font;
    const Values& values;
    float scale;
    usize budget; // bytes of text left to show
    usize used = 0;
    mutable std::vector<std::string> read; // the values' names read

    // Filled in, cut to what is left of the budget (at a letter's start).
    // Charged the larger of the template and what it makes (so a template
    // that makes nothing still costs its work); none once it is spent.
    std::string fill(const std::string& text) {
        if (budget == 0 || text.empty()) return {};
        std::string out = fill_text(text, values, &read);
        if (out.size() > budget) {
            usize n = budget;
            while (n > 0 && (static_cast<u8>(out[n]) & 0xC0) == 0x80) --n;
            out.resize(n);
        }
        const usize cost = std::min(std::max(text.size(), out.size()), budget);
        budget -= cost;
        used += cost;
        return out;
    }

    bool visible(const Widget& w) const {
        if (w.visible.empty()) return true;
        const bool negated = w.visible[0] == '!';
        const std::string_view name = negated ? std::string_view(w.visible).substr(1) : std::string_view(w.visible);
        read.emplace_back(name);
        return values.truthy(name) != negated;
    }

    TextStyle style_of(const Widget& w) const {
        TextStyle s;
        s.size = w.font_size * scale;
        s.color = w.color;
        s.outline = w.outline * scale;
        s.outline_color = w.outline_color;
        s.shadow = w.shadow * scale;
        s.align = w.align;
        return s;
    }

    // The size it would take with nothing stretching it (pixels), its shown flag set.
    Vec2 natural(Widget& w, float wrap_width) {
        w.own_visible = visible(w);
        w.shown = w.own_visible;
        if (!w.shown) {
            hide(w, false); // (and all it holds: none of them shown, clicked or focused)
            return {};
        }
        const float pad = 2.0f * w.padding * scale;
        Vec2 size{w.size.x * scale, w.size.y * scale};
        // What it holds is as wide as it will be: its parent's inside if it stretches, else its size (or the parent's).
        const float inner = std::max((w.stretch_x || size.x <= 0.0f ? wrap_width : size.x) - pad, 1.0f);
        w.shown_text.clear();
        if (w.type == WidgetType::text || w.type == WidgetType::button) w.shown_text = fill(w.text);
        if ((w.type == WidgetType::text || w.type == WidgetType::button) && (size.x == 0.0f || size.y == 0.0f)) {
            TextStyle s = style_of(w);
            if (w.wrap) s.wrap = inner;
            const Vec2 t = measure(font, w.shown_text, s);
            if (size.x == 0.0f) size.x = t.x + pad;
            if (size.y == 0.0f) size.y = t.y + pad;
        }
        // A row or column: what its children take, with gaps and padding.
        Vec2 content{};
        usize n = 0;
        for (Widget& c : w.children) {
            const Vec2 cs = natural(c, inner);
            if (!c.shown) continue;
            if (w.layout == Layout::column) {
                content.x = std::max(content.x, cs.x);
                content.y += cs.y + (n > 0 ? w.gap * scale : 0.0f);
            } else if (w.layout == Layout::row) {
                content.x += cs.x + (n > 0 ? w.gap * scale : 0.0f);
                content.y = std::max(content.y, cs.y);
            }
            ++n;
        }
        if (w.layout != Layout::none) {
            if (w.size.x == 0.0f) size.x = std::max(size.x, content.x + pad);
            if (w.size.y == 0.0f) size.y = std::max(size.y, content.y + pad);
        }
        w.rect = {0.0f, 0.0f, size.x, size.y}; // kept for place()
        return size;
    }

    // Not shown (nor clicked or focused); still drawn where it was while it fades out.
    void hide(Widget& w, bool fading) {
        w.shown = false;
        w.own_visible = visible(w);
        fading = fading || (w.fade > 0.0f && w.opacity > 0.0f);
        if (!fading) {
            w.rect = {};
            w.shown_text.clear();
        } else if (!w.shown_text.empty()) {
            // Still drawn: its text still counts (cut to what is left).
            if (w.shown_text.size() > budget) {
                usize n = budget;
                while (n > 0 && (static_cast<u8>(w.shown_text[n]) & 0xC0) == 0x80) --n;
                w.shown_text.resize(n);
            }
            budget -= w.shown_text.size();
            used += w.shown_text.size();
        }
        for (Widget& c : w.children) hide(c, fading);
    }

    // Placed in `parent` (its inside), from its natural size.
    void place(Widget& w, const Rect& inside, Vec2 size, bool stacked, Vec2 at) {
        if (w.stretch_x) size.x = inside.w;
        if (w.stretch_y) size.y = inside.h;
        if (stacked) {
            w.rect = {at.x + w.offset.x * scale, at.y + w.offset.y * scale, size.x, size.y};
        } else {
            w.rect = {inside.x + w.anchor.x * inside.w + w.offset.x * scale - w.pivot.x * size.x,
                      inside.y + w.anchor.y * inside.h + w.offset.y * scale - w.pivot.y * size.y, size.x, size.y};
        }
        const float pad = w.padding * scale;
        const Rect in{w.rect.x + pad, w.rect.y + pad, std::max(w.rect.w - 2.0f * pad, 0.0f),
                      std::max(w.rect.h - 2.0f * pad, 0.0f)};
        Vec2 cursor{in.x, in.y};
        for (Widget& c : w.children) {
            if (!c.shown) continue;
            Vec2 cs = natural_size(c);
            if (w.layout == Layout::column) {
                // Across a column: stretch fills its width; else the anchor's x says where.
                Vec2 at_c = cursor;
                if (!c.stretch_x) at_c.x = in.x + c.anchor.x * in.w - c.pivot.x * cs.x;
                if (c.stretch_x) cs.x = in.w;
                place(c, {in.x, cursor.y, in.w, cs.y}, cs, true, at_c);
                cursor.y += c.rect.h + w.gap * scale;
            } else if (w.layout == Layout::row) {
                Vec2 at_c = cursor;
                if (!c.stretch_y) at_c.y = in.y + c.anchor.y * in.h - c.pivot.y * cs.y;
                if (c.stretch_y) cs.y = in.h;
                place(c, {cursor.x, in.y, cs.x, in.h}, cs, true, at_c);
                cursor.x += c.rect.w + w.gap * scale;
            } else {
                place(c, in, cs, false, {});
            }
        }
    }

    // Each widget's natural size, kept from the measuring pass (in `rect`'s w and h until placed).
    static Vec2 natural_size(const Widget& w) { return {w.rect.w, w.rect.h}; }
};

u32 faded(u32 color, float alpha) {
    if (alpha >= 1.0f) return color;
    const float a = static_cast<float>(color >> 24) * std::clamp(alpha, 0.0f, 1.0f);
    return (color & 0x00FFFFFFu) | (static_cast<u32>(std::lround(a)) << 24);
}

// How opaque it is drawn by itself: eased if it fades, else shown or not.
float own_opacity(const Widget& w) { return w.fade > 0.0f ? w.opacity : (w.own_visible ? 1.0f : 0.0f); }

void draw_compass(Canvas& canvas, const Font& font, const Values& values, float scale, const Widget& w, const Rect& in,
                  float alpha, std::span<const CompassMark> marks) {
    if (!(in.w > 0.0f) || !(in.h > 0.0f)) return;
    const double raw = w.value_name.empty() ? w.value : values.number(w.value_name);
    const float heading = std::isfinite(raw) ? static_cast<float>(std::fmod(std::fmod(raw, 360.0) + 360.0, 360.0)) : 0.0f;
    const float span = std::clamp(w.span, 10.0f, 360.0f);
    const float per = in.w / span, cx = in.x + 0.5f * in.w;
    const u32 color = faded(w.color, alpha);
    const float t = std::max(std::round(2.0f * scale), 1.0f);
    canvas.push_clip(in);
    static constexpr const char* k_names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    TextStyle s;
    s.color = color;
    s.align = Align::center;
    s.outline = w.outline * scale;
    s.outline_color = faded(w.outline_color, alpha);
    s.shadow = w.shadow * scale;
    s.shadow_color = faded(rgba(0, 0, 0, 160), alpha);
    // Ticks every 15 degrees (taller at 45), letters at 45.
    const float from = std::ceil((heading - 0.5f * span) / 15.0f) * 15.0f;
    for (float d = from; d <= heading + 0.5f * span + 0.01f; d += 15.0f) {
        const float x = std::round(cx + (d - heading) * per);
        const int at = static_cast<int>(std::lround(d)) % 360;
        const int deg = at < 0 ? at + 360 : at;
        if (deg % 45 == 0) {
            s.size = (deg % 90 == 0 ? 1.0f : 0.75f) * w.font_size * scale;
            canvas.text({x, in.y + 0.42f * in.h}, k_names[deg / 45], s);
            canvas.rect({x - 0.5f * t, in.y, t, 0.35f * in.h}, color);
        } else {
            canvas.rect({x - 0.5f * t, in.y, t, 0.2f * in.h}, faded(color, 0.7f));
        }
    }
    // Markers by bearing: on it where they are, at its edge (dimmer) past it.
    const float r = std::max(std::round(0.12f * in.h), 2.0f);
    for (const CompassMark& m : marks) {
        const float off = turn_degrees(heading, m.bearing);
        const bool past = std::fabs(off) > 0.5f * span;
        const float x = std::round(cx + std::clamp(off, -0.5f * span, 0.5f * span) * per - (past ? std::copysign(r, off) : 0.0f));
        canvas.rect({x - r, in.y + in.h - 2.0f * r - t, 2.0f * r, 2.0f * r}, faded(m.color, alpha * (past ? 0.5f : 1.0f)));
    }
    // Where it looks: a mark in the middle.
    canvas.rect({std::round(cx) - t, in.y, 2.0f * t, in.h}, faded(color, 0.35f));
    canvas.pop_clip();
    (void)font;
}

void draw_widget(Canvas& canvas, const Font& font, const Values& values, float scale, const Widget& w,
                 const std::string& focus, const std::string& hovered,
                 const std::function<rhi::TextureId(std::string_view)>& images, float alpha,
                 std::span<const CompassMark> marks) {
    alpha *= own_opacity(w);
    if (alpha <= 0.0f || (!w.shown && w.rect.w <= 0.0f && w.rect.h <= 0.0f)) return;
    const Rect& r = w.rect;
    const bool lit = w.shown && w.type == WidgetType::button && !w.id.empty() && (w.id == focus || w.id == hovered);
    const u32 background = faded(lit && w.hover != 0 ? w.hover : w.background, alpha);
    if ((background >> 24) != 0) canvas.rect(r, background);
    const float pad = w.padding * scale;
    const Rect in{r.x + pad, r.y + pad, std::max(r.w - 2.0f * pad, 0.0f), std::max(r.h - 2.0f * pad, 0.0f)};
    switch (w.type) {
    case WidgetType::text:
    case WidgetType::button: {
        if (w.shown_text.empty()) break;
        TextStyle s;
        s.size = w.font_size * scale;
        s.color = faded(w.color, alpha);
        s.outline = w.outline * scale;
        s.outline_color = faded(w.outline_color, alpha);
        s.shadow = w.shadow * scale;
        s.shadow_color = faded(s.shadow_color, alpha);
        s.align = w.align;
        s.wrap = std::max(in.w, 1.0f); // aligned within its inside (wrapped there only if it wraps)
        const std::string& shown = w.shown_text;
        if (!w.wrap) {
            // Not wrapped: aligned in the box, one line a line break.
            s.wrap = 0.0f;
            const float x = w.align == Align::left ? in.x : w.align == Align::center ? in.x + 0.5f * in.w : in.x + in.w;
            const Vec2 t = measure(font, shown, s);
            canvas.text({x, in.y + 0.5f * (in.h - t.y)}, shown, s);
        } else {
            const Vec2 t = measure(font, shown, s);
            canvas.text({in.x, in.y + std::max(0.5f * (in.h - t.y), 0.0f)}, shown, s);
        }
        break;
    }
    case WidgetType::bar: {
        const double value = w.value_name.empty() ? w.value : values.number(w.value_name);
        const double max = w.max_name.empty() ? w.max : values.number(w.max_name);
        const float f = max > 0.0 && std::isfinite(value / max) ? static_cast<float>(std::clamp(value / max, 0.0, 1.0)) : 0.0f;
        Rect fill = in;
        switch (w.direction) {
        case Direction::right: fill.w = in.w * f; break;
        case Direction::left: fill.x = in.x + in.w * (1.0f - f); fill.w = in.w * f; break;
        case Direction::down: fill.h = in.h * f; break;
        case Direction::up: fill.y = in.y + in.h * (1.0f - f); fill.h = in.h * f; break;
        }
        canvas.rect(fill, faded(w.color, alpha));
        break;
    }
    case WidgetType::image:
        if (!w.image.empty() && images) {
            const rhi::TextureId t = images(w.image);
            if (t != 0) canvas.image(in, t, {0.0f, 0.0f, 1.0f, 1.0f}, faded(w.color, alpha));
        }
        break;
    case WidgetType::compass: draw_compass(canvas, font, values, scale, w, in, alpha, marks); break;
    case WidgetType::panel: break;
    }
    if (w.border > 0.0f && (w.border_color >> 24) != 0)
        canvas.frame(r, std::max(w.border * scale, 1.0f), faded(w.border_color, alpha));
    if (w.clip) canvas.push_clip(r);
    for (const Widget& c : w.children) draw_widget(canvas, font, values, scale, c, focus, hovered, images, alpha, marks);
    if (w.clip) canvas.pop_clip();
}

bool inside(const Rect& r, Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }

// The last (top) shown button at `p`, within its clipping ancestors.
// Clicked and focused: shown, and if it fades, at least half way in.
bool live(const Widget& w) { return w.shown && (w.fade <= 0.0f || w.opacity >= 0.5f); }

const Widget* button_at(const Widget& w, Vec2 p, bool clipped_out) {
    if (!w.shown) return nullptr;
    if (w.clip && !inside(w.rect, p)) clipped_out = true;
    for (auto it = w.children.rbegin(); it != w.children.rend(); ++it) {
        if (const Widget* b = button_at(*it, p, clipped_out)) return b;
    }
    if (!clipped_out && w.type == WidgetType::button && live(w) && inside(w.rect, p)) return &w;
    return nullptr;
}

// Whether a widget drawn with a background (seen: not clipped out) is at
// `p`: shown, or fading out (what the player sees takes the click).
bool covers(const Widget& w, Vec2 p, bool clipped_out, bool fading) {
    fading = fading || (!w.shown && w.fade > 0.0f && w.opacity > 0.0f);
    if (!w.shown && !fading) return false;
    if (fading && !w.own_visible && !(w.fade > 0.0f && w.opacity > 0.0f)) return false;
    if (w.clip && !inside(w.rect, p)) clipped_out = true;
    if (!clipped_out && (w.background >> 24) != 0 && inside(w.rect, p)) return true;
    for (const Widget& c : w.children) {
        if (covers(c, p, clipped_out, fading)) return true;
    }
    return false;
}

void buttons(const Widget& w, std::vector<const Widget*>& out) {
    if (!w.shown) return;
    if (w.type == WidgetType::button && live(w)) out.push_back(&w);
    for (const Widget& c : w.children) buttons(c, out);
}

const Widget* find_in(const Widget& w, std::string_view id) {
    if (w.id == id) return &w;
    for (const Widget& c : w.children) {
        if (const Widget* f = find_in(c, id)) return f;
    }
    return nullptr;
}

} // namespace

// --- Values -----------------------------------------------------------------

namespace {
// Generations from one count for all Values, so two never share one with different contents.
std::atomic<u64> g_generations{0};
u64 next_generation() { return g_generations.fetch_add(1, std::memory_order_relaxed) + 1; }
} // namespace

Values::Values() : m_generation(next_generation()) {}

void Values::set(const std::string& name, Value value) {
    if (name.empty() || name.size() > 128) return;
    if (const double* d = std::get_if<double>(&value); d != nullptr && !std::isfinite(*d)) value = 0.0;
    if (std::string* s = std::get_if<std::string>(&value); s != nullptr && s->size() > k_max_text) s->resize(k_max_text);
    const auto it = m_values.find(name);
    if (it != m_values.end()) {
        if (it->second == value) return; // (unchanged: the screens need not lay out again)
        it->second = std::move(value);
    } else if (m_values.size() < k_max_values) {
        m_values.emplace(name, std::move(value));
    } else {
        return;
    }
    m_generation = next_generation();
    m_generations[name] = m_generation;
}

void Values::erase(const std::string& name) {
    if (m_values.erase(name) > 0) {
        m_generations.erase(name);
        m_generation = next_generation();
    }
}

void Values::clear() {
    m_values.clear();
    m_generations.clear();
    m_generation = next_generation();
}

u64 Values::generation(std::string_view name) const {
    const auto it = m_generations.find(name);
    return it != m_generations.end() ? it->second : 0;
}

const Value* Values::find(std::string_view name) const {
    const auto it = m_values.find(name);
    return it != m_values.end() ? &it->second : nullptr;
}

double Values::number(std::string_view name, double otherwise) const {
    const Value* v = find(name);
    if (v == nullptr) return otherwise;
    return as_number(*v).value_or(otherwise);
}

bool Values::truthy(std::string_view name) const {
    const Value* v = find(name);
    if (v == nullptr) return false;
    if (const bool* b = std::get_if<bool>(v)) return *b;
    if (const double* d = std::get_if<double>(v)) return *d != 0.0;
    return !std::get<std::string>(*v).empty();
}

std::string Values::text(std::string_view name, int decimals) const {
    const Value* v = find(name);
    if (v == nullptr) return {};
    if (const std::string* s = std::get_if<std::string>(v)) return *s;
    if (const bool* b = std::get_if<bool>(v)) return *b ? "true" : "false";
    return format_number(std::get<double>(*v), decimals);
}

std::optional<u32> parse_color(std::string_view s) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return std::nullopt;
    u8 c[4] = {0, 0, 0, 255};
    for (usize i = 1; i + 1 < s.size(); i += 2) {
        const int hi = hex(s[i]), lo = hex(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        c[i / 2] = static_cast<u8>(hi * 16 + lo);
    }
    return rgba(c[0], c[1], c[2], c[3]);
}

std::string fill_text(std::string_view text, const Values& values) { return fill_text(text, values, nullptr); }

std::string fill_text(std::string_view text, const Values& values, std::vector<std::string>* read) {
    if (text.find_first_of("{}") == std::string_view::npos) return std::string(text);
    std::string out;
    out.reserve(text.size());
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c) {
            out.push_back(c);
            ++i;
            continue;
        }
        if (c != '{') {
            out.push_back(c);
            continue;
        }
        const usize end = text.find('}', i + 1);
        if (end == std::string_view::npos) {
            out.append(text.substr(i));
            break;
        }
        std::string_view name = text.substr(i + 1, end - i - 1);
        int decimals = -1;
        if (const usize colon = name.find(':'); colon != std::string_view::npos) {
            const std::string_view d = name.substr(colon + 1);
            int n = 0;
            const auto [p, ec] = std::from_chars(d.data(), d.data() + d.size(), n);
            if (ec == std::errc() && p == d.data() + d.size() && n >= 0 && n <= 9) decimals = n;
            name = name.substr(0, colon);
        }
        out += values.text(name, decimals);
        if (read != nullptr) read->emplace_back(name);
        i = end;
        if (out.size() >= k_max_text) break; // (a pathological template)
    }
    if (out.size() > k_max_text) {
        usize n = k_max_text;
        while (n > 0 && (static_cast<u8>(out[n]) & 0xC0) == 0x80) --n;
        out.resize(n);
    }
    return out;
}

// --- Screen -----------------------------------------------------------------

std::unique_ptr<Screen> Screen::load(std::string_view json, std::string& error) {
    error.clear();
    if (json.size() > (8u << 20)) {
        error = "a screen is at most 8 MB of JSON";
        return nullptr;
    }
    Json j = Json::parse(json, nullptr, false, true);
    if (j.is_discarded()) {
        error = "not JSON";
        return nullptr;
    }
    auto screen = std::unique_ptr<Screen>(new Screen());
    Loader loader;
    if (!loader.widget(j, screen->m_root, "screen", 0)) {
        error = loader.error;
        return nullptr;
    }
    screen->m_count = loader.count;
    // The root fills the screen: where and how big it is written are not used.
    Widget& root = screen->m_root;
    root.anchor = root.pivot = root.offset = root.size = {};
    root.stretch_x = root.stretch_y = false;
    return screen;
}

void Screen::lay_out(const Font& font, const Values& values, Vec2 size, float scale, usize* text_budget) {
    if (!finite(size.x) || !finite(size.y) || !finite(scale) || scale <= 0.0f) return;
    size = {std::max(size.x, 0.0f), std::max(size.y, 0.0f)};
    const usize budget = std::min(text_budget != nullptr ? *text_budget : k_max_text_bytes, k_max_text_bytes);
    // Nothing changed: as it was (its text still counted).
    const bool same_reads = std::all_of(m_reads.begin(), m_reads.end(),
                                        [&](const auto& r) { return values.generation(r.first) == r.second; });
    if (!m_dirty && m_font == &font && m_values == &values && same_reads && m_size.x == size.x && m_size.y == size.y &&
        m_scale == scale && m_budget == budget) {
        if (text_budget != nullptr) *text_budget -= std::min(*text_budget, m_text_bytes);
        return;
    }
    Layouter l{font, values, scale, budget, 0, {}};
    l.natural(m_root, size.x);
    l.place(m_root, {0.0f, 0.0f, size.x, size.y}, size, false, {}); // (the root fills the screen)
    m_text_bytes = l.used;
    if (text_budget != nullptr) *text_budget -= std::min(*text_budget, m_text_bytes);
    m_font = &font;
    m_values = &values;
    m_dirty = false;
    std::sort(l.read.begin(), l.read.end());
    l.read.erase(std::unique(l.read.begin(), l.read.end()), l.read.end());
    m_reads.clear();
    m_reads.reserve(l.read.size());
    for (std::string& name : l.read) {
        const u64 g = values.generation(name);
        m_reads.emplace_back(std::move(name), g);
    }
    m_size = size;
    m_scale = scale;
    m_budget = budget;
    // The focus on a button no longer shown moves on.
    if (!m_focus.empty()) {
        const Widget* f = find(m_focus);
        if (f == nullptr || !f->shown) m_focus.clear();
    }
}

void Screen::draw(Canvas& canvas, const Font& font, const Values& values, float scale,
                  const std::function<rhi::TextureId(std::string_view)>& images, std::span<const CompassMark> marks) const {
    draw_widget(canvas, font, values, scale, m_root, m_focus, m_focus, images, 1.0f, marks);
}

namespace {
// True if a widget fading out has gone (what it kept is let go of at the next layout).
bool advance_widget(Widget& w, float seconds) {
    bool gone = false;
    if (w.fade > 0.0f) {
        const float target = w.own_visible ? 1.0f : 0.0f;
        const float step = seconds / w.fade;
        const float was = w.opacity;
        w.opacity = w.opacity < target ? std::min(w.opacity + step, target) : std::max(w.opacity - step, target);
        gone = was > 0.0f && w.opacity <= 0.0f && !w.shown;
    } else {
        w.opacity = w.own_visible ? 1.0f : 0.0f;
    }
    for (Widget& c : w.children) gone = advance_widget(c, seconds) || gone;
    return gone;
}
} // namespace

void Screen::advance(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    if (advance_widget(m_root, static_cast<float>(std::min(seconds, 10.0)))) m_dirty = true;
}

const Widget* Screen::button_at(Vec2 point) const { return ui::button_at(m_root, point, false); }

bool Screen::covers(Vec2 point) const { return ui::covers(m_root, point, false, false); }

void Screen::point(Vec2 p) {
    if (const Widget* b = button_at(p)) m_focus = b->id;
}

std::optional<std::string> Screen::click(Vec2 p) {
    const Widget* b = button_at(p);
    if (b == nullptr) return std::nullopt;
    m_focus = b->id;
    return b->id;
}

void Screen::move_focus(int step) {
    std::vector<const Widget*> all;
    buttons(m_root, all);
    if (all.empty() || step == 0) return;
    usize at = all.size(); // (none focused: the first, or the last going back)
    for (usize i = 0; i < all.size(); ++i) {
        if (all[i]->id == m_focus) at = i;
    }
    const i64 n = static_cast<i64>(all.size());
    i64 next = 0;
    if (at == all.size()) {
        next = step > 0 ? 0 : n - 1;
    } else {
        next = ((static_cast<i64>(at) + (step > 0 ? 1 : -1)) % n + n) % n;
    }
    m_focus = all[static_cast<usize>(next)]->id;
}

std::optional<std::string> Screen::activate() const {
    if (m_focus.empty()) return std::nullopt;
    const Widget* f = find(m_focus);
    if (f == nullptr || !f->shown || f->type != WidgetType::button) return std::nullopt;
    return m_focus;
}

void Screen::focus(std::string id) {
    const Widget* w = find(id);
    m_focus = w != nullptr && w->type == WidgetType::button && w->shown ? std::move(id) : std::string();
}

bool Screen::shown(std::string_view id) const {
    const Widget* w = find(id);
    return w != nullptr && w->shown;
}

const Widget* Screen::find(std::string_view id) const {
    if (id.empty()) return nullptr;
    return find_in(m_root, id);
}

} // namespace giant::ui
