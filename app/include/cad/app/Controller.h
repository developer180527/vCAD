#pragma once

// Everything the UI needs, with no UI in it.
//
// This layer exists because the iPad shell is SwiftUI and the desktop shell is Qt. Any rule that
// lives in MainWindow does not exist on iPad. So: document, selection, commands, undo and the
// scene all live here; the shells only render state and forward gestures.
//
// Deliberately no Qt types, no signals/slots — observers are plain callbacks so both shells can
// implement them. tools/check_layering.py enforces the no-Qt half.

#include "cad/document/Document.h"
#include "cad/recompute/DdcCache.h"
#include "cad/recompute/Engine.h"
#include "cad/render/Camera.h"
#include "cad/render/NullBackend.h"
#include "cad/render/Scene.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cad::app {

/// One row of the model browser. Flat and copyable: the shell rebuilds its tree from these
/// rather than holding pointers into the document, so a recompute cannot dangle a widget.
struct TreeItem {
    document::ObjectId id;
    std::string label;
    std::string type;
    document::ObjectState state = document::ObjectState::Dirty;
    std::string error;              ///< empty unless state is Failed or Blocked
    bool selected = false;
    bool visible = true;
};

/// What a command needs to know before offering itself. A ribbon button that is enabled and then
/// fails is worse than one that is greyed out.
struct CommandContext {
    std::size_t selectedObjects = 0;
    std::size_t selectedElements = 0;
    bool documentEmpty = true;
    bool canUndo = false;
    bool canRedo = false;
};

/// A user-invocable action. The shell renders these; it does not know what they do.
struct Command {
    std::string id;                 ///< "feature.box", stable, used by shortcuts and tests
    std::string label;
    std::string tooltip;
    std::string iconName;           ///< resolved by the shell's theme
    std::function<bool(const CommandContext&)> enabled;
    std::function<void()> invoke;
};

/// The application. One per open document.
class Controller {
public:
    Controller();
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // ── observers ─────────────────────────────────────────────────────────────────────────
    //
    // Plain callbacks, not Qt signals: this header must compile for the SwiftUI shell too.

    /// Fired when the tree, selection or object state changed — i.e. redraw the browser.
    void onDocumentChanged(std::function<void()>);
    /// Fired when something needs repainting but the document did not change (camera, hover).
    void onViewChanged(std::function<void()>);
    /// User-facing message for the status bar. Never a stack trace.
    void onStatus(std::function<void(const std::string&)>);

    // ── document ──────────────────────────────────────────────────────────────────────────

    [[nodiscard]] std::vector<TreeItem> tree() const;
    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }
    [[nodiscard]] CommandContext context() const;

    void select(document::ObjectId, bool additive);
    void clearSelection();
    [[nodiscard]] const std::vector<document::ObjectId>& selection() const noexcept {
        return selection_;
    }

    void setVisible(document::ObjectId, bool);
    void remove(document::ObjectId);

    bool undo();
    bool redo();

    /// Runs the whole pipeline: recompute, then rebuild the scene. Safe to call often.
    void refresh();

    // ── properties, for the inspector ─────────────────────────────────────────────────────

    struct PropertyRow {
        std::string name;
        std::string value;          ///< already formatted for display, units included
        std::string type;
        bool editable = true;
    };
    [[nodiscard]] std::vector<PropertyRow> properties(document::ObjectId) const;

    /// Applies a typed edit from the inspector. Text in, because that is what a UI has; the
    /// parsing and unit handling belong here rather than in every shell.
    bool setProperty(document::ObjectId, const std::string& name, const std::string& text);

    // ── viewport ──────────────────────────────────────────────────────────────────────────

    [[nodiscard]] render::CameraController& camera() noexcept { return camera_; }
    [[nodiscard]] const render::SceneFrame& frame() const noexcept { return scene_->frame(); }
    [[nodiscard]] render::Bounds bounds() const noexcept { return scene_->bounds(); }
    void setViewportSize(std::uint32_t width, std::uint32_t height);
    void fitView();

    /// Statistics for the status bar. Users of large assemblies watch these.
    struct Stats {
        std::size_t objects = 0;
        std::size_t uniqueMeshes = 0;
        std::size_t instances = 0;
        std::size_t triangles = 0;
        std::size_t failed = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    void registerCommands();
    void notifyDocument();
    void notifyView();
    void status(const std::string&);

    /// Adds a primitive and selects it — the behaviour a user expects from a ribbon button.
    document::ObjectId addPrimitive(const std::string& type,
                                    const std::vector<std::pair<std::string, double>>& lengths);

    recompute::FeatureRegistry registry_ = recompute::FeatureRegistry::builtins();
    recompute::MemoryCache cache_;
    recompute::MemoryBlobStore blobs_;
    std::unique_ptr<render::MeshCache> meshes_;

    /// A null backend until the real one is wired in. The scene layer is complete and tested
    /// either way, so the shell can be built and used against this — which is the whole point
    /// of the seam being three narrow interfaces.
    render::NullBackend backend_;
    std::unique_ptr<render::SceneBuilder> scene_;
    render::CameraController camera_;
    render::Viewport viewport_;

    document::History history_{document::Document{}};
    std::vector<document::ObjectId> selection_;
    std::vector<render::Placement> placements_;
    std::vector<Command> commands_;
    std::size_t failedCount_ = 0;

    std::function<void()> documentChanged_;
    std::function<void()> viewChanged_;
    std::function<void(const std::string&)> statusFn_;
};

}  // namespace cad::app
