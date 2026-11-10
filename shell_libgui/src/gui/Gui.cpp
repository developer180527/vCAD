#include "gui/Gui.h"

#include <libgui.h>

#include <cstdio>
#include <cstdlib>

namespace cad::shell_libgui::gui {

namespace {

uint32_t toLibgui(Key k, char32_t ch) {
    switch (k) {
        case Key::Left: return LIBGUI_KEY_ARROW_LEFT;
        case Key::Right: return LIBGUI_KEY_ARROW_RIGHT;
        case Key::Up: return LIBGUI_KEY_ARROW_UP;
        case Key::Down: return LIBGUI_KEY_ARROW_DOWN;
        case Key::Home: return LIBGUI_KEY_HOME;
        case Key::End: return LIBGUI_KEY_END;
        case Key::PageUp: return LIBGUI_KEY_PAGE_UP;
        case Key::PageDown: return LIBGUI_KEY_PAGE_DOWN;
        case Key::Backspace: return LIBGUI_KEY_BACKSPACE;
        case Key::Delete: return LIBGUI_KEY_DELETE;
        case Key::Enter: return LIBGUI_KEY_ENTER;
        case Key::Tab: return LIBGUI_KEY_TAB;
        case Key::Escape: return LIBGUI_KEY_ESCAPE;
        case Key::Space: return LIBGUI_KEY_SPACE;
        case Key::Letter:
            if (ch >= U'a' && ch <= U'z') ch -= U'a' - U'A';
            return (ch >= U'A' && ch <= U'Z') ? LIBGUI_KEY_LETTER(char(ch)) : 0;
        case Key::Digit: return (ch >= U'0' && ch <= U'9') ? LIBGUI_KEY_DIGIT(ch - U'0') : 0;
        case Key::F: return (ch >= 1 && ch <= 12) ? LIBGUI_KEY_F(ch) : 0;
        case Key::None: break;
    }
    return 0;
}

}  // namespace

std::unique_ptr<Surface> Surface::create(std::span<const std::uint8_t> font, const Surface* sharing) {
    if (libgui_abi_version() != LIBGUI_ABI_VERSION) return nullptr;  // header and library disagree
    LibguiUi* ui = sharing ? libgui_ui_new_sharing_fonts(sharing->ui_) : libgui_ui_new(font.data(), font.size());
    if (ui == nullptr) return nullptr;
    // Paper White is vCAD's reference theme and is not ported yet; libgui's light preset stands in
    // until its tokens are extracted (see the shell's README).
    libgui_set_theme(ui, "light");
    libgui_install_default_keymap(ui);
    libgui_enable_mesh(ui, 1);  // bgfx cannot carry libgui's six-vec4 instances
    return std::unique_ptr<Surface>(new Surface(ui));
}

Surface::Surface(LibguiUi* ui) : ui_(ui) {}
Surface::~Surface() { libgui_ui_free(ui_); }

void Surface::pointerMoved(float x, float y) { libgui_push_pointer_moved(ui_, x, y); }
void Surface::pointerLeft() { libgui_push_pointer_left(ui_); }
void Surface::button(Button b, bool down) {
    const uint32_t lb = b == Button::Secondary ? LIBGUI_BUTTON_SECONDARY
                        : b == Button::Middle  ? LIBGUI_BUTTON_MIDDLE
                                               : LIBGUI_BUTTON_PRIMARY;
    libgui_push_pointer_button(ui_, lb, down ? 1 : 0);
}
void Surface::wheel(float dx, float dy, bool precise) {
    // Sent in the unit it arrived in, never converted: libgui applies its own policy per unit.
    libgui_push_wheel(ui_, dx, dy, precise ? LIBGUI_WHEEL_PIXEL : LIBGUI_WHEEL_LINE);
}
void Surface::key(Key k, char32_t ch, bool down, bool repeat) {
    if (const uint32_t lk = toLibgui(k, ch)) libgui_push_key(ui_, lk, down ? 1 : 0, repeat ? 1 : 0);
}
void Surface::text(const std::string& utf8) { libgui_push_text(ui_, utf8.c_str()); }
void Surface::preedit(const std::string& utf8, std::uint64_t cursor) {
    libgui_push_ime_preedit(ui_, utf8.c_str(), cursor);
}
void Surface::modifiers(const Modifiers& m) {
    libgui_push_modifiers(ui_, LibguiModifiers{uint8_t(m.shift), uint8_t(m.ctrl), uint8_t(m.alt), uint8_t(m.logo)});
}
void Surface::focusLost() { libgui_push_focus_lost(ui_); }

bool Surface::frame(float w, float h, float scale, float dt, const std::function<void(LibguiUi*)>& build) {
    // VCAD_REFERENCE=<file.ppm>: also render every frame on libgui's CPU reference renderer and keep
    // the last one. The difference between it and the window is the GPU port's fault, not libgui's.
    static const char* reference = std::getenv("VCAD_REFERENCE");
    if (reference) libgui_enable_reference_render(ui_, 1);
    libgui_begin_frame(ui_, w, h, scale, dt);
    build(ui_);
    libgui_end_frame(ui_);
    if (reference) {
        uint32_t rw = 0, rh = 0;
        if (const uint8_t* px = libgui_reference_pixels(ui_, &rw, &rh)) {
            if (FILE* f = std::fopen(reference, "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", rw, rh);
                for (uint64_t i = 0; i < uint64_t(rw) * rh; ++i) std::fwrite(px + i * 4, 1, 3, f);
                std::fclose(f);
            }
        }
    }
    return libgui_ui_poisoned(ui_) == 0;
}

render::UiFrame Surface::output() const {
    render::UiFrame f;
    uint64_t nv = 0, ni = 0, nb = 0;
    const void* v = libgui_mesh_vertices(ui_, &nv);
    const uint32_t* idx = libgui_mesh_indices(ui_, &ni);
    const LibguiBatch* b = libgui_mesh_batches(ui_, &nb);
    f.vertices = {static_cast<const std::byte*>(v), std::size_t(nv) * libgui_vertex_stride()};
    f.vertexCount = uint32_t(nv);
    f.indices = {idx, std::size_t(ni)};
    batches_.clear();
    for (uint64_t i = 0; i < nb; ++i)
        batches_.push_back({b[i].texture_kind == 1, b[i].texture_index, b[i].first, b[i].count});
    f.batches = batches_;

    LibguiGlobals g{};
    libgui_frame_globals(ui_, &g);
    f.logicalWidth = g.screen_width;
    f.logicalHeight = g.screen_height;
    f.scale = g.scale;
    LibguiColor c{};
    libgui_frame_clear_color(ui_, &c);
    f.clear[0] = c.r, f.clear[1] = c.g, f.clear[2] = c.b, f.clear[3] = c.a;
    f.atlas = libgui_frame_atlas(ui_, &f.atlasSize, &f.atlasVersion);
    return f;
}

bool Surface::wantsFrame(float elapsed) const { return libgui_needs_frame(ui_, elapsed) != 0; }

std::string Surface::lastError() {
    const char* e = libgui_last_error();
    return e ? e : "";
}

std::vector<render::UiAttribute> Surface::vertexLayout() {
    std::vector<render::UiAttribute> out;
    for (uint32_t i = 0; i < libgui_vertex_attribute_count(); ++i) {
        render::UiAttribute a;
        libgui_vertex_attribute(i, &a.floats, &a.offset);
        const char* n = libgui_vertex_attribute_name(i);
        a.name = n ? n : "";
        out.push_back(std::move(a));
    }
    return out;
}

std::uint32_t Surface::vertexStride() { return uint32_t(libgui_vertex_stride()); }

}  // namespace cad::shell_libgui::gui
