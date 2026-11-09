// Screens from data: loaded strictly from JSON, laid out by anchors, rows
// and columns at any size, filled in from values, drawn, clicked and moved
// through with keys.
#include "giant/ui/screen.h"

#include "giant_test.h"

#include <cmath>
#include <string>

using namespace giant;
using namespace giant::ui;

namespace {

const Font& default_font() {
    static const std::unique_ptr<Font> font = [] {
        std::string error;
        auto f = Font::load_default(error);
        GIANT_CHECK(f != nullptr && error.empty());
        if (f != nullptr) f->texture = 7;
        return f;
    }();
    return *font;
}

std::unique_ptr<Screen> load(const std::string& json) {
    std::string error;
    auto s = Screen::load(json, error);
    if (s == nullptr) std::printf("    load failed: %s\n", error.c_str());
    GIANT_CHECK(s != nullptr && error.empty());
    return s;
}

std::string refused(const std::string& json) {
    std::string error;
    auto s = Screen::load(json, error);
    GIANT_CHECK(s == nullptr && !error.empty());
    return error;
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

} // namespace

GIANT_TEST("ui values: numbers, text and flags by name; text filled in from them") {
    Values v;
    v.set("ammo", 30.0);
    v.set("name", std::string("Stalker"));
    v.set("alive", true);
    v.set("health", 0.756);
    v.set("bad", std::nan(""));
    v.set("count", std::string("12"));
    GIANT_CHECK(v.number("ammo") == 30.0 && v.number("alive") == 1.0 && v.number("count") == 12.0);
    GIANT_CHECK(v.number("name", -1.0) == -1.0 && v.number("missing", 5.0) == 5.0);
    GIANT_CHECK(v.number("bad") == 0.0); // not a number: kept as 0
    GIANT_CHECK(v.truthy("ammo") && v.truthy("name") && v.truthy("alive") && !v.truthy("missing"));
    v.set("alive", false);
    v.set("empty", std::string());
    GIANT_CHECK(!v.truthy("alive") && !v.truthy("empty"));
    GIANT_CHECK(v.text("ammo") == "30" && v.text("health") == "0.756" && v.text("health", 1) == "0.8");
    GIANT_CHECK(v.text("alive") == "false" && v.text("missing").empty());
    v.set("", 1.0);
    v.set(std::string(200, 'x'), 1.0);
    GIANT_CHECK(v.find("") == nullptr && v.size() == 7);
    v.erase("bad");
    GIANT_CHECK(v.find("bad") == nullptr && v.size() == 6);

    GIANT_CHECK(fill_text("Ammo {ammo}", v) == "Ammo 30");
    GIANT_CHECK(fill_text("{health:2} {name}!", v) == "0.76 Stalker!");
    GIANT_CHECK(fill_text("{{ammo}} {missing}.", v) == "{ammo} .");
    GIANT_CHECK(fill_text("open {ammo", v) == "open {ammo");
    GIANT_CHECK(fill_text("{health:x}", v) == "0.756"); // a bad count of places: as few as show it
    GIANT_CHECK(fill_text("plain", v) == "plain");

    // At most k_max_values names: more are not kept (an existing one still changes).
    Values many;
    for (usize i = 0; i < Values::k_max_values + 10; ++i) many.set("v" + std::to_string(i), 1.0);
    GIANT_CHECK(many.size() == Values::k_max_values);
    many.set("v0", 2.0);
    GIANT_CHECK(many.number("v0") == 2.0);
}

GIANT_TEST("ui screen: JSON refused where it is not a screen, saying where") {
    GIANT_CHECK(refused("not json") == "not JSON");
    GIANT_CHECK(refused("[]").find("object") != std::string::npos);
    GIANT_CHECK(refused(R"({"id": "a"})").find("type") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "pannel"})").find("type is one of") != std::string::npos);
    const std::string typo = refused(R"({"type": "panel", "children": [{"type": "text", "colour": "#fff"}]})");
    GIANT_CHECK(typo.find("children[0]") != std::string::npos && typo.find("colour") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "button"})").find("id") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "panel", "id": "a", "children": [{"type": "text", "id": "a"}]})").find("another") !=
                std::string::npos);
    GIANT_CHECK(refused(R"({"type": "panel", "background": "#ffff"})").find("colour") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "panel", "background": "#gg0000"})").find("colour") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "panel", "anchor": [0.5]})").find("anchor") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "panel", "size": [-1, 2]})").find("size") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "panel", "stretch": [1, 0]})").find("stretch") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "panel", "visible": "!"})").find("visible") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "bar", "value": true})").find("value") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "panel", "children": {}})").find("list") != std::string::npos);
    // Nested too deep.
    std::string deep = R"({"type": "panel"})";
    for (usize i = 0; i < Screen::k_max_depth + 1; ++i) deep = R"({"type": "panel", "children": [)" + deep + "]}";
    GIANT_CHECK(refused(deep).find("deeper") != std::string::npos);
    // Too many.
    std::string wide = R"({"type": "panel", "children": [)";
    for (usize i = 0; i < Screen::k_max_widgets; ++i) wide += std::string(i ? "," : "") + R"({"type": "panel"})";
    GIANT_CHECK(refused(wide + "]}").find("widgets") != std::string::npos);

    // Everything known, read.
    const auto s = load(R"({"type": "panel", "id": "root", "children": [
        {"type": "bar", "id": "hp", "anchor": [0, 1], "pivot": [0, 1], "offset": [20, -20], "size": [300, 20],
         "value": "health", "max": 100, "direction": "up", "color": "#ff000080", "background": "#000000",
         "border": 2, "border_color": "#ffffff", "visible": "!dead", "clip": true},
        {"type": "text", "text": "Ammo {ammo}", "font_size": 32, "align": "right", "wrap": false,
         "outline": 2, "outline_color": "#101010", "shadow": 1, "padding": 4},
        {"type": "image", "image": "map", "stretch": [true, false], "size": [0, 64]},
        {"type": "button", "id": "ok", "text": "OK", "hover": "#334455", "layout": "row", "gap": 3}]})");
    if (s == nullptr) return;
    GIANT_CHECK(s->widgets() == 5);
    const Widget* hp = s->find("hp");
    GIANT_CHECK(hp != nullptr && hp->type == WidgetType::bar && hp->value_name == "health" && hp->max == 100.0);
    GIANT_CHECK(hp->direction == Direction::up && hp->color == rgba(255, 0, 0, 128) && hp->visible == "!dead");
    GIANT_CHECK(hp->anchor.y == 1.0f && hp->pivot.y == 1.0f && hp->offset.x == 20.0f && hp->clip);
    GIANT_CHECK(s->root().children[1].align == Align::right && s->root().children[1].font_size == 32.0f);
    GIANT_CHECK(s->root().children[2].stretch_x && !s->root().children[2].stretch_y);
    GIANT_CHECK(s->find("ok")->hover == rgba(0x33, 0x44, 0x55) && s->find("missing") == nullptr && s->find("") == nullptr);
}

