#include "game_ui.h"

#include "editor_commands.h"
#include "game_saves.h"
#include "play_state.h"

#include "giant/core/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace giant::editor {

namespace {

constexpr float k_lines = 1080.0f; // UI units down a screen

} // namespace

const char* GameUi::to_string(Show show) {
    switch (show) {
    case Show::playing: return "playing";
    case Show::always: return "always";
    case Show::never: return "never";
    }
    return "playing";
}

GameUi::GameUi(rhi::Device* device) : m_device(device) {
    m_font = ui::Font::load_default(m_error);
    if (m_font == nullptr) {
        log_error("editor", "the game UI's font: " + m_error);
        return;
    }
    if (m_device == nullptr) return;
    rhi::TextureDesc desc;
    desc.width = m_font->atlas_width();
    desc.height = m_font->atlas_height();
    desc.format = rhi::Format::rgba8_unorm; // (its alpha is a distance, not a colour)
    desc.usage = rhi::TextureUsage::sampled;
    desc.name = "game UI font atlas";
    m_atlas = m_device->create_texture(desc, m_font->atlas());
    if (!m_atlas.valid()) {
        m_error = "could not make the font's texture";
        log_error("editor", "the game UI: " + m_error);
        return;
    }
    m_font->texture = rhi::ui_texture_id(m_atlas);
}

GameUi::~GameUi() {
    if (m_device != nullptr && m_atlas.valid()) m_device->destroy(m_atlas);
}

void GameUi::toast(std::string text, double seconds) {
    if (text.size() > k_max_toast_bytes) {
        usize n = k_max_toast_bytes;
        while (n > 0 && (static_cast<u8>(text[n]) & 0xC0) == 0x80) --n; // (not inside a letter)
        text.resize(n);
    }
    if (!std::isfinite(seconds)) seconds = 3.0;
    while (m_toasts.size() >= k_max_toasts) m_toasts.pop_front();
    m_toasts.push_back({std::move(text), std::clamp(seconds, 0.1, 30.0)});
}

bool GameUi::load_screen(const std::string& name, std::string_view json, int layer, std::string& error) {
    error.clear();
    if (name.empty() || name.size() > 64) {
        error = "a screen's name is 1 to 64 bytes";
        return false;
    }
    if (layer < -k_max_layer || layer > k_max_layer) {
        error = "a layer is from -" + std::to_string(k_max_layer) + " to " + std::to_string(k_max_layer);
        return false;
    }
    std::unique_ptr<ui::Screen> loaded = ui::Screen::load(json, error);
    if (loaded == nullptr) return false;
    const auto it = m_screens.find(name);
    if (it == m_screens.end() && m_screens.size() >= k_max_screens) {
        error = "at most " + std::to_string(k_max_screens) + " screens";
        return false;
    }
    const usize others = screen_widgets() - (it != m_screens.end() ? it->second.screen->widgets() : 0);
    if (others + loaded->widgets() > k_max_widgets) {
        error = "at most " + std::to_string(k_max_widgets) + " widgets on all screens";
        return false;
    }
    // Laid out now for the last frame's size, so it is clicked (and hidden) as it will be drawn.
    if (m_font != nullptr && m_width > 0.0f && m_height > 0.0f) loaded->lay_out(*m_font, values, {m_width, m_height}, m_scale);
    if (it != m_screens.end()) {
        // Replaced: the focus kept where its button still is (shown or not, laid out next frame).
        const std::string focus = it->second.screen->focused();
        if (!focus.empty() && loaded->find(focus) != nullptr && loaded->find(focus)->type == ui::WidgetType::button)
            loaded->focus(focus);
        it->second = {std::move(loaded), layer};
    } else {
        m_screens.emplace(name, Loaded{std::move(loaded), layer});
    }
    reorder();
    return true;
}

bool GameUi::remove_screen(const std::string& name) {
    if (m_screens.erase(name) == 0) return false;
    reorder();
    return true;
}

const ui::Screen* GameUi::screen(std::string_view name) const {
    const auto it = m_screens.find(name);
    return it != m_screens.end() ? it->second.screen.get() : nullptr;
}

