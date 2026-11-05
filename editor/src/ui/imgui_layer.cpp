#include "imgui_layer.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#include <imgui.h>

#include <cfloat>
#include <span>

namespace giant::editor {

namespace {

using platform::InputEvent;
using platform::Key;

static_assert(sizeof(ImDrawIdx) == sizeof(u32), "the build sets ImDrawIdx to 32 bits");
static_assert(sizeof(ImDrawVert) == sizeof(rhi::UiVertex), "same vertex layout");

ImGuiKey to_imgui(Key key) {
    const auto k = static_cast<u16>(key);
    if (key >= Key::a && key <= Key::z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (k - static_cast<u16>(Key::a)));
    }
    if (key >= Key::n0 && key <= Key::n9) {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (k - static_cast<u16>(Key::n0)));
    }
    if (key >= Key::f1 && key <= Key::f12) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (k - static_cast<u16>(Key::f1)));
    }
    switch (key) {
    case Key::tab: return ImGuiKey_Tab;
    case Key::left: return ImGuiKey_LeftArrow;
    case Key::right: return ImGuiKey_RightArrow;
    case Key::up: return ImGuiKey_UpArrow;
    case Key::down: return ImGuiKey_DownArrow;
    case Key::page_up: return ImGuiKey_PageUp;
    case Key::page_down: return ImGuiKey_PageDown;
    case Key::home: return ImGuiKey_Home;
    case Key::end: return ImGuiKey_End;
    case Key::insert: return ImGuiKey_Insert;
    case Key::del: return ImGuiKey_Delete;
    case Key::backspace: return ImGuiKey_Backspace;
    case Key::space: return ImGuiKey_Space;
    case Key::enter: return ImGuiKey_Enter;
    case Key::escape: return ImGuiKey_Escape;
    case Key::apostrophe: return ImGuiKey_Apostrophe;
    case Key::comma: return ImGuiKey_Comma;
    case Key::minus: return ImGuiKey_Minus;
    case Key::period: return ImGuiKey_Period;
    case Key::slash: return ImGuiKey_Slash;
    case Key::semicolon: return ImGuiKey_Semicolon;
    case Key::equals: return ImGuiKey_Equal;
    case Key::left_bracket: return ImGuiKey_LeftBracket;
    case Key::backslash: return ImGuiKey_Backslash;
    case Key::right_bracket: return ImGuiKey_RightBracket;
    case Key::grave: return ImGuiKey_GraveAccent;
    case Key::left_ctrl: return ImGuiKey_LeftCtrl;
    case Key::left_shift: return ImGuiKey_LeftShift;
    case Key::left_alt: return ImGuiKey_LeftAlt;
    case Key::left_super: return ImGuiKey_LeftSuper;
    case Key::right_ctrl: return ImGuiKey_RightCtrl;
    case Key::right_shift: return ImGuiKey_RightShift;
    case Key::right_alt: return ImGuiKey_RightAlt;
    case Key::right_super: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

int to_imgui(platform::MouseButton button) {
    switch (button) {
    case platform::MouseButton::left: return 0;
    case platform::MouseButton::right: return 1;
    case platform::MouseButton::middle: return 2;
    case platform::MouseButton::x1: return 3;
    case platform::MouseButton::x2: return 4;
    }
    return 0;
}

platform::Window::Cursor to_cursor(ImGuiMouseCursor cursor) {
    using C = platform::Window::Cursor;
    switch (cursor) {
    case ImGuiMouseCursor_None: return C::hidden;
    case ImGuiMouseCursor_TextInput: return C::text;
    case ImGuiMouseCursor_ResizeAll: return C::resize_all;
    case ImGuiMouseCursor_ResizeNS: return C::resize_ns;
    case ImGuiMouseCursor_ResizeEW: return C::resize_ew;
    case ImGuiMouseCursor_ResizeNESW: return C::resize_nesw;
    case ImGuiMouseCursor_ResizeNWSE: return C::resize_nwse;
    case ImGuiMouseCursor_Hand: return C::hand;
    case ImGuiMouseCursor_NotAllowed: return C::not_allowed;
    default: return C::arrow;
    }
}

} // namespace

ImGuiLayer::ImGuiLayer(ConstructKey, platform::Window& window, rhi::Device& device)
    : m_window(window), m_device(device) {}