GIANT_TEST("ui screen: laid out by anchors and pivots at any size and scale") {
    const auto s = load(R"({"type": "panel", "stretch": [true, true], "children": [
        {"type": "panel", "id": "tl", "size": [100, 50], "offset": [10, 20]},
        {"type": "panel", "id": "br", "anchor": [1, 1], "size": [100, 50], "offset": [-10, -10]},
        {"type": "panel", "id": "mid", "anchor": [0.5, 0.5], "size": [200, 100]},
        {"type": "panel", "id": "top", "anchor": [0.5, 0], "pivot": [0.5, 1], "size": [40, 40]},
        {"type": "panel", "id": "fill", "stretch": [true, false], "size": [0, 30], "padding": 5,
         "children": [{"type": "panel", "id": "inner", "stretch": [true, true]}]}]})");
    if (s == nullptr) return;
    Values v;
    s->lay_out(default_font(), v, {1920.0f, 1080.0f}, 1.0f);
    const Rect root = s->root().rect;
    GIANT_CHECK(root.x == 0.0f && root.y == 0.0f && root.w == 1920.0f && root.h == 1080.0f);
    const Rect tl = s->find("tl")->rect, br = s->find("br")->rect, mid = s->find("mid")->rect;
    GIANT_CHECK(tl.x == 10.0f && tl.y == 20.0f && tl.w == 100.0f && tl.h == 50.0f);
    GIANT_CHECK(br.x == 1920.0f - 10.0f - 100.0f && br.y == 1080.0f - 10.0f - 50.0f);
    GIANT_CHECK(mid.x == 860.0f && mid.y == 490.0f);
    const Rect top = s->find("top")->rect;
    GIANT_CHECK(top.x == 940.0f && top.y == -40.0f); // above the top: its bottom middle on the top middle
    const Rect fill = s->find("fill")->rect, inner = s->find("inner")->rect;
    GIANT_CHECK(fill.w == 1920.0f && fill.h == 30.0f);
    GIANT_CHECK(inner.x == 5.0f && inner.y == 5.0f && inner.w == 1910.0f && inner.h == 20.0f);

    // At 4K: everything in units scales by height / 1080; anchors follow the size.
    s->lay_out(default_font(), v, {3840.0f, 2160.0f}, 2.0f);
    const Rect tl2 = s->find("tl")->rect, br2 = s->find("br")->rect, mid2 = s->find("mid")->rect;
    GIANT_CHECK(tl2.x == 20.0f && tl2.y == 40.0f && tl2.w == 200.0f && tl2.h == 100.0f);
    GIANT_CHECK(br2.x == 3840.0f - 20.0f - 200.0f && br2.y == 2160.0f - 20.0f - 100.0f);
    GIANT_CHECK(mid2.x == 1720.0f && mid2.y == 980.0f);
    // An ultrawide: the same screen, wider.
    s->lay_out(default_font(), v, {2560.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(s->find("br")->rect.x == 2560.0f - 110.0f && s->find("mid")->rect.x == 1180.0f);
    // A size that is not a size: nothing changes.
    s->lay_out(default_font(), v, {std::nanf(""), 1.0f}, 1.0f);
    s->lay_out(default_font(), v, {100.0f, 100.0f}, 0.0f);
    GIANT_CHECK(s->find("br")->rect.x == 2560.0f - 110.0f);
}

GIANT_TEST("ui screen: rows and columns stack their children with gaps, fit them, and stretch across") {
    const auto s = load(R"({"type": "panel", "stretch": [true, true], "children": [
        {"type": "panel", "id": "col", "layout": "column", "gap": 10, "padding": 8, "offset": [100, 100], "children": [
            {"type": "panel", "id": "a", "size": [100, 20]},
            {"type": "panel", "id": "b", "size": [60, 30], "anchor": [1, 0]},
            {"type": "panel", "id": "hidden", "size": [500, 500], "visible": "never"},
            {"type": "panel", "id": "c", "size": [0, 10], "stretch": [true, false]}]},
        {"type": "panel", "id": "row", "layout": "row", "gap": 4, "anchor": [1, 0], "size": [0, 50], "children": [
            {"type": "panel", "id": "r1", "size": [30, 10]},
            {"type": "panel", "id": "r2", "size": [40, 10], "anchor": [0, 0.5], "pivot": [0, 0.5]},
            {"type": "panel", "id": "r3", "size": [50, 0], "stretch": [false, true]}]}]})");
    if (s == nullptr) return;
    Values v;
    s->lay_out(default_font(), v, {1920.0f, 1080.0f}, 1.0f);
    // The column: as wide as its widest, as tall as all with gaps, padded; the hidden one takes no room.
    const Rect col = s->find("col")->rect;
    GIANT_CHECK(col.x == 100.0f && col.y == 100.0f);
    GIANT_CHECK(col.w == 100.0f + 16.0f && col.h == 20.0f + 10.0f + 30.0f + 10.0f + 10.0f + 16.0f);
    const Rect a = s->find("a")->rect, b = s->find("b")->rect, c = s->find("c")->rect;
    GIANT_CHECK(a.x == 108.0f && a.y == 108.0f);
    GIANT_CHECK(b.y == 108.0f + 20.0f + 10.0f && b.x == 108.0f + 100.0f - 60.0f); // across: at its anchor's x
    GIANT_CHECK(c.y == b.y + 30.0f + 10.0f && c.x == 108.0f && c.w == 100.0f && c.h == 10.0f);
    GIANT_CHECK(!s->find("hidden")->shown);
    // The row: right-anchored, as wide as its children, given its height.
    const Rect row = s->find("row")->rect;
    GIANT_CHECK(row.w == 30.0f + 4.0f + 40.0f + 4.0f + 50.0f && row.h == 50.0f && row.x == 1920.0f - row.w && row.y == 0.0f);
    const Rect r1 = s->find("r1")->rect, r2 = s->find("r2")->rect, r3 = s->find("r3")->rect;
    GIANT_CHECK(r1.x == row.x && r1.y == 0.0f);
    GIANT_CHECK(r2.x == row.x + 34.0f && r2.y == 20.0f); // middled across
    GIANT_CHECK(r3.x == row.x + 78.0f && r3.h == 50.0f);

    // Shown, it takes its room.
    v.set("never", true);
    s->lay_out(default_font(), v, {1920.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(s->find("hidden")->shown && s->find("col")->rect.w == 500.0f + 16.0f);
    GIANT_CHECK(s->find("c")->rect.w == 500.0f);
}

GIANT_TEST("ui screen: text fits what it says, wraps in its width, and follows the values") {
    const auto s = load(R"({"type": "panel", "stretch": [true, true], "children": [
        {"type": "text", "id": "ammo", "text": "Ammo {ammo}", "font_size": 30, "padding": 5},
        {"type": "text", "id": "long", "text": "a long line of words that will need wrapping somewhere", "wrap": true,
         "size": [200, 0], "anchor": [0, 1], "pivot": [0, 1]},
        {"type": "panel", "id": "col", "layout": "column", "anchor": [1, 0], "pivot": [1, 0], "children": [
            {"type": "text", "id": "t1", "text": "One"},
            {"type": "text", "id": "t2", "text": "Three words here"}]}]})");
    if (s == nullptr) return;
    Values v;
    v.set("ammo", 5.0);
    const Font& font = default_font();
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    TextStyle style;
    style.size = 30.0f;
    const Vec2 m5 = measure(font, "Ammo 5", style);
    const Rect ammo = s->find("ammo")->rect;
    GIANT_CHECK(near(ammo.w, m5.x + 10.0f) && near(ammo.h, m5.y + 10.0f));
    v.set("ammo", 120.0);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(s->find("ammo")->rect.w > ammo.w);
    // Wrapped in 200: several lines, its bottom on the bottom.
    const Rect lr = s->find("long")->rect;
    TextStyle one;
    GIANT_CHECK(lr.w == 200.0f && lr.h > 2.5f * measure(font, "a", one).y && near(lr.y + lr.h, 1080.0f));
    // A column of text: as wide as the widest.
    const Rect col = s->find("col")->rect;
    GIANT_CHECK(near(col.w, measure(font, "Three words here", one).x) && near(col.x + col.w, 1920.0f));
    GIANT_CHECK(s->find("t2")->rect.y > s->find("t1")->rect.y);
}

