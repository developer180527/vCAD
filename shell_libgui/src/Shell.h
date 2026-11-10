#pragma once
// vCAD's libgui shell: the application, its dock, and its panels. Toolkit-free from the host's
// side -- main.cpp hands it a native view and events and never sees libgui or bgfx.

#include "cad/app/Controller.h"
#include "cad/app/RibbonLayout.h"
#include "cad/app/ViewportInput.h"
#include "cad/kernel/Result.h"
#include "cad/render/Camera.h"
#include "Icons.h"
#include "gui/Gui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct LibguiDock;
struct LibguiUi;

namespace cad::render {
class UiPass;
}

namespace cad::shell_libgui {

/// Which pointer buttons are held, from the host. libgui reports a drag's movement for the primary
/// button only; the viewport's right- and middle-button gestures track the pointer themselves.
struct Buttons {
    bool primary = false, secondary = false, middle = false;
};

/// One entry in a file dialog's type list: a name and its extensions without dots ("step;stp").
struct FileFilter {
    std::string name;
    std::string extensions;
};

/// What the shell needs from the OS, provided by the host (main.cpp), so the shell names no
/// windowing library. Every one of these is the platform's own: the file pickers are the OS's,
/// never drawn by libgui.
struct HostServices {
    /// Shows the OS's open (save=false) or save dialog. `done` gets the chosen path, or an empty
    /// string if the user cancelled; it runs on the main thread, possibly frames later.
    std::function<void(bool save, const std::string& title, const std::vector<FileFilter>& filters,
                       const std::string& suggested, std::function<void(const std::string&)> done)>
        chooseFile;
    /// A modal OS message box. Returns the index of the button pressed, or -1 if it was dismissed.
    std::function<int(const std::string& title, const std::string& message, const std::vector<std::string>& buttons)>
        ask;
    std::function<void(const std::string&)> setTitle;
};

class Shell {
public:
    explicit Shell(std::span<const std::uint8_t> font);
    ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    /// Brings up the renderer on `nativeView` (an NSView on macOS) with the scene drawn into a
    /// texture, then the interface pass on the same bgfx. Fails with the reason, rather than
    /// leaving a window that draws nothing.
    kernel::Result<void> attach(void* nativeView, std::uint32_t pixelWidth, std::uint32_t pixelHeight,
                                double scale);

    /// The OS services: file dialogs, message boxes, the window title.
    void setHost(HostServices host);

    /// The window was asked to close. Calls `close` once unsaved changes are saved or discarded;
    /// never, if the user cancels.
    void requestClose(std::function<void()> close);

    /// Opens a document, as the command line asks.
    kernel::Result<void> open(const std::string& path);

    /// Runs a command by id ("feature.box"), if it exists and is enabled. Also the scripted-check
    /// hook: VCAD_COMMANDS runs a comma-separated list at start, then fits the view.
    void runCommand(const std::string& id);
    void fitView() { controller_.fitView(); }
    /// Opens a command's Properties panel, as its ribbon button does. VCAD_PANEL's hook.
    void openPanel(const std::string& id) { beginPanel(id); }
    /// Runs a File menu action after the next frame: "open", "save", "saveas", "import", "export".
    /// VCAD_FILE's hook, for scripted checks of the native dialogs.
    void fileAction(const std::string& name);
    /// Opens a sketch on an origin plane: "XY", "XZ" or "YZ". VCAD_SKETCH's hook.
    void sketchOn(const std::string& plane);

    /// The main window's input goes here.
    [[nodiscard]] gui::Surface& surface() { return *main_; }

    /// One frame: the interface, then the scene, then present. `origin` is the window's content
    /// area on the desktop, in physical pixels, which docking needs.
    void tick(float logicalWidth, float logicalHeight, float scale, float dt, float originX, float originY,
              Buttons held, float pointerDesktopX, float pointerDesktopY);

    [[nodiscard]] bool wantsFrame(float elapsed) const { return main_->wantsFrame(elapsed); }

private:
    void fileMenu(LibguiUi* ui);
    void openDocument();
    void saveDocument(bool saveAs, std::function<void()> then = {});
    void importFile();
    void exportFile();
    /// Runs `then` once the current document's changes are saved or discarded, or never if the user
    /// cancels: the guard in front of everything that replaces the document.
    void confirmDiscard(std::function<void()> then);
    void syncTitle();
    void alert(const std::string& title, const std::string& message);
    void ribbon(LibguiUi* ui);
    void ribbonItem(LibguiUi* ui, const app::RibbonItem& item);
    void ribbonSection(LibguiUi* ui, const app::RibbonSection& section);
    [[nodiscard]] bool commandEnabled(const std::string& id) const;
    void properties(LibguiUi* ui);
    void beginPanel(const std::string& id);
    void askValue(LibguiUi* ui);
    void applyTheme();
    void panel(LibguiUi* ui, std::uint64_t tab);
    void browser(LibguiUi* ui);
    void viewport(LibguiUi* ui);

    std::span<const std::uint8_t> font_;
    app::Controller controller_;
    std::unique_ptr<gui::Surface> main_;
    std::unique_ptr<render::UiPass> uiPass_;
    LibguiDock* dock_ = nullptr;
    std::uint32_t surfaceWidth_ = 0, surfaceHeight_ = 0;  ///< the window, physical px
    float scale_ = 1.0f;
    Buttons held_;
    std::uint64_t ribbonTab_ = 0;  ///< index into app::partRibbon()
    IconAtlas icons_;
    render::TextureId iconTexture_ = render::TextureId::None;  ///< the atlas, from the UiPass
public:
    /// A label's measured width at the ribbon's font size, from the last paint; 0 if never painted.
    /// Layout is decided before painting, so the first frame estimates and the next is exact.
    std::unordered_map<std::string, float> textWidth_;
private:

    // The viewport widget. Its pointer RULES are app::ViewportInput's, shared with every shell; what
    // is left here is translating libgui's per-frame responses into press, move and release.
    std::uint32_t viewportWidth_ = 0, viewportHeight_ = 0;  ///< the scene's target, physical px
    app::ViewportInput input_{controller_};
    bool gesture_ = false;                 ///< a button went down over the viewport and is still held
    app::ViewportInput::Button gestureButton_ = app::ViewportInput::Button::Primary;
    bool doubleSeen_ = false;              ///< libgui reported this gesture's double click
    bool wasHovered_ = false;
    float lastX_ = -1, lastY_ = -1;        ///< widget-local logical px, last frame
    std::string message_;

    // The Properties panel of the command in progress, Inventor-style: floating over the viewport's
    // top-left. One text buffer per parameter, seeded from the app and pushed back on OK.
    float viewX_ = 0, viewY_ = 0, viewW_ = 0, viewH_ = 0;  ///< the viewport widget, logical px
    std::vector<std::pair<std::string, std::string>> fields_;  ///< name, text being edited
    std::string panelError_;
    std::string panelCommand_;  ///< which command the buffers were seeded for

    // A ribbon item that needs one number before it runs (Offset, Radius), asked in the Properties
    // panel. Empty id: nothing being asked.
    std::string askId_, askTitle_, askLabel_, askText_;

    HostServices host_;
    /// An action chosen during the frame that must run after it: a native dialog is modal, and
    /// opening one halfway through building the interface would leave libgui's frame open under it.
    std::function<void()> pendingAction_;
    std::string path_;          ///< where the document was opened from or last saved to; empty if never
    std::string shownTitle_;    ///< what the window title says, so it is set only when it changes  ///< the last pick or command message, for the status line
};

}  // namespace cad::shell_libgui
