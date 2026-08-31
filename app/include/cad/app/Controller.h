#pragma once

// Everything the UI needs, with no UI in it.
//
// This layer exists because the iPad shell is SwiftUI and the desktop shell is Qt. Any rule that
// lives in MainWindow does not exist on iPad. So: document, selection, commands, undo and the
// scene all live here; the shells only render state and forward gestures.
//
// Deliberately no Qt types, no signals/slots — observers are plain callbacks so both shells can
// implement them. tools/check_layering.py enforces the no-Qt half.

#include "cad/app/Model.h"
#include "cad/app/SelectionRanking.h"
#include "cad/app/SketchDrawing.h"
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
#include <span>
#include <array>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace cad::app {

/// One row of the model browser. Flat and copyable: the shell rebuilds its tree from these
/// rather than holding pointers into the document, so a recompute cannot dangle a widget.
/// Where an object belongs in the browser.
///
/// A parametric modeller shows two different things: what EXISTS, by kind, and what HAPPENED, in
/// order. A flat list conflates them, which is how three origin datums ended up sitting in the
/// feature history between a Box and a Sketch — reading as three modelling steps the user did not
/// perform. Reference geometry is not history and does not belong in it.
///
/// SolidWorks and Inventor keep one ordered tree and separate the two with FOLDERS; Fusion splits
/// them across a browser and a timeline. vCAD follows the first, because its tree is already one
/// ordered list — see docs/design/MODELLING_UX.md §1.
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

    /// The document as it stands. Read-only: every edit goes through a command so that undo,
    /// dirty tracking and recompute cannot be bypassed.
    ///
    /// Exposed because callers outside this class legitimately need to READ geometry — a shell
    /// resolving a picked face to its element name, a test checking where a solid ended up. The
    /// alternative was a growing set of one-off accessors, each of which is this one with a
    /// narrower view and another name to learn.
    [[nodiscard]] const document::Document& document() const noexcept { return history_.current(); }
    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }
    [[nodiscard]] CommandContext context() const;

    void select(document::ObjectId, bool additive);

    /// Replaces the whole selection in ONE step.
    ///
    /// Exists because the incremental form is a trap for a shell whose tree rebuilds on notification:
    /// clear-then-select-each fires N+1 notifications, each rebuilding the widget the caller is
    /// iterating. One call, one notification, one rebuild.
    void setSelection(std::vector<document::ObjectId>);

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

    /// Orbits the view, unless a sketch is open.
    ///
    /// # Why the rule lives here
    ///
    /// A sketch is drawn on a plane, and the whole of the in-place sketcher assumes you are looking
    /// at that plane: the pointer is unprojected onto it, the rubber band is drawn in it, and the
    /// live dimensions are measured in it. Orbiting away leaves every one of those working and
    /// none of them legible — the user is drawing on a surface they are seeing edge-on.
    ///
    /// **This is a deliberate divergence.** Fusion, SolidWorks and Inventor all permit orbiting
    /// inside a sketch; they simply start you normal to the plane. vCAD locks it until the sketch
    /// is finished, which was an explicit product decision — recorded in MODELLING_UX.md §2 beside
    /// the camera-restore divergence so the next person does not "fix" it back.
    ///
    /// Returns false when it refused, so a shell can say why rather than appearing to ignore the
    /// gesture.
    bool orbitCamera(float dxPixels, float dyPixels);

    /// Pushes the camera's current matrices into the scene and requests a repaint.
    ///
    /// Call after orbit/pan/zoom. Mutating the CameraController alone changes nothing on screen:
    /// the scene holds its own copy of the matrices, and until they are pushed the frame renders
    /// from the old ones. This used to happen as a side effect of setViewportSize, which meant
    /// every mouse-move ran two culls to deliver one camera update -- and removing that call as
    /// waste silently disabled orbiting altogether.
    void cameraChanged();

    // ── picking ───────────────────────────────────────────────────────────────────────────
    //
    // Here rather than in the shell for the reason the whole layer exists: "a click at these
    // pixels means that face" is a model rule, and a Qt implementation of it does not exist on
    // iPad. The pick pass has been in the bgfx backend since M3 and has never been driven from
    // above; this is the seam that drives it.

    /// What was under a point, in model terms.
    struct Pick {
        bool hit = false;
        document::ObjectId object{};      ///< which document object owns the element
        naming::ElementName element;      ///< null if the slot carried no name
        std::uint32_t slot = 0;           ///< absolute element slot, for O(1) highlighting
        float depth = 1.0f;
    };

    /// What a click selects.
    ///
    /// Lives here rather than in the shell, which is where the setting started (see MainWindow's
    /// filter bar): "a click at this level means that face" is a model rule, and a rule that lives
    /// in MainWindow does not exist on iPad. Named `SelectionLevel` rather than "filter" because it
    /// does not filter a set -- it decides what a pick RESOLVES TO.
    /// `Auto` resolves to whatever was actually hit — the vertex, edge or face the ranking chose.
    ///
    /// The right default for touch, and arguably for everything. A fixed level means the user must
    /// declare in advance what kind of thing they are about to point at, which a desktop can afford
    /// (a persistent filter control) and a tablet cannot: the screen is too precious for a control
    /// that is only occasionally needed, and the ranking already knows what was under the finger.
    ///
    /// Appended, not inserted, because these values are persisted in preferences.
    enum class SelectionLevel : std::uint8_t { Body, Face, Edge, Vertex, Auto };

    [[nodiscard]] SelectionLevel selectionLevel() const noexcept { return selectionLevel_; }

    /// Changes the level, and clears the selection made at the old one.
    ///
    /// Kept rather than converted: there is no honest mapping from "this face" to "this edge", and
    /// silently keeping a face selected while the level says Edge is how a command ends up acting on
    /// something the user cannot see is selected.
    void setSelectionLevel(SelectionLevel);

    /// Selects one named element directly, without going through a pick.
    ///
    /// The GPU pick is how a CLICK selects; this is how anything else does — a model tree that
    /// lists faces, a command that acts on a named edge, a test that must not need a GPU. Refuses
    /// a name the object's map does not know, so a stale reference cannot enter the selection.
    bool selectElement(document::ObjectId, const naming::ElementName&, bool additive = false);

    /// One picked piece of geometry, below the level of a document object.
    struct ElementSelection {
        document::ObjectId object{};
        naming::ElementName element;
        std::uint32_t slot = 0;        ///< kept so highlighting stays O(1)
    };
    [[nodiscard]] const std::vector<ElementSelection>& elementSelection() const noexcept {
        return elementSelection_;
    }

    /// Nearest element at a point, in DEVICE pixels.
    ///
    /// Device rather than logical, because that is what the id buffer is indexed in. A shell on a
    /// Retina display that forwards logical coordinates picks at half the intended position, and
    /// the bug looks like an inaccurate picker rather than a units mistake.
    [[nodiscard]] Pick pickAt(std::uint32_t x, std::uint32_t y);

    /// A planar face at a point, with the frame a sketch would be placed on.
    ///
    /// Separate from `pickAt` because the ANSWER is different: a pick either found something or
    /// did not, whereas asking for a sketch plane can fail in ways the user needs told — nothing
    /// under the cursor, an element that is an edge rather than a face, or a face that is curved.
    /// `kernel::planeOf` already refuses a non-planar face with a reason, and the point of
    /// returning a Result here is that the reason survives the trip to the shell instead of
    /// becoming a silent no-op on click.
    struct FacePick {
        document::ObjectId object{};
        naming::ElementName face;
        sketch::SketchFrame frame;
    };
    [[nodiscard]] kernel::Result<FacePick> pickSketchFace(std::uint32_t x, std::uint32_t y);

    /// Points the camera face-on at a sketch plane, and repaints.
    ///
    /// Takes the resolved frame rather than a face name, because the same call has to serve a datum
    /// and a global plane later, and all three arrive here as a frame. The camera keeps its distance
    /// and its projection: aligning is a rotation, and changing the zoom at the same time makes the
    /// transition impossible to follow.
    void alignViewTo(const sketch::SketchFrame&);

    /// What a click did, so the shell can say so without working it out again.
    ///
    /// A message even on success, because at Face or Edge level the useful feedback is WHICH face --
    /// and an empty message on the failure paths would leave a click that did nothing unexplained,
    /// which is the complaint this whole seam exists to answer.
    struct ClickResult {
        bool hit = false;         ///< something was under the pointer
        bool changed = false;     ///< the selection changed, so redraw
        std::string message;
        /// How many things the aperture found, best first — the size of the Select Other list.
        ///
        /// Returned rather than left for the caller to ask separately, because asking costs a whole
        /// pick: the scene is re-rendered into the id buffer and read back. The iPad shell did ask
        /// separately, purely to report the count in a diagnostic, and so paid for every tap twice.
        std::size_t candidates = 0;
    };

    /// A click in the viewport, in DEVICE pixels. `additive` is the ctrl/shift-click behaviour.
    ///
    /// Clicking empty space with `additive` false clears the selection, which is what every CAD
    /// application does and what makes the model tree and the viewport agree.
    ///
    /// Equivalent to `tapAt` with a radius of zero, and kept because a mouse genuinely does point
    /// at one pixel.
    ClickResult clickAt(std::uint32_t x, std::uint32_t y, bool additive);

    /// One thing found under a pointer, ranked against the others.
    ///
    /// Carries a label because the only reason to hold the whole list is to SHOW it — SolidWorks's
    /// Select Other, Shapr3D's overlap pop-up — and a shell that had to resolve each entry back to
    /// a name would be re-deriving what this call already resolved.
    struct Candidate {
        document::ObjectId object{};
        naming::ElementName element;
        std::uint32_t slot = 0;
        PickKind kind = PickKind::Unknown;
        std::string label;        ///< "Box · Face", for a Select Other list
        std::uint64_t distanceSq = 0;
        float depth = 1.0f;
    };

    /// Everything within `radiusPixels` of a point, best first.
    ///
    /// **The shared answer to "a finger is not a mouse".** A tap covers ~88 device pixels on a
    /// Retina iPad — Apple's 44 pt minimum target — and inside that area there are normally several
    /// elements. Both shells call this and differ only in the radius they pass, which is what stops
    /// them disagreeing about what pointing means. See docs/design/SELECTION.md.
    ///
    /// Ranked by kind (vertex, then edge, then face), then screen distance, then depth. NOT by
    /// depth alone: the frontmost thing under a fingertip is nearly always a face, so depth-first
    /// ranking makes edges unselectable by touch.
    [[nodiscard]] std::vector<Candidate> candidatesAt(std::uint32_t x, std::uint32_t y,
                                                      std::uint32_t radiusPixels);

    /// A tap, in DEVICE pixels, with a finger-sized aperture. Selects the best candidate.
    ///
    /// The same rules as `clickAt` once something is chosen — including that tapping an already
    /// selected thing deselects it, which is the touch convention (Onshape: "tap to select, tap
    /// again to deselect") and falls out of `select` already being a toggle.
    ClickResult tapAt(std::uint32_t x, std::uint32_t y, std::uint32_t radiusPixels, bool additive);

    /// The same tap, resolved at a level other than the active one.
    ///
    /// What a double tap needs: the convention on a tablet is that one tap takes the face or edge
    /// under the pointer and two taps take the whole body. That is one gesture asking for a
    /// different LEVEL, not a different kind of pick, so it is a parameter rather than a mode the
    /// shell has to set and unset around the call.
    ClickResult tapAt(std::uint32_t x, std::uint32_t y, std::uint32_t radiusPixels, bool additive,
                      SelectionLevel level);

    /// The pointer moved to a point. Marks what is under it as hovered.
    ///
    /// Returns whether the highlight actually changed, so a shell can repaint on that instead of on
    /// every mouse-move event. Hover fires continuously; a redraw per event is the difference
    /// between a responsive viewport and a warm laptop.
    bool hoverAt(std::uint32_t x, std::uint32_t y);

    /// Hover with an aperture, ranked exactly as `tapAt` ranks a click.
    ///
    /// A shell should pass the same radius it passes to `tapAt`, so what lights up under the
    /// pointer is what a click would take. Passing zero is the old single-pixel behaviour.
    bool hoverAt(std::uint32_t x, std::uint32_t y, std::uint32_t radiusPixels);

    /// What is currently hovered, if anything. The absolute element slot.
    ///
    /// Exposed so a shell — or a test driving one — can see the pre-highlight the same way the
    /// renderer does. Without it "hover works" is only checkable by looking at pixels.
    [[nodiscard]] const std::optional<std::uint32_t>& hoveredElement() const noexcept {
        return hoveredSlot_;
    }

    /// Drops the hover highlight -- the pointer left the viewport.
    bool clearHover();

    /// Scripts the next pick, for tests and headless tools.
    ///
    /// The null backend's picker has no rasteriser on purpose (see NullBackend.h): what this layer
    /// owns is turning a slot into an `ElementName` and a plane, and whether a GPU writes the
    /// right ids into the id buffer needs a GPU to answer honestly. So the headless tests script
    /// the slot and assert on everything above it. Ignored once a real renderer is attached —
    /// otherwise a stray call in shipping code would fake a click.
    void scriptNextPick(std::uint32_t elementSlot, bool valid = true);

    /// Records "the user last clicked this element", as a Body-level click does internally.
    ///
    /// A test seam, beside `scriptNextPick` and for the same reason: the real path runs through the
    /// GPU pick buffer, which a headless test has no way to fill. It exercises what CONSUMES the
    /// last pick — Start Sketch choosing a face — rather than the pick itself, which face_pick.cpp
    /// covers separately.
    void scriptPickForTest(document::ObjectId, const naming::ElementName&);

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
    /// What a click or a tap does with what it found. Shared so the two cannot diverge.
    ClickResult applyPick(const Pick&, bool additive);
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

    /// Starts a sketch on one of the origin datums, by name.
    ///
    /// The EXPLICIT form of Start Sketch, for a caller that already knows which plane it wants — a
    /// test, a script, or a shell offering "sketch on XY" directly. `beginSketch` is the
    /// interactive form and deliberately asks when nothing is selected; this is how you say so
    /// without going through a selection.
    document::ObjectId beginSketchOn(sketch::Plane);

    /// Whether Start Sketch is waiting for the user to choose a plane or face.
    ///
    /// A real state rather than a shell flag, because both shells must behave the same and because
    /// the viewport needs to know that the next click means "sketch here" rather than "select
    /// this". See `sketchOnPickedPlane`.
    [[nodiscard]] bool awaitingSketchPlane() const noexcept { return awaitingSketchPlane_; }

    /// Starts the sketch on whatever flat face or plane is under a point, in DEVICE pixels.
    /// Does nothing unless `awaitingSketchPlane()`.
    document::ObjectId sketchOnPickedPlane(std::uint32_t x, std::uint32_t y);

    /// Gives up on choosing a plane — Escape.
    void cancelSketchPlanePick();

    /// Creates a Sketch feature and immediately edits it — what Start Sketch does.
    ///
    /// Sketches on the SELECTED planar face when there is one, and on XY otherwise. That is the
    /// order every CAD application uses: you pick the surface you want to draw on, then draw. The
    /// camera goes to that plane on entry and comes back to where it was when the sketch is
    /// finished — leaving the user looking at the plane afterwards means every sketch quietly
    /// changes the view they had arranged.
    document::ObjectId beginSketch();

    /// Creates a sketch placed on a named face of `body`, and selects it.
    ///
    /// The `body` is stored as an ObjectId property, which is what makes the face a real dependency
    /// rather than a string: the engine folds that feature's cache key into this one, so editing
    /// the face's own feature re-resolves the sketch instead of leaving it cached against where the
    /// face used to be.
    document::ObjectId addSketchOnFace(document::ObjectId body, const std::string& face);

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

    /// Where a click at these device pixels lands on the sketch being edited, in the sketch's own
    /// 2D coordinates.
    ///
    /// This is what makes sketching IN the viewport possible at all. `pickAt` answers "what did I
    /// click on" from the GPU pick buffer, which is empty wherever there is no geometry — and most
    /// of a sketch is drawn over empty space. This answers "where on the sketch plane is this
    /// pixel", which has an answer everywhere the plane is visible.
    ///
    /// Empty when there is no sketch being edited, or when the plane is edge-on: a ray parallel to
    /// the plane meets it nowhere, or nowhere useful, and returning a colossal coordinate would put
    /// a line the user cannot see into their sketch. The shell should refuse the click and say why
    /// rather than drawing something absurd.
    ///
    /// Device pixels, origin top-left — the same convention as `pickAt` and `clickAt`.
    [[nodiscard]] std::optional<std::array<double, 2>> sketchPointAt(float x, float y) const;

    /// Which drawing tool the next click in the sketch means.
    ///
    /// Model state rather than shell state, for the same reason the pick is: two shells must agree
    /// on what a click does, and a tool that lived in one shell's canvas would have to be
    /// reimplemented — and re-debugged — in the next one.
    /// Kept as an alias so `Controller::SketchTool::Line` still names the same thing for the two
    /// shells and every test. The tool itself belongs to SketchDrawing now.
    using SketchTool = SketchDrawing::Tool;

    [[nodiscard]] SketchTool sketchTool() const noexcept { return drawing_.tool(); }

    /// Switching tools ABANDONS a half-drawn shape. A line waiting for its second point is not
    /// something to carry into the circle tool, and silently keeping it is how a stray segment
    /// appears from a click the user made a minute ago.
    void setSketchTool(SketchTool);

    /// Ends the run of connected segments without leaving the tool.
    ///
    /// Escape, a double-click, or closing the loop. The TOOL stays active: ending a chain means
    /// "this run is finished", not "I am done drawing", and dropping back to Select would make
    /// every separate shape cost a trip to the toolbar.
    void endSketchChain();

    /// Removes all geometry from the sketch being edited. For tests and for a future Delete All;
    /// the seeded rectangle otherwise makes "is there a closed profile" true before a single click.
    void clearSketch();

    /// What a click at these device pixels does, given the current tool.
    ///
    /// Two-click tools keep their first point here rather than in the shell, so both shells behave
    /// identically and neither can forget to clear it. Returns whether the click was used: a click
    /// that missed the plane (edge-on, or outside the sketch environment) is refused rather than
    /// snapped to something arbitrary.
    bool sketchClickAt(float x, float y, bool additive = false);

    /// Which constraints the current sketch selection could take.
    ///
    /// The menu ADAPTS to the selection rather than offering everything and failing — Shapr3D's
    /// constraints menu "automatically highlights valid options based on your selected elements",
    /// and the alternative is a wall of buttons that mostly produce error messages.
    ///
    /// Computed from what is selected, so a shell that shows these cannot offer something the model
    /// would refuse.
    [[nodiscard]] std::vector<sketch::ConstraintKind> applicableConstraints() const;

    // Applying one is `applySketchConstraint`, below.

    /// Whether the path a pointer travelled is an arc rather than a straight drag.
    ///
    /// Asked by a shell on pen-up, to decide between committing the straight segment its rubber
    /// band has been promising and committing an arc. The tolerance is a hand's, derived from the
    /// camera here so the shells do not each need the camera to ask the question — and so "curved"
    /// means one thing in this application rather than one thing per shell.
    [[nodiscard]] bool strokeIsCurved(std::span<const std::array<float, 2>> devicePoints) const;

    /// The stroke SO FAR, in DEVICE pixels, drawn as ink while the pen is still down.
    ///
    /// A stroke is one gesture and its geometry only exists when the pen lifts — so without this
    /// the user draws blind and finds out what they drew afterwards. Every sketcher with a pen
    /// shows the trail; ours showed nothing, which is the single thing that made drawing on the
    /// tablet feel like guessing.
    ///
    /// The ink is the RAW path, not the fitted line or arc: showing a straightened preview would
    /// promise a shape the classifier has not decided on yet, and the promise would sometimes be
    /// wrong. Pass an empty span to clear it — a cancelled gesture must leave no trail.
    void showStrokeInk(std::span<const std::array<float, 2>> devicePoints);

    /// A whole pen stroke, in DEVICE pixels: the points the pointer travelled through.
    ///
    /// The stylus counterpart of `sketchClickAt`, and the same rules apply once the points are on
    /// the plane — snapping, chaining, inferred constraints, a solve per edit. What differs is only
    /// that one stroke makes one segment and the stroke's own shape chooses between a line and an
    /// arc (docs/design/SKETCHING_IPAD.md).
    ///
    /// Points that miss the plane are DROPPED rather than the stroke refused: a hand that crosses
    /// the silhouette of a solid mid-stroke has not stopped drawing. The stroke fails only when too
    /// few points survive to describe anything.
    bool sketchStrokeAt(std::span<const std::array<float, 2>> devicePoints);

    /// Tracks the pointer while a two-click shape is half-drawn, so the overlay can show what the
    /// shape WOULD be. Ignored when nothing is pending — the cursor means nothing before the first
    /// click, and following it then would draw a line from the origin to the mouse.
    ///
    /// Returns whether the preview changed, so a shell can skip a repaint on a mouse move that did
    /// not move far enough to matter.
    bool sketchHoverAt(float x, float y);

    /// What the half-drawn shape currently measures.
    ///
    /// Shown live while drawing, the way every CAD sketcher does it, so a user sees the size they
    /// are making rather than eyeballing it and dimensioning afterwards.
    struct PreviewMeasure {
        bool valid = false;
        bool circle = false;   ///< `length` is a radius rather than a length
        double length = 0.0;   ///< millimetres
        double angle = 0.0;    ///< degrees from the sketch's +u axis; meaningless for a circle
    };
    [[nodiscard]] PreviewMeasure sketchPreviewMeasure() const;

    /// The same measurement, formatted for display: length in the document's units, angle in
    /// degrees. Formatted HERE because the unit preference lives here — a shell printing raw
    /// millimetres would be showing a number the rest of the application does not use.
    struct PreviewText {
        bool valid = false;
        std::string length;   ///< "40 mm", or "R 12.5 mm" for a circle
        std::string angle;    ///< "-45.3°"; empty for a circle
    };
    [[nodiscard]] PreviewText sketchPreviewText() const;

    /// The number the user is typing to fix that measurement exactly.
    ///
    /// Kept on the model, not in a shell's text field, because it CHANGES WHAT THE NEXT CLICK
    /// MEANS: with a value typed, the shape is committed at that size rather than wherever the
    /// pointer happens to be, and gains a driving dimension. That is a modelling rule, and a shell
    /// holding it would have to reimplement the same rule to behave the same way.
    /// What the user has typed so far — for the drawing tools, or for a dimension being edited.
    ///
    /// One accessor for both, because the shell draws one readout and should not have to know which
    /// of the two is feeding it.
    [[nodiscard]] const std::string& sketchDimensionInput() const noexcept {
        return editingDimension_ ? dimensionInput_ : drawing_.input();
    }

    /// Appends a character, ignoring anything that cannot appear in a length. Returns whether it
    /// was taken, so a shell knows whether to swallow the keystroke or let it act as a shortcut.
    bool typeSketchDimension(char c);
    void backspaceSketchDimension();
    void clearSketchDimension();

    /// Locks the length, so the pointer only chooses the DIRECTION from here on.
    ///
    /// Tab, in every CAD sketcher. Without it a value the user has just decided keeps tracking the
    /// mouse and is lost the moment they move to aim — which is why the number is worth typing at
    /// all. Locks the typed text when there is any, and otherwise the length currently shown.
    ///
    /// Spent when the segment commits: a lock that survived would silently force every following
    /// line to the same length.
    bool lockSketchDimension();

    /// The locked length in millimetres, or nothing. A shell shows a lock beside the field.
    [[nodiscard]] const std::optional<double>& sketchLockedLength() const noexcept {
        return drawing_.lockedLength();
    }

    /// Commits the half-drawn shape at the typed size, as if the user had clicked at exactly that
    /// distance. False when nothing is pending or the text will not parse.
    bool commitSketchDimension();

    /// The pending first point of a two-click tool, for drawing a rubber band. Empty when the next
    /// click starts a shape rather than finishing one.
    [[nodiscard]] const std::optional<std::array<double, 2>>& sketchPending() const noexcept {
        return drawing_.pending();
    }

    /// The sketch being edited, as world-space line segments: x,y,z per endpoint, two endpoints
    /// per segment. Empty outside the sketch environment.
    ///
    /// Here rather than in the renderer because it needs the sketch's frame and the sketch's
    /// geometry, and `render/` must depend on neither — it receives floats. Here rather than in the
    /// shell because both shells need the same lines, and because a sketch being DRAWN is not in
    /// the document yet: nothing else renders it, so without this the user draws blind.
    ///
    /// Curves are flattened here too. A circle mid-edit does not need the kernel's tessellator and
    /// should not wait for a recompute to appear.
    [[nodiscard]] std::vector<float> sketchOverlayVertices() const;

    /// A digest of those vertices, for the renderer's revision check. Changes when the sketch
    /// changes and not when the camera moves, which is what keeps an orbit from re-uploading it.
    [[nodiscard]] std::uint64_t sketchOverlayRevision() const;

    /// The half-drawn shape, as DASHED world-space segments — the same layout, a separate list.
    ///
    /// Separate from the committed geometry because it is drawn differently: dimmer and dashed, so
    /// a proposal cannot be mistaken for a fact. Two batches rather than one, since the edge
    /// shader takes its colour per batch.
    [[nodiscard]] std::vector<float> sketchPreviewVertices() const;

    /// The sketch's curves as RIBBONS — triangles a few pixels wide — rather than line primitives.
    ///
    /// bgfx draws lines exactly one physical pixel wide and ignores `widthPx` entirely; there is no
    /// portable line-width control in modern graphics APIs. On a Retina display that makes a sketch
    /// a hairline that is genuinely hard to see, which is how this was reported.
    ///
    /// Width is computed from `worldPerPixel`, so the ribbon stays the same thickness on screen at
    /// any zoom — and is recomputed when the camera moves, which is why the revision below includes
    /// the camera scale.
    struct CurveMesh {
        std::vector<render::CadVertex> vertices;
        std::vector<std::uint32_t> indices;
    };
    [[nodiscard]] CurveMesh sketchCurveMesh(double widthPixels) const;
    [[nodiscard]] std::uint64_t sketchPreviewRevision() const;

    /// Hands the closed region of the sketch being edited to the scene, shaded.
    ///
    /// The signal a user needs BEFORE pressing Extrude: shading appears exactly when the curves
    /// form a profile and disappears the moment they do not. vCAD already computed this — a
    /// successful `toFace()` IS the test — and until now spent the answer on a red ERR afterwards.
    void pushSketchProfile();

    /// The outside facts SketchDrawing needs, gathered fresh for each call.
    [[nodiscard]] SketchDrawing::Context drawingContext() const;


    /// Whether a plain left drag orbits instead of selecting.
    ///
    /// A discoverable way to rotate the view. Orbit is otherwise on the middle button or on Alt,
    /// and neither is findable: a laptop has no middle button, and nobody discovers a modifier
    /// chord by looking at the screen. Every CAD application has had a visible navigation control
    /// for this reason — this is the state behind one.
    ///
    /// Model state, so both shells share it and a touch front end (which has no modifiers at all)
    /// has the same switch to flip.
    [[nodiscard]] bool orbitMode() const noexcept { return orbitMode_; }

    /// What a plain left press means right now: draw into the sketch, or navigate.
    ///
    /// A model rule rather than a chain of conditions in a shell's press handler, because getting
    /// the ORDER wrong is invisible: the shell checked the drawing tool first, so turning Orbit on
    /// while the Line tool was active did nothing at all — the press drew a point and the mode was
    /// never consulted. Stating it once, here, means both shells ask the same question and a test
    /// can pin the precedence down.
    ///
    /// Orbit mode wins. It is a mode the user turned on deliberately, and a mode that loses to
    /// whatever else happens to be active is not a mode.
    [[nodiscard]] bool leftPressDraws() const noexcept {
        return !orbitMode_ && environment_ == Environment::Sketch
               && drawing_.tool() != SketchTool::Select;
    }
    void setOrbitMode(bool);

    /// Cuts away everything between the camera and the sketch plane.
    ///
    /// Sketching on a face buried inside a part is otherwise done blind: the material in front of
    /// the plane hides both the face and what is being drawn on it. Fusion calls this Slice and
    /// puts it on the sketch palette; it is a VIEW state, so it changes nothing in the document and
    /// turns itself off when the sketch closes.
    [[nodiscard]] bool sliceEnabled() const noexcept { return slice_; }
    void setSliceEnabled(bool);

    /// Points the camera squarely at the sketch being edited, with the sketch's own v axis up.
    ///
    /// Called on entering the sketch environment. This is what "sketching happens in the same world
    /// as the model" means in practice: the camera moves to the plane, rather than the model being
    /// replaced by a 2D surface that shares none of its coordinates.
    void alignCameraToSketch();

    /// Puts the camera back where it was before the sketch opened. A no-op if nothing was saved.
    void restoreCameraAfterSketch();

    /// Pushes the slice plane to the scene, or clears it. Called whenever the view changes,
    /// because the plane's normal follows the camera.
    void applySlice();

    /// Hands the sketch being edited to the scene as an overlay, so it is visible while drawn.
    void pushSketchOverlay();

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

    /// The sketch curve nearest a point, in DEVICE pixels, or nothing.
    ///
    /// Hit testing in SKETCH space rather than on screen: the point is unprojected onto the plane
    /// first, so the same tolerance means the same thing whatever the camera is doing. Lives here
    /// rather than in a canvas because both shells need it and neither should own the rule.
    [[nodiscard]] std::optional<sketch::GeoId> sketchGeometryAt(float x, float y,
                                                                float radiusPixels) const;

    /// One dimension on a sketch curve, at the size it is currently drawn.
    ///
    /// A line gets a length, a circle or arc a radius. Created at the CURRENT value, which is what
    /// every CAD application does: the dimension records what you drew, and typing a new number is
    /// what changes the geometry. Creating it at zero would collapse the sketch the moment it
    /// solved.
    ///
    /// Returns the constraint's index, so a shell can hand it straight to `setSketchDimension`.
    [[nodiscard]] std::optional<std::size_t> dimensionSketchGeometry(sketch::GeoId);

    /// One dimension, placed where the shell should draw it.
    struct DimensionLabel {
        float x = 0.0f;            ///< DEVICE pixels, already projected
        float y = 0.0f;
        std::string text;          ///< formatted in the document's display units
        std::size_t constraint = 0;
        bool radius = false;       ///< a radius reads "R40"; a length reads "40 mm"
        /// A dimension of the shape being DRAWN, which does not exist yet.
        ///
        /// The live numbers a CAD shows on the edges while you drag — "100 mm" along the bottom of
        /// a rectangle and "80 mm" up its side. They are not constraints and cannot be clicked or
        /// edited; a shell must draw them and leave them alone. Distinguished by a flag rather than
        /// by a separate list because a shell places all dimensions the same way, and two lists
        /// would mean two placement routines that drift apart.
        bool preview = false;
    };

    /// Every dimension in the sketch being edited, projected to the viewport.
    ///
    /// Projected HERE rather than in the shell because the camera and the sketch's frame both live
    /// here, and a second shell would otherwise reimplement the same two transforms. What the shell
    /// adds is the drawing: an offset in screen pixels, a background, a font.
    ///
    /// Empty outside the sketch environment. Dimensions behind the camera are omitted rather than
    /// clamped, because a label pinned to the edge of the screen points at nothing.
    [[nodiscard]] std::vector<DimensionLabel> sketchDimensionLabels() const;

    /// A world point in DEVICE pixels, or nothing when it is behind the camera.
    [[nodiscard]] std::optional<std::array<float, 2>> projectToViewport(
        const std::array<double, 3>& world) const;

    /// Dimensions the sketch curve at a point, in DEVICE pixels, and opens it for typing.
    ///
    /// Created at the size the geometry ALREADY is, which is what every CAD application does: the
    /// dimension records what you drew, and typing a new number is what changes it. Created at zero
    /// it would collapse the sketch the instant it solved.
    ///
    /// After this the typed-dimension keys — digits, backspace, Enter — drive the new dimension
    /// rather than the drawing tools, so the shell needs no second text field.
    bool dimensionSketchAt(float x, float y);

    /// The dimension currently being typed into, if any.
    [[nodiscard]] std::optional<std::size_t> editingDimension() const noexcept {
        return editingDimension_;
    }

    /// Trims the sketch curve at a point, in DEVICE pixels.
    ///
    /// The click does two jobs: it chooses the curve, and it chooses WHICH span of it to remove —
    /// a curve crossed twice has three spans and only the click says which one was meant. See
    /// `sketch::trim` for the rules.
    ///
    /// Returns false when nothing was under the pointer or the trim was refused; the status line
    /// carries the reason either way.
    bool trimSketchAt(float x, float y);

    /// Changes a dimension and re-solves — the moment a sketch becomes parametric.
    bool setSketchDimension(std::size_t constraint, double millimetres);

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

    /// One exportable format, for a shell's file dialog.
    struct ExportFormat {
        std::string id;            ///< "step"
        std::string displayName;   ///< "STEP (AP203/214/242)"
        std::vector<std::string> extensions;
        bool solids = false;       ///< false means the format is mesh-only and B-rep is lost
    };

    /// Every format that can be WRITTEN. Asked of the registry rather than hard-coded, so a format
    /// compiled in conditionally (3MF) appears exactly when it is available.
    [[nodiscard]] static std::vector<ExportFormat> exportFormats();

    /// Writes the visible bodies to `path`, choosing the format by extension.
    ///
    /// What gets exported is a MODEL decision, not the shell's: the visible bodies, which is what
    /// the user is looking at. Not every object — a Box consumed by a Fillet is still in the
    /// document, and writing both would put the un-filleted block in the file alongside the real
    /// part. The tip-body rule that decides what to draw decides what to write, so the file matches
    /// the screen.
    ///
    /// Several bodies become one compound, because that is what "export this part" means when a
    /// part has more than one solid in it.
    bool exportDocument(const std::string& path);

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
    /// Creates the three origin datum planes a new part starts with.
    void seedOriginPlanes();

    document::ObjectId addSketch();



    /// Extrudes the selected Sketch feature into a solid.
    void addExtrude(double millimetres);

    /// Adds a two-input boolean over the current selection. Cut, Fuse and Common differ only in
    /// the feature type, so they share this rather than three copies of the same twelve lines.
    void addBoolean(const std::string& type, const std::string& label);

    /// Revolves the selected sketch about one of its own straight edges.
    ///
    /// The axis is picked as an EDGE, which chooses both inputs at once: the compute resolves the
    /// axis in the profile's element map, so it has to be an edge of the sketch being revolved.
    void addRevolve(double degrees);

    /// Moves the selected body. Millimetres, because the document is.
    void addTranslate(double dxMm, double dyMm, double dzMm);

    /// Drills a hole into the selected FACE, perpendicular to it and at its centre.
    ///
    /// A face rather than a body, because that is what the feature actually takes: the direction
    /// and the position both come from the face, which is why picking one is the whole input.
    void addHole(double diameterMm, double depthMm);

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
    /// The camera as it was before entering the sketch, restored on finish or cancel.
    ///
    /// A whole CameraController rather than a few angles: alignment, ortho height, target and the
    /// turntable angles all move when a sketch opens, and restoring a subset puts the user
    /// somewhere they never were, which is worse than not restoring at all.
    std::optional<render::CameraController> cameraBeforeSketch_;

    /// The line and circle tools: tool, chain, snapping, inference and dimension entry. Owns every
    /// piece of sketch-drawing state that used to be loose members of this class.
    SketchDrawing drawing_;

    bool slice_ = false;
    bool orbitMode_ = false;

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
    /// The constraint index a typed value will be applied to, when the Dimension tool placed one.
    ///
    /// Separate from the drawing tools' own typed input: that one sizes the segment being DRAWN and
    /// belongs to the chain, while this one edits a dimension that already exists. Sharing the
    /// buffer would make Escape mean two different things.
    /// The pen's trail while a stroke is in progress: world-space line pairs, cleared on commit.
    std::vector<float> strokeInk_;
    std::uint64_t strokeInkRevision_ = 0;
    bool awaitingSketchPlane_ = false;
    std::optional<std::size_t> editingDimension_;
    std::string dimensionInput_;

    sketch::SolveReport lastSketchSolve_;
    std::vector<sketch::GeoId> sketchSelection_;

    SelectionLevel selectionLevel_ = SelectionLevel::Body;
    std::vector<ElementSelection> elementSelection_;

    /// The element under the LAST click, whatever the selection level was.
    ///
    /// Body-level clicks select an object and throw the element away — correct for selection, and
    /// wrong for the question "which face is the user looking at". Start Sketch needs the second
    /// question answered: in every CAD application you click a face and press Sketch, without first
    /// telling a filter what kind of thing you are about to click.
    std::optional<ElementSelection> lastPicked_;
    /// The slot currently hovered, or none. Held so hoverAt can tell "same element as last time"
    /// from "moved to a new one" without rebuilding the highlight table.
    std::optional<std::uint32_t> hoveredSlot_;

    /// Pushes selection and hover into the scene's highlight table.
    void refreshHighlights();

    /// Every element slot the scene holds for an object. Used to highlight a whole body.
    [[nodiscard]] std::vector<std::uint32_t> slotsOf(document::ObjectId) const;

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

/// Free rather than a member, matching toString(Environment) above.
const char* toString(Controller::SelectionLevel) noexcept;

}  // namespace cad::app