GIANT_TEST("ui screen: drawn as laid out; bars fill by their value, hidden widgets are not drawn") {
    const auto s = load(R"({"type": "panel", "stretch": [true, true], "children": [
        {"type": "bar", "id": "hp", "size": [200, 20], "value": "health", "max": "most", "color": "#ff0000",
         "background": "#000000", "padding": 2},
        {"type": "bar", "id": "up", "size": [20, 100], "offset": [300, 0], "value": 0.25, "direction": "up"},
        {"type": "text", "id": "msg", "text": "Hello", "visible": "talking"},
        {"type": "image", "id": "pic", "image": "map", "size": [64, 64], "offset": [400, 0]}]})");
    if (s == nullptr) return;
    const Font& font = default_font();
    Values v;
    v.set("health", 30.0);
    v.set("most", 120.0);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    Canvas canvas;
    canvas.begin(1920.0f, 1080.0f, font);
    int asked = 0;
    s->draw(canvas, font, v, 1.0f, [&](std::string_view name) -> rhi::TextureId {
        ++asked;
        return name == "map" ? 42u : 0u;
    });
    GIANT_CHECK(asked == 1);
    const rhi::UiDrawList list = canvas.list();
    const auto& cmds = list.commands;
    GIANT_CHECK(!cmds.empty());
    // The hp bar: background, then a quarter of its inside, red.
    const auto& verts = list.vertices;
    bool found_quarter = false, found_up = false, found_image = false;
    for (usize i = 0; i + 3 < verts.size(); ++i) {
        const auto& q = verts[i];
        if (q.color == rgba(255, 0, 0) && near(q.x, 2.0f) && near(q.y, 2.0f)) {
            // The quad's right edge: 2 + 196 * 0.25.
            for (usize k = i; k < std::min(i + 4, verts.size()); ++k) {
                if (near(verts[k].x, 2.0f + 196.0f * 0.25f)) found_quarter = true;
            }
        }
        if (q.color == k_white && near(q.x, 300.0f) && near(q.y, 75.0f)) found_up = true; // up: the fill's top at 3/4 down
    }
    for (const auto& c : cmds) {
        if (c.texture == 42u) found_image = true;
    }
    GIANT_CHECK(found_quarter && found_up && found_image);
    // Hidden text is not drawn: talking shows it, more vertices.
    const usize quiet = canvas.vertices();
    v.set("talking", true);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    canvas.begin(1920.0f, 1080.0f, font);
    s->draw(canvas, font, v, 1.0f);
    GIANT_CHECK(canvas.vertices() > quiet); // (no image without a lookup: 4 fewer; the text's letters more)
    GIANT_CHECK(s->find("msg")->shown);
}