ui::Screen* GameUi::find_screen(std::string_view name) {
    const auto it = m_screens.find(name);
    return it != m_screens.end() ? it->second.screen.get() : nullptr;
}

int GameUi::layer(std::string_view name) const {
    const auto it = m_screens.find(name);
    return it != m_screens.end() ? it->second.layer : 0;
}

void GameUi::reorder() {
    m_order.clear();
    for (auto& [name, loaded] : m_screens) m_order.emplace_back(name, &loaded);
    std::stable_sort(m_order.begin(), m_order.end(), [](const auto& a, const auto& b) { return a.second->layer < b.second->layer; });
}

void GameUi::clear_screens() {
    m_screens.clear();
    m_order.clear();
}

std::vector<std::string> GameUi::screen_names() const {
    std::vector<std::string> out;
    for (const auto& entry : m_order) out.push_back(entry.first);
    return out;
}

usize GameUi::screen_widgets() const {
    usize n = 0;
    for (const auto& [name, loaded] : m_screens) n += loaded.screen->widgets();
    return n;
}

void GameUi::set_image(const std::string& name, rhi::TextureId texture) {
    if (name.empty() || name.size() > 128) return;
    if (texture == 0) {
        m_images.erase(name);
    } else if (m_images.size() < 1024 || m_images.contains(name)) {
        m_images[name] = texture;
    }
}

void GameUi::queue(const ui::UiEvent& e) {
    while (m_events.size() >= k_max_events) m_events.pop_front();
    m_events.push_back(e);
}

std::optional<ui::UiEvent> GameUi::click(ui::Vec2 p) {
    if (!m_shown) return std::nullopt;
    for (auto it = m_order.rbegin(); it != m_order.rend(); ++it) {
        if (const std::optional<std::string> id = it->second->screen->click(p)) {
            const ui::UiEvent e{it->first, *id};
            queue(e);
            return e;
        }
        if (it->second->screen->covers(p)) break; // (over the screens under it)
    }
    return std::nullopt;
}

void GameUi::point(ui::Vec2 p) {
    if (!m_shown) return;
    for (auto it = m_order.rbegin(); it != m_order.rend(); ++it) {
        if (it->second->screen->button_at(p) != nullptr) {
            it->second->screen->point(p);
            return;
        }
        if (it->second->screen->covers(p)) return;
    }
}

bool GameUi::move_focus(std::string_view name, int step) {
    ui::Screen* s = find_screen(name);
    if (s == nullptr) return false;
    s->move_focus(step);
    return true;
}

std::optional<ui::UiEvent> GameUi::activate(std::string_view name) {
    ui::Screen* s = find_screen(name);
    if (s == nullptr) return std::nullopt;
    const std::optional<std::string> id = s->activate();
    if (!id) return std::nullopt;
    const ui::UiEvent e{std::string(name), *id};
    queue(e);
    return e;
}

std::optional<ui::UiEvent> GameUi::press(std::string_view name, std::string_view id) {
    ui::Screen* s = find_screen(name);
    if (s == nullptr) return std::nullopt;
    const ui::Widget* w = s->find(id);
    if (w == nullptr || w->type != ui::WidgetType::button || !s->shown(id)) return std::nullopt;
    s->focus(std::string(id));
    const ui::UiEvent e{std::string(name), std::string(id)};
    queue(e);
    return e;
}

std::vector<ui::UiEvent> GameUi::take_events() {
    std::vector<ui::UiEvent> out(m_events.begin(), m_events.end());
    m_events.clear();
    return out;
}

bool GameUi::showing(const EditorState& state) const {
    const bool playing = state.play != nullptr && state.play->playing();
    return m_font != nullptr && (show == Show::always || (show == Show::playing && playing));
}