std::unique_ptr<ImGuiLayer> ImGuiLayer::create(platform::Window& window, rhi::Device& device,
                                               const char* ini_path) {
    IMGUI_CHECKVERSION();
    auto layer = std::make_unique<ImGuiLayer>(ConstructKey{}, window, device);
    layer->m_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(layer->m_context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = ini_path;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendPlatformName = "giant_platform";
    io.BackendRendererName = "giant_rhi";
    ImGui::StyleColorsDark();

    // The font atlas goes to the GPU once (the classic path: this renderer
    // does not take ImGui texture updates yet).
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const usize bytes = static_cast<usize>(width) * static_cast<usize>(height) * 4;
    rhi::TextureDesc font_desc;
    font_desc.width = static_cast<u32>(width);
    font_desc.height = static_cast<u32>(height);
    font_desc.format = rhi::Format::rgba8_unorm; // UI colours are gamma-space
    font_desc.usage = rhi::TextureUsage::sampled;
    font_desc.name = "UI font atlas";
    layer->m_font_texture = device.create_texture(font_desc, std::span<const u8>(pixels, bytes));
    if (!layer->m_font_texture.valid()) {
        log_error("editor", "could not create the UI font texture");
        return nullptr;
    }
    io.Fonts->SetTexID(static_cast<ImTextureID>(rhi::ui_texture_id(layer->m_font_texture)));
    return layer;
}

ImGuiLayer::~ImGuiLayer() {
    if (m_context != nullptr) {
        ImGui::DestroyContext(m_context);
    }
    m_device.destroy(m_font_texture); // deferred until the GPU is done with it
}

void ImGuiLayer::begin_frame(float delta_seconds, const std::vector<InputEvent>& input) {
    ImGui::SetCurrentContext(m_context);
    ImGuiIO& io = ImGui::GetIO();
    const float points_w = static_cast<float>(m_window.point_width());
    const float points_h = static_cast<float>(m_window.point_height());
    io.DisplaySize = ImVec2(points_w, points_h);
    if (points_w > 0.0f && points_h > 0.0f) {
        io.DisplayFramebufferScale =
            ImVec2(static_cast<float>(m_window.pixel_width()) / points_w,
                   static_cast<float>(m_window.pixel_height()) / points_h);
    }
    io.DeltaTime = delta_seconds > 0.0f ? delta_seconds : 1.0f / 60.0f;

    for (const InputEvent& e : input) {
        switch (e.type) {
        case InputEvent::Type::mouse_move: io.AddMousePosEvent(e.x, e.y); break;
        case InputEvent::Type::mouse_left: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
        case InputEvent::Type::mouse_button:
            io.AddMousePosEvent(e.x, e.y);
            io.AddMouseButtonEvent(to_imgui(e.button), e.down);
            break;
        case InputEvent::Type::mouse_wheel:
            io.AddMouseWheelEvent(-e.x, e.y); // ImGui: +x scrolls left, ours scrolls right
            break;
        case InputEvent::Type::key: {
            using platform::modifier::alt;
            using platform::modifier::ctrl;
            using platform::modifier::shift;
            using platform::modifier::super;
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.modifiers & ctrl) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.modifiers & shift) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.modifiers & alt) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (e.modifiers & super) != 0);
            const ImGuiKey key = to_imgui(e.key);
            if (key != ImGuiKey_None) {
                io.AddKeyEvent(key, e.down);
            }
            break;
        }
        case InputEvent::Type::text: io.AddInputCharactersUTF8(e.text.c_str()); break;
        case InputEvent::Type::focus: io.AddFocusEvent(e.down); break;
        }
    }
    ImGui::NewFrame();
}

void ImGuiLayer::end_frame() {
    ImGui::SetCurrentContext(m_context);
    ImGui::Render();
    const ImGuiIO& io = ImGui::GetIO();
    m_window.set_text_input(io.WantTextInput);
    m_window.set_cursor(io.MouseDrawCursor ? platform::Window::Cursor::hidden
                                           : to_cursor(ImGui::GetMouseCursor()));

    // Convert to pixels: ImGui works in points, the swap chain in pixels.
    const ImDrawData* draw = ImGui::GetDrawData();
    m_vertices.clear();
    m_indices.clear();
    m_commands.clear();
    m_lists.clear();
    if (draw == nullptr || !draw->Valid) {
        return;
    }
    const ImVec2 scale = draw->FramebufferScale;
    const ImVec2 origin = draw->DisplayPos;
    struct Range {
        usize vertex_begin, vertex_count, index_begin, index_count, command_begin, command_count;
    };
    std::vector<Range> ranges;
    for (const ImDrawList* list : draw->CmdLists) {
        Range range{m_vertices.size(), static_cast<usize>(list->VtxBuffer.Size), m_indices.size(),
                    static_cast<usize>(list->IdxBuffer.Size), m_commands.size(), 0};
        for (const ImDrawVert& v : list->VtxBuffer) {
            m_vertices.push_back({(v.pos.x - origin.x) * scale.x, (v.pos.y - origin.y) * scale.y,
                                  v.uv.x, v.uv.y, v.col});
        }
        m_indices.insert(m_indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback != nullptr) {
                // The editor adds no callbacks; ImGui's reset marker needs no work.
                GIANT_ASSERT(cmd.UserCallback == ImDrawCallback_ResetRenderState,
                             "UI draw callbacks are not supported");
                continue;
            }
            rhi::UiDrawCommand out;
            out.first_index = cmd.IdxOffset;
            out.index_count = cmd.ElemCount;
            out.vertex_offset = cmd.VtxOffset;
            out.clip_min_x = (cmd.ClipRect.x - origin.x) * scale.x;
            out.clip_min_y = (cmd.ClipRect.y - origin.y) * scale.y;
            out.clip_max_x = (cmd.ClipRect.z - origin.x) * scale.x;
            out.clip_max_y = (cmd.ClipRect.w - origin.y) * scale.y;
            out.texture = static_cast<rhi::TextureId>(cmd.GetTexID());
            m_commands.push_back(out);
            ++range.command_count;
        }
        ranges.push_back(range);
    }
    // Views are made after all appends, so no vector moves under them.
    for (const Range& r : ranges) {
        m_lists.push_back({std::span<const rhi::UiVertex>(m_vertices).subspan(r.vertex_begin,
                                                                              r.vertex_count),
                           std::span<const u32>(m_indices).subspan(r.index_begin, r.index_count),
                           std::span<const rhi::UiDrawCommand>(m_commands)
                               .subspan(r.command_begin, r.command_count)});
    }
}

bool ImGuiLayer::wants_keyboard() const {
    ImGui::SetCurrentContext(m_context);
    return ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace giant::editor