GIANT_TEST("ui screen: buttons pointed at, clicked and moved through with keys") {
    const auto s = load(R"({"type": "panel", "stretch": [true, true], "children": [
        {"type": "panel", "layout": "column", "gap": 10, "anchor": [0.5, 0.5], "children": [
            {"type": "button", "id": "resume", "text": "Resume", "size": [300, 50]},
            {"type": "button", "id": "save", "text": "Save", "size": [300, 50], "visible": "can_save"},
            {"type": "button", "id": "quit", "text": "Quit", "size": [300, 50]}]},
        {"type": "panel", "id": "clipper", "clip": true, "size": [100, 100], "children": [
            {"type": "button", "id": "half", "size": [200, 50]}]},
        {"type": "button", "id": "over", "size": [50, 50], "offset": [0, 200]},
        {"type": "button", "id": "top", "size": [50, 50], "offset": [25, 200]}]})");
    if (s == nullptr) return;
    const Font& font = default_font();
    Values v;
    v.set("can_save", true);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    const Rect resume = s->find("resume")->rect, save = s->find("save")->rect;
    GIANT_CHECK(resume.w == 300.0f && save.y == resume.y + 60.0f);
    const Vec2 in_save{save.x + 10.0f, save.y + 10.0f};
    GIANT_CHECK(s->button_at(in_save) == s->find("save"));
    GIANT_CHECK(s->button_at({5.0f, 900.0f}) == nullptr);
    GIANT_CHECK(s->click({5.0f, 900.0f}) == std::nullopt);
    GIANT_CHECK(s->click(in_save) == std::optional<std::string>("save") && s->focused() == "save");
    // Clipped out: not clicked outside its clipping parent.
    GIANT_CHECK(s->button_at({50.0f, 10.0f}) == s->find("half"));
    GIANT_CHECK(s->button_at({150.0f, 10.0f}) == nullptr);
    // Overlapping: the one drawn last (on top).
    GIANT_CHECK(s->button_at({40.0f, 210.0f}) == s->find("top"));
    s->point({10.0f, 210.0f});
    GIANT_CHECK(s->focused() == "over");

    // Keys: in the order written, round the ends; hidden ones skipped.
    s->move_focus(+1);
    GIANT_CHECK(s->focused() == "top");
    s->move_focus(+1);
    GIANT_CHECK(s->focused() == "resume");
    GIANT_CHECK(s->activate() == std::optional<std::string>("resume"));
    s->move_focus(-1);
    GIANT_CHECK(s->focused() == "top");
    v.set("can_save", false);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    s->move_focus(+1);
    s->move_focus(+1);
    GIANT_CHECK(s->focused() == "quit"); // resume, then (save hidden) quit
    // The focused one hidden: the focus is let go of.
    s->move_focus(-1);
    s->move_focus(-1);
    GIANT_CHECK(s->focused() == "top");
    s->move_focus(+1);
    s->move_focus(+1);
    GIANT_CHECK(s->focused() == "quit");
    v.set("can_save", true);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    s->move_focus(-1);
    GIANT_CHECK(s->focused() == "save");
    v.set("can_save", false);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(s->focused().empty() && s->activate() == std::nullopt);
    s->move_focus(-1);
    GIANT_CHECK(s->focused() == "top"); // none focused, going back: the last
    s->move_focus(0);
    GIANT_CHECK(s->focused() == "top");
}

