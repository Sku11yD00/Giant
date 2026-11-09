// Screens from data: a tree of widgets loaded from JSON (panels, text,
// bars, images, buttons), laid out by anchors or in rows and columns, at
// any window size, filled in from named values the game sets (the
// text "Ammo {ammo}", a bar's "value": "health"), and clicked or moved
// through with keys. The AI writes the JSON; nothing here is code a
// designer must write.
//
// Units are UI units: pixels at 1080 lines high (times `scale`). The root
// fills the screen (its anchor, pivot, offset, size and stretch are not used). A wrapped text
// wraps at its own width if it has one, else its parent's inside: in a
// row, give it a size. Filled-in text is at most 4096 bytes a widget and
// k_max_text_bytes a screen (the rest not shown).
//
// JSON (every key optional but "type"; unknown keys are refused, so a typo
// is caught):
//   {"type": "panel" | "text" | "bar" | "image" | "button",
//    "id": "name",                 // to find it, and what a button's click says (one widget each)
//    "anchor": [x, y],             // where in its parent (0 to 1: [0,0] top left, [1,1] bottom right)
//    "pivot": [x, y],              // which of its own points sits there (default: the anchor)
//    "offset": [x, y],             // moved by this many units
//    "size": [w, h],               // units; 0: fit what it holds (text, a row, a column)
//    "stretch": [bool, bool],      // as wide / as tall as its parent's inside
//    "layout": "none" | "column" | "row", "gap": units, "padding": units,
//    "clip": bool,                 // its children drawn only inside it
//    "visible": "name" | "!name",  // shown while the value is true, non-zero or non-empty (or not)
//    "background": "#rrggbb[aa]", "border": units, "border_color": "#...",
//    "color": "#...",              // text, a bar's fill, an image's tint
//    "text": "Ammo {ammo}",        // {name} or {name:2} (two decimals) from the values
//    "font_size": units, "align": "left" | "center" | "right", "wrap": bool,
//    "outline": units, "outline_color": "#...", "shadow": units,
//    "value": "name" | number, "max": "name" | number,   // a bar's fill
//    "direction": "right" | "left" | "up" | "down",      // a bar's
//    "image": "name",              // an image the game has given the UI
//    "hover": "#...",              // a button's background when pointed at or focused
//    "children": [ ... ]}
#pragma once

#include "giant/ui/canvas.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace giant::ui {

// What the game tells the UI: numbers, text and flags by name.
using Value = std::variant<double, std::string, bool>;

class Values {
public:
    Values();
    void set(const std::string& name, Value value);
    void erase(const std::string& name);
    void clear();
    // Changes each time a value may have (screens lay out again only then);
    // unique among all Values (a copy keeps its source's).
    u64 generation() const { return m_generation; }
    const Value* find(std::string_view name) const;
    double number(std::string_view name, double otherwise = 0.0) const; // a flag as 0 or 1; text read as a number if it is one
    bool truthy(std::string_view name) const;                          // true, non-zero, non-empty
    // As text: numbers with `decimals` places (-1: as few as show it, at most 3).
    std::string text(std::string_view name, int decimals = -1) const;
    const std::map<std::string, Value, std::less<>>& all() const { return m_values; }
    usize size() const { return m_values.size(); }
    static constexpr usize k_max_values = 4096;

private:
    std::map<std::string, Value, std::less<>> m_values;
    u64 m_generation = 0;
};

// "{name}" and "{name:2}" filled in from `values` ("{{" is a brace); an
// unknown name as empty.
std::string fill_text(std::string_view text, const Values& values);

enum class WidgetType : u8 { panel, text, bar, image, button };
enum class Layout : u8 { none, column, row };
enum class Direction : u8 { right, left, up, down };

struct Widget {
    WidgetType type = WidgetType::panel;
    std::string id;
    Vec2 anchor{}, pivot{}, offset{}, size{};
    bool stretch_x = false, stretch_y = false;
    Layout layout = Layout::none;
    float gap = 0.0f, padding = 0.0f;
    bool clip = false;
    std::string visible; // a value's name ("!name": while it is not)
    u32 background = 0, border_color = 0, color = k_white, hover = 0, outline_color = k_black;
    float border = 0.0f;
    std::string text;
    float font_size = 24.0f;
    Align align = Align::left;
    bool wrap = false;
    float outline = 0.0f, shadow = 0.0f;
    std::string value_name, max_name; // a bar's (empty: the number)
    double value = 0.0, max = 1.0;
    Direction direction = Direction::right;
    std::string image;
    std::vector<Widget> children;

    // Laid out (pixels), whether it is shown (not before it is first laid
    // out), and its text as filled in.
    Rect rect;
    bool shown = false;
    std::string shown_text;
};

// A click, a key's activation: which button.
struct UiEvent {
    std::string screen;
    std::string id;
};

class Screen {
public:
    // Null (and `error` set, saying where) if the JSON is not a screen.
    static std::unique_ptr<Screen> load(std::string_view json, std::string& error);

    // Laid out over `size` pixels (a frame's: the values read, text filled
    // in and measured), only if the values, size or scale changed since it
    // last was (or `font`). Its text, filled in, is cut to `text_budget`
    // bytes in all (counted down by each text's template or what it made,
    // the larger: share one budget across screens a frame).
    void lay_out(const Font& font, const Values& values, Vec2 size, float scale, usize* text_budget = nullptr);
    // Bytes of text work as last laid out (each text's template or what it made, the larger).
    usize text_bytes() const { return m_text_bytes; }
    // At most this much text, filled in, a screen (or all a frame shares).
    static constexpr usize k_max_text_bytes = 64 * 1024;
    // Drawn as laid out; `images` names to textures (none: no images drawn).
    void draw(Canvas& canvas, const Font& font, const Values& values, float scale,
              const std::function<rhi::TextureId(std::string_view)>& images = {}) const;

    // The top button shown at `point` (pixels, as laid out), if any.
    const Widget* button_at(Vec2 point) const;
    // Whether anything it shows with a background is at `point` (a click
    // there is not for the screens under it).
    bool covers(Vec2 point) const;
    // Pointed at: that button is hovered (and focused). A click says which was clicked.
    void point(Vec2 point);
    std::optional<std::string> click(Vec2 point);
    // Keys or a pad: the focus moved to the next (+1) or previous (-1) button
    // shown, in the order they are written; activating says the focused one.
    void move_focus(int step);
    std::optional<std::string> activate() const;
    const std::string& focused() const { return m_focus; }
    // The focus on that button (none: if it is not one, or not shown as last laid out).
    void focus(std::string id);
    // Shown as last laid out (it and all that hold it).
    bool shown(std::string_view id) const;

    const Widget& root() const { return m_root; }
    const Widget* find(std::string_view id) const;
    usize widgets() const { return m_count; }

    static constexpr usize k_max_widgets = 4096;
    static constexpr usize k_max_depth = 32;

private:
    Widget m_root;
    usize m_count = 0;
    usize m_text_bytes = 0;
    // As last laid out (so it is not again while nothing changed).
    const Font* m_font = nullptr;
    const Values* m_values = nullptr;
    u64 m_generation = 0;
    Vec2 m_size{-1.0f, -1.0f};
    float m_scale = 0.0f;
    usize m_budget = 0;
    std::string m_focus; // a button's id
};

} // namespace giant::ui
