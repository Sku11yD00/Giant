// HUD pieces that need the world: projection, headings and bearings,
// markers at their place or at the edge, compasses, fades.
#include "giant/ui/hud.h"
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

bool near(float a, float b, float eps = 0.05f) { return std::fabs(a - b) <= eps; }

Quat yaw(float degrees) { return quat_from_axis_angle({0.0f, 1.0f, 0.0f}, degrees_to_radians(degrees)); }

} // namespace

GIANT_TEST("ui hud: headings and bearings clockwise from north; points projected onto the screen") {
    GIANT_CHECK(near(heading_degrees(Quat{}), 0.0f));      // looks down -Z: north
    GIANT_CHECK(near(heading_degrees(yaw(90.0f)), 270.0f)); // yaw 90 looks west
    GIANT_CHECK(near(heading_degrees(yaw(-90.0f)), 90.0f)); // east
    GIANT_CHECK(near(heading_degrees(yaw(180.0f)), 180.0f));
    const DVec3 o{1000.0, 5.0, -2000.0};
    GIANT_CHECK(near(bearing_degrees(o, o + DVec3{0, 0, -10}), 0.0f));
    GIANT_CHECK(near(bearing_degrees(o, o + DVec3{10, 3, 0}), 90.0f));
    GIANT_CHECK(near(bearing_degrees(o, o + DVec3{0, 0, 10}), 180.0f));
    GIANT_CHECK(near(bearing_degrees(o, o + DVec3{-10, 0, 0}), 270.0f));
    GIANT_CHECK(near(bearing_degrees(o, o), 0.0f));
    GIANT_CHECK(near(turn_degrees(350.0f, 10.0f), 20.0f) && near(turn_degrees(10.0f, 350.0f), -20.0f));
    GIANT_CHECK(near(turn_degrees(0.0f, 180.0f), 180.0f) && near(turn_degrees(90.0f, 90.0f), 0.0f));

    View view;
    view.position = {50000.0, 100.0, 50000.0}; // (far out: doubles)
    view.vertical_fov_degrees = 90.0f;
    const Vec2 size{1920.0f, 1080.0f};
    Projected p = project(view, size, view.position + DVec3{0, 0, -10});
    GIANT_CHECK(p.in_front && near(p.at.x, 960.0f) && near(p.at.y, 540.0f) && near(p.depth, 10.0f, 0.01f));
    // 45 degrees up at a 90 degree view: the top edge.
    p = project(view, size, view.position + DVec3{0, 10, -10});
    GIANT_CHECK(p.in_front && near(p.at.y, 0.0f, 0.5f));
    p = project(view, size, view.position + DVec3{0, 0, 10});
    GIANT_CHECK(!p.in_front);
    // Turned west: what is west is ahead.
    view.orientation = yaw(90.0f);
    p = project(view, size, view.position + DVec3{-10, 0, 0});
    GIANT_CHECK(p.in_front && near(p.at.x, 960.0f, 0.5f));
    GIANT_CHECK(!project(view, {0.0f, 0.0f}, view.position + DVec3{-10, 0, 0}).in_front);
}