GIANT_TEST("ui screen: what a hidden widget holds is hidden too: not clicked, focused or activated") {
    const auto s = load(R"({"type": "panel", "stretch": [true, true], "children": [
        {"type": "panel", "id": "menu", "visible": "menu_open", "layout": "column", "children": [
            {"type": "button", "id": "a", "size": [100, 40]},
            {"type": "button", "id": "b", "size": [100, 40]}]},
        {"type": "button", "id": "c", "size": [100, 40], "anchor": [1, 1], "pivot": [1, 1]}]})");
    if (s == nullptr) return;
    const Font& font = default_font();
    Values v;
    v.set("menu_open", true);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    s->focus("b");
    GIANT_CHECK(s->focused() == "b" && s->shown("b") && s->button_at({10.0f, 10.0f}) == s->find("a"));
    v.set("menu_open", false);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(!s->shown("a") && !s->shown("b") && s->shown("c") && !s->shown("nothing"));
    GIANT_CHECK(s->focused().empty() && s->activate() == std::nullopt && s->button_at({10.0f, 10.0f}) == nullptr);
    s->move_focus(+1);
    GIANT_CHECK(s->focused() == "c");
    s->move_focus(+1);
    GIANT_CHECK(s->focused() == "c"); // the only one shown
    s->focus("menu"); // not a button
    GIANT_CHECK(s->focused().empty());
}

