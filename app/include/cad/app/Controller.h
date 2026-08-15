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
#include "cad/features/Builtins.h"
#include "cad/recompute/Engine.h"
#include "cad/render/Camera.h"
#include "cad/render/BgfxBackend.h"
#include "cad/render/NullBackend.h"
#include "cad/render/Scene.h"
#include "cad/sketch/Sketch.h"
#include "cad/units/Units.h"

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

/// One editable input of a command in progress.
///
/// Text in and text out, exactly like PropertyRow: the shell renders a field, the user types, and
/// the parsing and unit handling stay here rather than in every shell. `kind` is a rendering hint
/// only — a Bool wants a checkbox, a Length wants a line edit that accepts "2 in".
struct CommandParameter {
    enum class Kind : std::uint8_t { Length, Angle, Real, Integer, Text, Bool };
    std::string name;      ///< stable key the command reads
    std::string label;     ///< shown to the user
    Kind kind = Kind::Length;
    std::string value;     ///< current text, already formatted with units
};

/// User preferences that change how the application behaves, not what the model is.
///
/// In `app/` rather than the shell because every one of these is consumed BELOW the shell: display
/// units decide how Controller formats a property, the navigation preset is read by the camera, and
/// the sketch tolerances are passed to inference. A shell that owned them would have to push each
/// one down on every change, and the iPad shell would have to do it again.
///
/// Deliberately NOT part of the document. A colleague opening your file should get their own units
/// and their own mouse, not yours.
struct Preferences {
    /// How lengths are SHOWN and how a bare number is READ. Storage is always millimetres; this
    /// never changes what is in the file.
    units::UnitSystem displayUnits = units::UnitSystem::Millimetre;

    /// Which mouse button orbits. The single most personal setting in any CAD application, and the
    /// first thing someone changes when the app does not match the one they came from.
    render::NavigationPreset navigation = render::NavigationPreset::Cad;

    /// Sketch constraint inference, in document units and degrees. Exposed because the right value
    /// depends on the drawings a user actually receives — see Infer.h on why guessing is unsafe.
    double snapTolerance = 0.01;
    double angleTolerance = 0.5;
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

    [[nodiscard]] const Preferences& preferences() const noexcept { return preferences_; }
    /// Applies preferences and notifies. Everything derived from them — property formatting, the
    /// camera's gesture mapping — updates from this one call rather than each shell remembering.
    void setPreferences(const Preferences&);

    /// Renames a feature. Cosmetic to the geometry, but NOT to the file: labels are saved, and
    /// Controller::saveDigest folds them in so a rename marks the document modified.
    void rename(document::ObjectId, const std::string& label);

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

    /// Pushes the camera's current matrices into the scene and requests a repaint.
    ///
    /// Call after orbit/pan/zoom. Mutating the CameraController alone changes nothing on screen:
    /// the scene holds its own copy of the matrices, and until they are pushed the frame renders
    /// from the old ones. This used to happen as a side effect of setViewportSize, which meant
    /// every mouse-move ran two culls to deliver one camera update -- and removing that call as
    /// waste silently disabled orbiting altogether.
    void cameraChanged();

    /// Viewport background, 0..255 per channel. The shell passes its theme colour down so the
    /// GPU clears to the same paper the rest of the window is painted on.
    void setViewportBackground(int r, int g, int b);

    // ── the GPU renderer ──────────────────────────────────────────────────────────────────
    //
    // Off by default. Everything above works against the null backend, and a shell that never
    // calls attachRenderer() behaves exactly as it did before this existed — which is what keeps
    // the headless tests and the CI build honest.

    /// Brings up bgfx and rebuilds the scene against it. Offscreen: the frame is rendered into a
    /// framebuffer and read back, rather than drawn into a native surface owned by the shell.
    ///
    /// That is a deliberate first step, not the endgame. It composites with the Qt widgets we
    /// already have (marking menu, context toolbar, ViewCube when it lands) because they paint
    /// over an ordinary QWidget, and it is identical on all three platforms. It costs a full
    /// framebuffer readback per frame, which is why renderFrame() is honest about its cost and
    /// why this is not yet a claim about interactive performance on large assemblies.
    ///
    /// "Offscreen" does NOT mean "no window" — bgfx still needs a Metal device, and on macOS
    /// render/src/MetalSurface.mm creates a standalone CAMetalLayer for exactly this case.
    /// `nativeView` is the shell's own native view handle — on macOS the NSView behind
    /// QWidget::winId(). Pass it and the renderer draws STRAIGHT TO THE SCREEN with no readback,
    /// which is the difference between 27 fps and the display's refresh rate. Pass null and it
    /// falls back to the offscreen path, which runs everywhere and composites with Qt overlays.
    ///
    /// The shell hands over a view, not a surface: turning one into the other is platform work,
    /// and doing it here keeps Metal out of shell_qt. The same call will take an HWND or an
    /// X11 Window when those land.
    kernel::Result<void> attachRenderer(std::uint32_t width, std::uint32_t height,
                                        void* nativeView = nullptr, double scale = 1.0);

