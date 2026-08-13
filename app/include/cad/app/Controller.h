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
#include "cad/sketch/Sketch.h"

#include <functional>
#include <optional>
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

/// What the app is currently doing, which changes what a click MEANS.
///
/// The concept DESKTOP_UX 3.1 identified as missing and three future features need: the sketch
/// editor, assembly edit-in-place, and drawing sheets. It lives here rather than in the shell
/// because the rule "in a sketch, clicks hit sketch geometry" is a model rule, not a Qt one -- put
/// it in MainWindow and it does not exist on iPad.
enum class Environment : std::uint8_t {
    Model,   ///< the default: features, solids, the browser
    Sketch,  ///< editing one sketch's geometry and constraints
};

const char* toString(Environment) noexcept;

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

    /// Writes the feature tree to a native document (ADR 0003) and marks this controller clean.
    /// `kind` is written into the file's header so openDocument can tell what it is without
    /// trusting the extension. Defaulted because only Part documents have a Controller today;
    /// passing it explicitly is what stops Assembly files being written as parts later.
    kernel::Result<void> saveTo(const std::filesystem::path&, const std::string& kind = "Part");

    /// Replaces this controller's document with one read from disk. Clears undo history: opening a
    /// file is not an edit, and undoing past it into the previous document would be nonsense.
    kernel::Result<void> loadFrom(const std::filesystem::path&);

    /// Whether there are unsaved changes.
    ///
    /// Measured by comparing the document's content digest against the digest at the last save,
    /// rather than by counting edits. That way undoing back to the saved state correctly reports
    /// clean, and an edit that changes nothing (retyping the same dimension) does not mark the
    /// document dirty — both of which a boolean flag flipped on every commit gets wrong.
    [[nodiscard]] bool modified() const noexcept;

    /// Digest used for dirty tracking only: the document digest plus labels. See the
    /// implementation for why labels are in this one and not in Document::digest().
    [[nodiscard]] std::uint64_t saveDigest() const noexcept;

    /// Imports a foreign CAD file (STEP, IGES, STL) as a new feature.
    ///
    /// Takes a path rather than being a plain command, because choosing a file needs a file
    /// dialog and dialogs are the shell's business — `app/` must stay free of any toolkit so the
    /// iPad shell can reuse it. Same split will apply to Open and Save.
    ///
    /// Returns the error the import failed with, so the shell can show it. The document is left
    /// untouched on failure rather than holding a feature that cannot compute.
    kernel::Result<document::ObjectId> importFile(const std::filesystem::path&);

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