GIANT_TEST("ui screen: laid out again only when something changed; text cut to a budget; big numbers right") {
    const Font& font = default_font();
    // The root fills the screen, stretched or not.
    const auto s = load(R"({"type": "panel", "children": [
        {"type": "text", "id": "t", "text": "{word}", "anchor": [1, 1], "pivot": [1, 1]},
        {"type": "panel", "id": "cover", "size": [100, 100], "background": "#00000080"},
        {"type": "panel", "id": "wide", "size": [100, 0], "stretch": [true, false], "layout": "column", "offset": [0, 200],
         "children": [{"type": "text", "id": "w", "wrap": true, "text": "a few words that fit across a whole screen"}]}]})");
    if (s == nullptr) return;
    Values v;
    v.set("word", std::string("Hello"));
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    const Rect t = s->find("t")->rect;
    GIANT_CHECK(near(t.x + t.w, 1920.0f) && near(t.y + t.h, 1080.0f) && s->text_bytes() == 6 + 42);
    // Stretched with a size: what it holds wraps at the width it is (one line here).
    TextStyle one;
    GIANT_CHECK(s->find("wide")->rect.w == 1920.0f && near(s->find("w")->rect.h, measure(font, "a", one).y));
    // Unchanged: not laid out again (a rect moved by hand stays).
    const u64 g = v.generation();
    v.set("word", std::string("Hello"));
    GIANT_CHECK(v.generation() == g);
    const_cast<Widget*>(s->find("t"))->rect.x = -5.0f;
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(s->find("t")->rect.x == -5.0f);
    v.set("word", std::string("Hello there"));
    GIANT_CHECK(v.generation() != g);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(s->find("t")->rect.x > 0.0f && s->find("t")->shown_text == "Hello there");
    // A budget shared by screens: what is left is shown (cut at a letter), and counted down.
    v.set("word", std::string("Grüße"));
    usize budget = 3;
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f, &budget);
    GIANT_CHECK(s->find("t")->shown_text == "Gr" && s->find("w")->shown_text.empty() && budget == 0);
    usize plenty = 1000;
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f, &plenty);
    GIANT_CHECK(plenty == 1000 - 7 - 42);
    usize again = 1000;
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f, &again); // (unchanged: still counted)
    GIANT_CHECK(again == plenty);
    // A filled-in text is at most 4096 bytes.
    v.set("word", std::string(4000, 'x'));
    const auto many = load(R"({"type": "text", "id": "m", "text": "{word}{word}{word}"})");
    if (many != nullptr) {
        many->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
        GIANT_CHECK(many->find("m")->shown_text.size() == 4096);
    }
    // A template that makes nothing still costs its work: the budget runs out.
    const auto empty = load(R"({"type": "panel", "children": [{"type": "text", "id": "a", "text": "{}{}{}{}"},
        {"type": "text", "id": "b", "text": "x"}]})");
    if (empty != nullptr) {
        usize small = 8;
        empty->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f, &small);
        GIANT_CHECK(small == 0 && empty->find("b")->shown_text.empty());
    }
    // Another Values in its place: laid out again (generations are never shared).
    {
        Values other;
        other.set("word", std::string("Other"));
        s->lay_out(font, other, {1920.0f, 1080.0f}, 1.0f);
        GIANT_CHECK(s->find("t")->shown_text == "Other");
        other = Values{};
        other.set("word", std::string("Third"));
        s->lay_out(font, other, {1920.0f, 1080.0f}, 1.0f);
        GIANT_CHECK(s->find("t")->shown_text == "Third");
    }
    // The root fills the screen wherever it is written to be.
    const auto moved = load(R"({"type": "panel", "anchor": [0.5, 0.5], "pivot": [0, 0], "offset": [10, 20], "size": [100, 100]})");
    if (moved != nullptr) {
        moved->lay_out(font, v, {800.0f, 600.0f}, 1.0f);
        const Rect r = moved->root().rect;
        GIANT_CHECK(r.x == 0.0f && r.y == 0.0f && r.w == 800.0f && r.h == 600.0f);
    }
    // Covered: a background there.
    GIANT_CHECK(s->covers({50.0f, 50.0f}) && !s->covers({150.0f, 50.0f}));
    // Big numbers: all their digits.
    Values big;
    big.set("n", 1.5e70);
    big.set("m", -1e300);
    GIANT_CHECK(big.text("n").size() == 71 && big.text("n")[0] == '1');
    GIANT_CHECK(big.text("m").size() == 302 && big.text("m", 2).size() == 305);
    // Many children: refused before they are made.
    std::string zeros = R"({"type": "panel", "children": [)";
    for (int i = 0; i < 100000; ++i) zeros += i ? ",0" : "0";
    GIANT_CHECK(refused(zeros + "]}").find("widgets") != std::string::npos);
    GIANT_CHECK(refused(R"({"type": "panel", "visible": ")" + std::string(129, 'x') + R"("})").find("visible") != std::string::npos);
}