void GameUi::update(const EditorState& state, double seconds, float width, float height) {
    const auto from = std::chrono::steady_clock::now();
    const bool playing = state.play != nullptr && state.play->playing();
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    // A save done (by hand or the autosave): said so.
    if (state.saves != nullptr) {
        const GameSaves::Status& s = state.saves->status();
        if (m_saves_known && s.saves > m_saves_seen) toast("Saved", 2.0);
        if (m_saves_known && s.autosaves > m_autosaves_seen) toast("Autosaved", 2.0);
        m_saves_seen = s.saves;
        m_autosaves_seen = s.autosaves;
        m_saves_known = true;
    }
    for (Toast& t : m_toasts) t.left -= seconds;
    std::erase_if(m_toasts, [](const Toast& t) { return t.left <= 0.0; });

    m_scale = std::isfinite(height) ? std::clamp(height / k_lines, 0.25f, 8.0f) : 1.0f;
    m_shown = showing(state);
    m_width = std::isfinite(width) ? width : 0.0f;
    m_height = std::isfinite(height) ? height : 0.0f;
    if (!m_shown) {
        m_build_ms = 0.0;
        return;
    }
    const float s = m_scale;
    m_canvas.begin(width, height, *m_font);
    const float cx = std::round(0.5f * width), cy = std::round(0.5f * height);

    // The crosshair while playing: a small cross, dark-edged so it shows on snow and in the dark.
    if (playing) {
        const float arm = std::round(9.0f * s), gap = std::round(4.0f * s), t = 2.0f * std::max(std::round(s), 1.0f);
        const float e = std::max(std::round(1.0f * s), 1.0f);
        const u32 dark = ui::rgba(0, 0, 0, 140), light = ui::rgba(255, 255, 255, 220);
        const float h = 0.5f * t; // (an even thickness: centred on the middle, on whole pixels)
        const ui::Rect arms[4] = {{cx - gap - arm, cy - h, arm, t},
                                  {cx + gap, cy - h, arm, t},
                                  {cx - h, cy - gap - arm, t, arm},
                                  {cx - h, cy + gap, t, arm}};
        for (const ui::Rect& r : arms) m_canvas.rect({r.x - e, r.y - e, r.w + 2.0f * e, r.h + 2.0f * e}, dark);
        for (const ui::Rect& r : arms) m_canvas.rect(r, light);
    }

    // Screens, by layer: laid out for this frame's size and values, then drawn.
    const auto image = [this](std::string_view name) -> rhi::TextureId {
        const auto it = m_images.find(name);
        return it != m_images.end() ? it->second : 0;
    };
    usize text = ui::Screen::k_max_text_bytes; // shown in all, a frame
    for (auto& [name, loaded] : m_order) {
        (void)name;
        loaded->screen->lay_out(*m_font, values, {width, height}, s, &text);
        loaded->screen->draw(m_canvas, *m_font, values, s, image);
    }

    // A save being written: a mark at the bottom right.
    const std::string doing = state.saves != nullptr ? state.saves->status().doing : std::string();
    if (doing == "saving" || doing == "loading" || doing == "applying") {
        ui::TextStyle style;
        style.size = 22.0f * s;
        style.align = ui::Align::right;
        style.color = ui::rgba(255, 255, 255, 200);
        style.shadow = std::max(1.0f, 2.0f * s);
        m_canvas.text({width - 32.0f * s, height - 56.0f * s}, doing == "saving" ? "Saving…" : "Loading…", style);
    }

    // Toasts: bottom centre, newest lowest, on a dark band (wrapped at 1200 units).
    ui::TextStyle style;
    style.size = 26.0f * s;
    style.align = ui::Align::center;
    style.wrap = 1200.0f * s;
    const float pad_x = 18.0f * s, pad_y = 8.0f * s, gap = 16.0f * s;
    float bottom = height - 130.0f * s;
    for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) {
        const float fade = static_cast<float>(std::clamp(it->left / 0.5, 0.0, 1.0));
        const ui::Vec2 size = ui::measure(*m_font, it->text, style);
        const auto alpha = [&](float a) { return static_cast<u8>(std::lround(a * fade)); };
        const float y = bottom - size.y;
        m_canvas.rect({cx - 0.5f * size.x - pad_x, y - pad_y, size.x + 2.0f * pad_x, size.y + 2.0f * pad_y},
                      ui::rgba(10, 12, 14, alpha(170.0f)));
        style.color = ui::rgba(255, 255, 255, alpha(255.0f));
        m_canvas.text({cx - 0.5f * style.wrap, y}, it->text, style); // (centred within the wrap's width)
        bottom = y - 2.0f * pad_y - gap;
    }
    m_build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}

} // namespace giant::editor
