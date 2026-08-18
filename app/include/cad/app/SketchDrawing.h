#pragma once

/// The line and circle tools: what a click MEANS while a sketch is open.
///
/// # Why this is its own type
///
/// This state — the tool, the point a chain is waiting on, where the pointer is, the number being
/// typed, whether that number is locked — accumulated inside `Controller` until it was 24 members
/// of a class that is also the document, the camera, the selection, the command catalogue and the
/// exporter. Sketch interaction is a mode with its own rules and its own lifetime, and reading those
/// rules meant reading past everything else.
///
/// # The boundary, which is the useful part
///
/// This class works entirely in the sketch's own 2D coordinates and knows nothing about pixels, the
/// camera, the scene or the document. The Controller converts a click into `(u, v)` — that needs a
/// camera ray — and hands the result here; what comes back is a decision, not a drawing.
///
/// So the split is: **the Controller knows where the pointer is in the world, this knows what
/// pointing there means.** Everything that made the old code hard to follow was those two mixed
/// together.
///
/// # Context rather than a back-pointer
///
/// The few outside facts a decision needs — which sketch, how big a pixel is, which units the user
/// reads — arrive as a `Context` per call. A pointer back to the Controller would have been less
/// code and would have rebuilt exactly the tangle this extraction exists to undo: every method able
/// to reach anything, so no method's dependencies are visible.

#include "cad/sketch/Sketch.h"
#include "cad/units/Units.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cad::app {

class SketchDrawing {
public:
    /// Which drawing tool the next click means.
    enum class Tool : std::uint8_t { Select, Line, Circle };

    /// A point in the sketch's own 2D coordinates, in millimetres.
    using Point = std::array<double, 2>;

    /// What a decision needs to know about the world outside this class.
    ///
    /// Assembled by the caller for each call rather than stored, because every field of it can
    /// change between two clicks: the user can zoom, change units, or edit a different sketch.
    /// Holding a stale copy is the bug this shape makes impossible.
    struct Context {
        sketch::Sketch* sketch = nullptr;
        /// World units per screen pixel. Snapping and dash length are specified in PIXELS, because
        /// a hand is steady in pixels — a millimetre tolerance snaps from across the screen when
        /// zoomed out and never snaps at all when zoomed in.
        double worldPerPixel = 1.0;
        units::UnitSystem displayUnits = units::UnitSystem::Millimetre;
    };

    /// What the half-drawn shape currently measures.
    struct Measure {
        bool valid = false;
        bool circle = false;   ///< `length` is a radius rather than a length
        double length = 0.0;   ///< millimetres, or the locked value when one is set
        double angle = 0.0;    ///< degrees from the sketch's +u axis; meaningless for a circle
    };

    /// The same measurement formatted for a person: length in the document's units.
    struct Text {
        bool valid = false;
        std::string length;   ///< "40 mm", or "R 12.5 mm" for a circle
        std::string angle;    ///< "-45.3°"; empty for a circle
    };

    /// What a call did, so the caller knows whether to solve, redraw and what to say.
    ///
    /// Returned rather than done here: solving the sketch, pushing overlays and setting the status
    /// bar are the Controller's, and a class that reached out to do them would be back to needing
    /// the back-pointer this one does without.
    struct Outcome {
        bool used = false;             ///< the input was consumed; a shell should not treat it as a shortcut
        bool geometryChanged = false;  ///< the sketch was edited, so it needs re-solving
        std::string status;            ///< empty when there is nothing worth saying
    };

    [[nodiscard]] Tool tool() const noexcept { return tool_; }

    /// Switching tools ABANDONS a half-drawn shape. A line waiting for its second point means
    /// nothing to the circle tool, and keeping it is how a stray segment appears from a click made
    /// a minute ago.
    void setTool(Tool);

    /// The point a chain is waiting on, for drawing a rubber band. Empty when the next click starts
    /// a shape rather than continuing one.
    [[nodiscard]] const std::optional<Point>& pending() const noexcept { return pending_; }

    /// Where the pointer is, while `pending()` is set.
    [[nodiscard]] const std::optional<Point>& hover() const noexcept { return hover_; }

    /// The number being typed to fix the size exactly.
    [[nodiscard]] const std::string& input() const noexcept { return input_; }

    /// The length fixed by Tab, in millimetres. A shell shows a padlock beside the field.
    [[nodiscard]] const std::optional<double>& lockedLength() const noexcept { return locked_; }

    /// Where the shape would END if clicked now: the pointer, or the pointer's DIRECTION at the
    /// locked distance. What the rubber band must draw, so the preview cannot promise one length
    /// and commit another.
    [[nodiscard]] std::optional<Point> aimed() const;

    /// A click at `at`, already mapped into sketch coordinates.
    ///
    /// Snaps first, so the point that ends one segment and the point that starts the next are the
    /// same point when they should be. Without that a click lands NEAR the previous endpoint, the
    /// segments do not meet, and no amount of careful aiming closes a profile.
    Outcome click(const Context&, Point at);

    /// Tracks the pointer while a shape is half-drawn. Returns whether the preview moved enough to
    /// be worth a repaint.
    bool hover(const Context&, Point at);

    /// Ends the run of connected segments without leaving the tool. Escape, a double-click, or
    /// closing the loop. Ending a chain means "this run is finished", not "I am done drawing".
    void endChain();

    /// Appends a character, ignoring anything that cannot appear in a length. Returns whether it was
    /// taken, so a shell knows whether to swallow the keystroke or let it act as a shortcut.
    bool type(char);
    void backspace();
    void clearInput();

    /// Locks the length, so the pointer only chooses direction from here on — Tab. Locks the typed
    /// text when there is any, otherwise the length currently shown.
    bool lock(const Context&);

    /// Commits the half-drawn shape at the typed size, as if the user had clicked at exactly that
    /// distance, with a driving dimension.
    Outcome commitTyped(const Context&);

    /// Drops every curve from the sketch and ends the chain.
    void clear(const Context&);

    [[nodiscard]] Measure measure() const;
    [[nodiscard]] Text text(units::UnitSystem) const;

    /// The rubber band, as dashed segments in sketch coordinates: pairs of points, two per dash.
    /// Empty when nothing is half-drawn.
    [[nodiscard]] std::vector<Point> previewSegments(const Context&) const;

private:
    /// The nearest snappable point within `kSnapPixels` of `at`, or nothing.
    [[nodiscard]] std::optional<Point> snap(const Context&, Point at) const;

    /// Whether a click here brings the chain back onto itself.
    [[nodiscard]] bool closesLoop(const Context&, Point at) const;

    /// Applies the constraint a hand was obviously aiming for — horizontal or vertical.
    void infer(const Context&, sketch::GeoId id, Point from, Point to) const;

    Tool tool_ = Tool::Select;
    std::optional<Point> pending_;
    std::optional<Point> hover_;
    std::string input_;
    std::optional<double> locked_;

    /// How close, in pixels, a click has to be to snap.
    static constexpr double kSnapPixels = 10.0;
};

}  // namespace cad::app