    /// True when the renderer presents directly to a native surface rather than being blitted.
    /// The shell needs to know: presenting is submit-only, blitting needs a paint.
    [[nodiscard]] bool presentsDirectly() const noexcept { return presenting_; }

    /// Submits a frame straight to the native surface. Only valid when presentsDirectly().
    ///
    /// Separate from renderFrame() because it is a genuinely different operation, not an option
    /// on the same one: there is no image, nothing is copied back, and the cost is the draw calls
    /// alone.
    void presentFrame();

    [[nodiscard]] bool rendererAttached() const noexcept { return gpu_ != nullptr; }

    /// Which renderer bgfx chose, or "null". Worth showing: "why is it blank" has twice been
    /// "it fell back to Noop", and the name is the only clue.
    [[nodiscard]] std::string rendererName() const;

    /// One rendered frame, WITH the dimensions it was actually rendered at.
    ///
    /// The size is returned rather than left for the caller to recompute from its own widget,
    /// because those two can disagree — a resize reaches the widget and the backend at different
    /// moments — and when they do, every row of the image starts at the wrong offset and the
    /// frame arrives skewed into diagonal streaks. Returning them together makes that
    /// unrepresentable.
    struct RenderedFrame {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> pixels;   ///< tightly packed RGBA8, width * height * 4
    };

    /// Renders one frame. Fails rather than returning an empty frame when no renderer is
    /// attached, because a blank image and a broken renderer must not look the same.
    kernel::Result<RenderedFrame> renderFrame();

    /// Where the last frame's time went, in milliseconds.
    ///
    /// Split into submit and readback because the two have completely different fixes: submit
    /// time is the scene and the draw calls, readback is the price of the offscreen path and is
    /// only removed by rendering to a native surface. Guessing which one dominates is how a day
    /// gets spent on the wrong half.
    struct RenderTiming {
        double submitMs = 0.0;    ///< building and submitting the frame
        double captureMs = 0.0;   ///< blit + readTexture + the frames pumped waiting for it
    };
    [[nodiscard]] RenderTiming lastRenderTiming() const noexcept { return timing_; }

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

    // ── command in progress ───────────────────────────────────────────────────────────────
    //
    // The state DESKTOP_UX 3.2 has described since ADR 0008 promised "non-modal" and did not say
    // what that meant. A command with parameters is STARTED, edited, then committed or abandoned —
    // it is not a function call that happens instantly with defaults.
    //
    // Lives here rather than in the shell because both shells need it, and because committing is a
    // document edit: only this layer may touch History.

    /// Starts a parameterised command. False if the id has none, in which case the shell should
    /// invoke it directly as before.
    bool beginCommand(const std::string& id);

    /// Empty when no command is in progress.
    [[nodiscard]] const std::string& activeCommand() const noexcept { return activeCommand_; }
    [[nodiscard]] const std::vector<CommandParameter>& commandParameters() const noexcept {
        return commandParameters_;
    }

    /// Text in, as everywhere else. Returns false if the text will not parse, leaving the old value
    /// so a half-typed entry cannot destroy a good one.
    bool setCommandParameter(const std::string& name, const std::string& text);

    /// Applies the command and clears the panel. False if it could not run.
    bool commitCommand();
    void cancelCommand();

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

    /// Whether the marker suspends this feature. Delegated to Document rather than left for the
    /// shell to recompute: "everything after the marker" is a model rule, and a shell that
    /// reimplements the comparison is a shell that can disagree with the engine about what is
    /// suspended.
    [[nodiscard]] bool isRolledBack(document::ObjectId) const;

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

    recompute::FeatureRegistry registry_ = features::builtins();
    recompute::MemoryCache cache_;
    recompute::MemoryBlobStore blobs_;
    std::unique_ptr<render::MeshCache> meshes_;

    /// The fallback, and still the default. The scene layer is complete and tested against it,
    /// so a shell that never attaches a GPU is fully functional — which is the whole point of
    /// the seam being three narrow interfaces, and is what CI runs.
    render::NullBackend backend_;

    /// Non-null once attachRenderer() succeeds. Held by pointer because bgfx is a process
    /// singleton with a real lifetime, unlike the null backend which is just memory.
    std::unique_ptr<render::BgfxBackend> gpu_;

    /// Whichever of the two the scene is currently built against. Nothing above the seam knows
    /// which one it holds.
    render::Backend active_;
    RenderTiming timing_;
    bool presenting_ = false;

    /// Drops our reference to the presentation surface and forgets it. Null-safe, and a no-op
    /// where the surface belongs to the shell.
    void releaseSurface() noexcept;

    /// The platform surface the renderer presents into.
    ///
    /// OWNED only on Apple, where attachRenderer creates the CAMetalLayer. Elsewhere it is the
    /// shell's own window handle, which is not ours to destroy — hence releaseSurface() rather
    /// than a destroy call at each site.
    void* surface_ = nullptr;
    double viewportScale_ = 1.0;   ///< device pixel ratio, kept for layer resizes

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

    Preferences preferences_;
    std::string activeCommand_;
    std::vector<CommandParameter> commandParameters_;

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