GIANT_TEST("ui hud: markers at their place, at the screen's edge pointing the way, faded by distance") {
    const Font& font = default_font();
    Markers markers;
    Marker ahead;
    ahead.id = "camp";
    ahead.label = "Camp";
    ahead.position = {0.0, 0.0, -100.0};
    GIANT_CHECK(markers.set(ahead));
    Marker behind = ahead;
    behind.id = "home";
    behind.position = {0.0, 0.0, 50.0};
    GIANT_CHECK(markers.set(behind));
    Marker right = ahead;
    right.id = "east";
    right.position = {100.0, 0.0, -1.0};
    right.clamp = false;
    GIANT_CHECK(markers.set(right));
    Marker far = ahead;
    far.id = "far";
    far.position = {0.0, 0.0, -1000.0};
    far.max_distance = 500.0;
    far.compass = false;
    GIANT_CHECK(markers.set(far));
    GIANT_CHECK(!markers.set(Marker{}) && markers.size() == 4);
    Marker nan = ahead;
    nan.id = "nan";
    nan.position.x = std::nan("");
    GIANT_CHECK(!markers.set(nan));

    View view;
    view.position = {0.0, 1.7, 0.0};
    Canvas canvas;
    canvas.begin(1920.0f, 1080.0f, font);
    markers.draw(canvas, font, view, {1920.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(canvas.vertices() > 0 && markers.placed().size() == 4);
    const auto placed = [&](const char* id) {
        for (const PlacedMarker& p : markers.placed()) {
            if (p.id == id) return p;
        }
        return PlacedMarker{};
    };
    const PlacedMarker camp = placed("camp"), home = placed("home"), east = placed("east"), fp = placed("far");
    GIANT_CHECK(camp.shown && camp.on_screen && near(camp.at.x, 960.0f, 1.0f) && near(static_cast<float>(camp.distance), 100.0f, 0.1f));
    // Behind: at the bottom edge (48 units in), not on the screen.
    GIANT_CHECK(home.shown && !home.on_screen && near(home.at.y, 1080.0f - 48.0f, 1.0f) && near(home.at.x, 960.0f, 1.0f));
    GIANT_CHECK(near(home.bearing, 180.0f));
    // Off to the right and not clamped: not shown; past its distance: not shown.
    GIANT_CHECK(!east.shown && !east.on_screen && near(east.bearing, 90.0f, 1.0f));
    GIANT_CHECK(!fp.shown);
    // Clamped to the right edge when it is.
    right.clamp = true;
    GIANT_CHECK(markers.set(right));
    canvas.begin(1920.0f, 1080.0f, font);
    markers.draw(canvas, font, view, {1920.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(placed("east").shown && near(placed("east").at.x, 1920.0f - 48.0f, 1.0f));
    // The compass marks: those that go on one (not "far").
    GIANT_CHECK(markers.compass_marks().size() == 3);
    GIANT_CHECK(markers.erase("east") && !markers.erase("east") && markers.find("east") == nullptr);
    // At most k_max.
    for (usize i = 0; i < Markers::k_max; ++i) {
        Marker m = ahead;
        m.id = "m" + std::to_string(i);
        markers.set(m);
    }
    GIANT_CHECK(markers.size() == Markers::k_max);
    GIANT_CHECK(markers.set(ahead)); // (replacing one is not one more)
}

GIANT_TEST("ui screen: a compass shows the heading, its letters and markers; widgets fade in and out") {
    const Font& font = default_font();
    std::string error;
    const auto s = Screen::load(R"({"type": "panel", "children": [
        {"type": "compass", "id": "c", "value": "heading", "span": 90, "size": [600, 40], "anchor": [0.5, 0],
         "color": "#ffffff", "font_size": 20},
        {"type": "text", "id": "prompt", "text": "[E] Open", "visible": "near_door", "fade": 0.5,
         "anchor": [0.5, 0.7], "background": "#000000ff"}]})",
                                error);
    GIANT_CHECK(s != nullptr && error.empty());
    if (s == nullptr) return;
    GIANT_CHECK(Screen::load(R"({"type": "compass", "span": 5})", error) == nullptr && error.find("span") != std::string::npos);
    GIANT_CHECK(Screen::load(R"({"type": "panel", "fade": 20})", error) == nullptr && error.find("fade") != std::string::npos);
    Values v;
    v.set("heading", 90.0);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    const Rect c = s->find("c")->rect;
    GIANT_CHECK(c.w == 600.0f && near(c.x, 660.0f));
    // Facing east with a mark due east (in the middle) and one due west (past it: at an edge).
    const CompassMark marks[2] = {{90.0f, rgba(255, 0, 0)}, {270.0f, rgba(0, 255, 0)}};
    Canvas canvas;
    canvas.begin(1920.0f, 1080.0f, font);
    s->advance(0.0);
    s->draw(canvas, font, v, 1.0f, {}, marks);
    const rhi::UiDrawList list = canvas.list();
    float red_x = -1.0f, green_x = -1.0f;
    for (const rhi::UiVertex& q : list.vertices) {
        if (q.color == rgba(255, 0, 0)) red_x = std::max(red_x, q.x);
        if ((q.color & 0xFFFFFFu) == (rgba(0, 255, 0) & 0xFFFFFFu)) green_x = std::max(green_x, q.x);
    }
    GIANT_CHECK(red_x > 955.0f && red_x < 970.0f);
    GIANT_CHECK(green_x >= c.x + c.w - 1.0f || green_x <= c.x + 20.0f); // at an edge
    const usize compass_only = canvas.vertices();

    // The prompt: fades in over half a second, out the same, and is not drawn once gone.
    const auto prompt_alpha = [&]() {
        canvas.begin(1920.0f, 1080.0f, font);
        s->draw(canvas, font, v, 1.0f, {}, marks);
        int best = -1;
        for (const rhi::UiVertex& q : canvas.list().vertices) {
            if ((q.color & 0xFFFFFFu) == 0) best = std::max(best, static_cast<int>(q.color >> 24));
        }
        return best;
    };
    v.set("near_door", true);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    s->advance(0.0);
    GIANT_CHECK(prompt_alpha() == -1 || prompt_alpha() == 0);
    s->advance(0.25);
    GIANT_CHECK(std::abs(prompt_alpha() - 128) <= 2);
    s->advance(0.5);
    GIANT_CHECK(prompt_alpha() == 255 && s->shown("prompt"));
    v.set("near_door", false);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    GIANT_CHECK(!s->shown("prompt")); // not shown (nor clicked): only drawn, fading
    s->advance(0.25);
    GIANT_CHECK(std::abs(prompt_alpha() - 128) <= 2 && s->find("prompt")->rect.w > 0.0f);
    s->advance(0.3);
    GIANT_CHECK(prompt_alpha() == -1);
    canvas.begin(1920.0f, 1080.0f, font);
    s->draw(canvas, font, v, 1.0f, {}, marks);
    GIANT_CHECK(canvas.vertices() == compass_only);
}

GIANT_TEST("ui screen: laid out again only for the values its layout reads; bars and compasses read theirs as drawn") {
    const Font& font = default_font();
    std::string error;
    const auto s = Screen::load(R"({"type": "panel", "children": [
        {"type": "compass", "id": "c", "value": "heading", "size": [600, 40]},
        {"type": "bar", "id": "b", "value": "hp", "size": [100, 10]},
        {"type": "text", "id": "t", "text": "{ammo}", "visible": "!dead"}]})",
                                error);
    GIANT_CHECK(s != nullptr);
    if (s == nullptr) return;
    Values v;
    v.set("ammo", 3.0);
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
    const auto moved = [&]() {
        const_cast<Widget*>(s->find("t"))->rect.x = -7.0f;
        s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f);
        return s->find("t")->rect.x != -7.0f; // (laid out again: put back)
    };
    v.set("heading", 12.5);
    GIANT_CHECK(!moved());
    v.set("hp", 0.5);
    v.set("unrelated", true);
    GIANT_CHECK(!moved());
    v.set("ammo", 4.0);
    GIANT_CHECK(moved());
    v.set("dead", false); // (set, though it reads the same: a value it reads)
    GIANT_CHECK(moved());
    v.erase("dead");
    GIANT_CHECK(moved());
    GIANT_CHECK(!moved());
    v.clear();
    GIANT_CHECK(moved() && v.generation("ammo") == 0);
}

