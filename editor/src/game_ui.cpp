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
    m_shown = m_font != nullptr && (show == Show::always || (show == Show::playing && playing));
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