public:
    // ── sketch environment ────────────────────────────────────────────────────────────────

    [[nodiscard]] Environment environment() const noexcept { return environment_; }

    /// Enters the sketch environment on an existing Sketch feature. The app enters this FOR the
    /// user (ADR 0008's anti-workbench decision): you never pick a mode, you pick a thing to edit.
    bool editSketch(document::ObjectId);

    /// Creates a Sketch feature and immediately edits it — what Start Sketch does.
    document::ObjectId beginSketch();

    /// Writes the edited sketch back into its feature and returns to the model environment.
    ///
    /// One commit for the whole editing session, not per stroke. A user drawing twelve lines wants
    /// one undo step called "Sketch", not twelve.
    void finishSketch();

    /// Leaves without writing anything back.
    void cancelSketch();

    /// The sketch being edited, or null outside the sketch environment. Non-const so the shell can
    /// add geometry; every mutation must be followed by solveSketch().
    [[nodiscard]] sketch::Sketch* activeSketch() noexcept;
    [[nodiscard]] const sketch::Sketch* activeSketch() const noexcept;

    /// Re-solves the edited sketch and notifies observers. Called after every edit, because a
    /// sketch that does not follow its constraints while you draw is not a sketch.
    sketch::SolveReport solveSketch();

    /// Selected sketch geometry, by GeoId.
    ///
    /// Held here rather than in the canvas for the same reason the model selection is: what is
    /// selected is a model fact that commands act on, and the iPad shell needs the same set. The
    /// canvas does the HIT TESTING, which is genuinely view work -- it needs screen distances and a
    /// pixel tolerance -- and reports the result here.
    void selectSketchGeometry(sketch::GeoId, bool additive);
    void clearSketchSelection();
    [[nodiscard]] const std::vector<sketch::GeoId>& sketchSelection() const noexcept {
        return sketchSelection_;
    }

    /// Deletes the selected sketch geometry, and every constraint that referred to it.
    ///
    /// Dropping the constraints is not optional: a constraint pointing at deleted geometry cannot
    /// be satisfied or even evaluated, and leaving one behind means the next solve fails on a
    /// sketch the user thinks they just tidied up.
    void deleteSketchSelection();

    /// Applies a constraint to the current sketch selection, then re-solves.
    ///
    /// Only the kinds that are unambiguous from GEOMETRY selection are accepted. Coincident and
    /// Distance act on POINTS, and "these two lines are coincident" has no single meaning — which
    /// of the four endpoint pairs? Those need point-level selection, which the canvas does not have
    /// yet. Rejecting them here with a message beats guessing an endpoint pair and constraining
    /// something the user did not ask for.
    ///
    /// Returns false and sets a status message when the selection does not suit the constraint.
    bool applySketchConstraint(sketch::ConstraintKind);

    /// Radius or diameter constraint on the selected circle or arc, in millimetres.
    ///
    /// Separate because it carries a VALUE, and the value comes from the shell — a dialog today,
    /// the command property panel once that exists (DESKTOP_UX 3.2).
    bool applySketchRadius(double millimetres);

    /// Last solve's result, for the status bar's degrees-of-freedom readout.
    [[nodiscard]] const sketch::SolveReport& lastSketchSolve() const noexcept {
        return lastSketchSolve_;
    }

    /// Moves the rollback marker. Null rolls the whole tree forward.
    ///
    /// Public with the sketch API above: the shell drives both directly, unlike the feature
    /// builders below which exist only to serve registered commands.
    ///
    /// Not an undoable edit: where you are looking in the tree is not a change to the model, and an
    /// undo stack full of marker moves would bury the edits a user actually wants to undo.
    void setRollback(std::optional<document::ObjectId>);
    [[nodiscard]] std::optional<document::ObjectId> rollback() const;

private:
    /// Adds a Sketch feature, seeded with a fully constrained rectangle.
    ///
    /// The seed is a STOPGAP and says so in the status line. A real Start Sketch enters a sketch
    /// environment with a drawing canvas (DESKTOP_UX 3.1); until that exists, an empty sketch
    /// feature would fail to compute and give the user a red error for doing what the button said.
    /// A constrained rectangle is something you can immediately extrude and edit through the
    /// properties grid, which exercises the whole parametric path.
    document::ObjectId addSketch();

    /// Extrudes the selected Sketch feature into a solid.
    void addExtrude(double millimetres);

    /// Adds a two-input boolean over the current selection. Cut, Fuse and Common differ only in
    /// the feature type, so they share this rather than three copies of the same twelve lines.
    void addBoolean(const std::string& type, const std::string& label);

    /// Adds an edge-based feature (Fillet, Chamfer) over the selected body.
    ///
    /// Applies to EVERY edge of the body, because edge picking is not wired to the viewport yet.
    /// That is a real limitation, not a placeholder: filleting all edges is what a user gets
    /// until selection exists, and the status message says so. The alternative — leaving the
    /// command disabled — hides a working kernel feature and leaves the naming layer untested
    /// through the UI.
    void addEdgeFeature(const std::string& type, const std::string& label,
                        const std::string& sizeProperty, double millimetres);

    /// Every edge of an object's computed shape, by stable element name. Empty if the object has
    /// no output yet.
    [[nodiscard]] std::vector<naming::ElementName> edgesOf(document::ObjectId) const;

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

    /// Document digest as of the last save (or of the empty document, which is why a brand-new
    /// document is not "modified" until it is actually edited).
    Environment environment_ = Environment::Model;
    /// The sketch being edited. Held by value: it is a working copy, so cancelling is simply
    /// discarding it and no half-applied edit can reach the document.
    std::optional<sketch::Sketch> editing_;
    document::ObjectId editingId_;
    sketch::SolveReport lastSketchSolve_;
    std::vector<sketch::GeoId> sketchSelection_;

    std::uint64_t savedDigest_ = 0;   ///< set in the constructor from saveDigest()
    std::vector<document::ObjectId> selection_;
    std::vector<render::Placement> placements_;
    std::vector<Command> commands_;
    std::size_t failedCount_ = 0;

    std::function<void()> documentChanged_;
    std::function<void()> viewChanged_;
    std::function<void(const std::string&)> statusFn_;
};

}  // namespace cad::app