GIANT_TEST("ui hud: fading widgets keep counting their text and taking clicks; markers stay finite at any size") {
    const Font& font = default_font();
    std::string error;
    const auto s = Screen::load(R"({"type": "panel", "children": [
        {"type": "text", "id": "a", "text": "{t}", "visible": "a", "fade": 1, "background": "#000000ff", "size": [200, 50]},
        {"type": "text", "id": "b", "text": "{t}", "visible": "!a", "fade": 1},
        {"type": "button", "id": "go", "size": [50, 50], "anchor": [1, 1], "pivot": [1, 1], "fade": 1}]})",
                                error);
    GIANT_CHECK(s != nullptr);
    if (s == nullptr) return;
    Values v;
    v.set("t", std::string(3000, 'x'));
    v.set("a", true);
    usize budget = 4000;
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f, &budget);
    s->advance(0.4);
    GIANT_CHECK(s->button_at({1900.0f, 1060.0f}) == nullptr); // fading in: not yet half way
    s->advance(0.2);
    GIANT_CHECK(s->button_at({1900.0f, 1060.0f}) == s->find("go"));
    s->advance(1.0);
    // "a" goes, "b" comes: "a" fading out still holds its 3000 bytes, so "b" gets what is left.
    v.set("a", false);
    budget = 4000;
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f, &budget);
    GIANT_CHECK(s->find("a")->shown_text.size() == 3000 && s->find("b")->shown_text.size() == 1000 && budget == 0);
    GIANT_CHECK(!s->shown("a") && s->covers({100.0f, 25.0f})); // drawn: takes the click
    s->advance(1.0);
    budget = 4000;
    s->lay_out(font, v, {1920.0f, 1080.0f}, 1.0f, &budget);
    GIANT_CHECK(!s->covers({100.0f, 25.0f}) && s->find("a")->shown_text.empty());

    // A marker ahead on a screen too small for the edge inset; out-of-range positions refused.
    Markers markers;
    Marker m;
    m.id = "x";
    m.position = {0.0, 0.0, -10.0};
    GIANT_CHECK(markers.set(m));
    m.id = "huge";
    m.position = {1e300, 0.0, 0.0};
    GIANT_CHECK(!markers.set(m));
    Canvas canvas;
    canvas.begin(80.0f, 80.0f, font);
    View view;
    markers.draw(canvas, font, view, {80.0f, 80.0f}, 1.0f);
    GIANT_CHECK(markers.placed().size() == 1 && std::isfinite(markers.placed()[0].at.x) && std::isfinite(markers.placed()[0].at.y));
    view.position.x = std::nan("");
    markers.draw(canvas, font, view, {80.0f, 80.0f}, 1.0f);
    GIANT_CHECK(markers.placed().empty());
}
