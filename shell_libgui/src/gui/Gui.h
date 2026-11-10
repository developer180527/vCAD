#pragma once
// The adapter between vCAD's libgui shell and libgui itself.
//
// libgui is pre-1.0 and changes most weeks, so its PLUMBING is confined to this directory: the
// Ui's lifecycle, input, turning a finished frame into render::UiFrame, and docking. When libgui
// renames a lifecycle call or changes its vertex, this is the file that changes, and nothing above
// it does.
//
// Widgets are deliberately NOT wrapped. A panel calls libgui's widgets directly: they are the
// vocabulary a panel is written in, and a one-to-one wrapper per widget would be a second API to
// keep in step with the first for no gain. What churns there is what the panels are about anyway.
//
// No platform types, no libgui types in this header: the host (main.cpp) includes it and must
// stay free of both, and so must anything that is not a panel.

#include "cad/render/UiPass.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct LibguiUi;

namespace cad::shell_libgui::gui {

enum class Button : std::uint8_t { Primary, Secondary, Middle };

/// The keys the host translates. Letters and digits carry their character.
enum class Key : std::uint8_t {
    None, Left, Right, Up, Down, Home, End, PageUp, PageDown, Backspace, Delete, Enter, Tab, Escape,
    Space, Letter, Digit, F,
};

struct Modifiers {
    bool shift = false, ctrl = false, alt = false, logo = false;
};

/// One window's interface: a libgui Ui, the input it has been given, and its last frame.
class Surface {
public:
    /// `font` must outlive every Surface. `sharing` makes this window use another's fonts and
    /// glyph atlas, so a torn-off panel rasterises nothing twice.
    static std::unique_ptr<Surface> create(std::span<const std::uint8_t> font, const Surface* sharing = nullptr);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // Input, in the window's logical pixels, top-left origin.
    void pointerMoved(float x, float y);
    void pointerLeft();
    void button(Button, bool down);
    /// `precise`: a trackpad or a free-spinning wheel reporting pixels, rather than notched lines.
    void wheel(float dx, float dy, bool precise);
    void key(Key, char32_t ch, bool down, bool repeat);
    void text(const std::string& utf8);
    /// IME composition in progress: shown in place, not yet committed. `cursor` is a byte offset.
    void preedit(const std::string& utf8, std::uint64_t cursor);
    void modifiers(const Modifiers&);
    void focusLost();

    /// Builds one frame: begin, `build` with the raw handle, end. Returns false if libgui refused
    /// the frame (a poisoned handle); the reason is in lastError().
    bool frame(float logicalWidth, float logicalHeight, float scale, float dt,
               const std::function<void(LibguiUi*)>& build);

    /// The frame just built, borrowed until the next frame().
    [[nodiscard]] render::UiFrame output() const;

    /// Whether libgui wants another frame without new input: an animation, a caret blink.
    [[nodiscard]] bool wantsFrame(float elapsed) const;

    [[nodiscard]] static std::string lastError();

    /// The vertex layout libgui emits, for render::UiPass::create.
    [[nodiscard]] static std::vector<render::UiAttribute> vertexLayout();
    [[nodiscard]] static std::uint32_t vertexStride();

    [[nodiscard]] LibguiUi* raw() const noexcept { return ui_; }

private:
    explicit Surface(LibguiUi*);
    LibguiUi* ui_;
    mutable std::vector<render::UiBatch> batches_;
};

}  // namespace cad::shell_libgui::gui
