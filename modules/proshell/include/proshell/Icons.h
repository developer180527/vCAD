#pragma once

/// Vector icons, drawn from geometry at the requested size rather than loaded from files.
///
/// # Why the glyphs are not in here
///
/// An icon set is two separable things: the MACHINERY (allocate at the device pixel ratio, set up
/// a pen that scales with the size, hand back a `QIcon`) and the VOCABULARY (what a "fillet" looks
/// like). The machinery is identical in every application. The vocabulary never is — a CAD app
/// needs `extrude` and `loft`, an architecture app needs `wall` and `storey`, and neither has any
/// use for the other's.
///
/// So this library owns the machinery plus the handful of glyphs every professional application
/// genuinely shares — new, open, save, undo, redo, delete — and the application registers its own
/// vocabulary through `addProvider`. A library that shipped `extrude` would be a CAD library
/// wearing a generic name.
///
/// # Providers rather than a name-keyed registry
///
/// A provider is asked for a name and answers "I drew it" or "not mine", and providers are tried
/// most-recently-registered first. That is a slightly weaker contract than a map from name to
/// painter — nothing can enumerate the available glyphs — and it was chosen because it lets an
/// application register one function containing the `if/else` chain it already has, rather than
/// splitting sixty glyphs into sixty registrations to gain an enumeration nobody needs yet.
/// If a use appears for listing or overriding individual glyphs, a keyed registry is a compatible
/// addition on top of this.
///
/// Rendered at the *current* screen's device pixel ratio. Known limitation: an icon built on a 2x
/// display and then dragged to a 1x monitor is not re-rendered, because these are baked QPixmaps
/// created once. The real fix is a QIconEngine, which re-renders per paint and per ratio; worth
/// doing when an icon set becomes real, not before.

#include <QColor>
#include <QIcon>
#include <QString>

#include <functional>
#include <vector>

class QPainter;

namespace proshell {

/// Paints `name` at `size` logical points into an already-configured painter.
///
/// The painter arrives with antialiasing on, a pen of the right colour and width, and logical
/// coordinates — so a glyph body is pure geometry and never repeats the setup. Returns false, and
/// draws nothing, for a name this provider does not know.
using GlyphPainter = std::function<bool(QPainter&, const QString& name, int size)>;

/// The process-wide icon set.
class IconSet {
public:
    [[nodiscard]] static IconSet& instance();

    /// Adds a source of glyphs. Later providers are tried FIRST, so an application can override a
    /// built-in glyph by registering its own after startup rather than by patching this library.
    void addProvider(GlyphPainter);

    /// The named icon, or a neutral placeholder square when no provider claims the name.
    ///
    /// A placeholder rather than an empty icon, deliberately: a missing glyph should look like a
    /// missing glyph. An empty `QIcon` produces a button with a label and no image, which reads as
    /// an intentional design rather than as something to fix.
    [[nodiscard]] QIcon icon(const QString& name, int size = 32) const;

    /// The two colours glyphs are drawn in. The pen arrives set to `line`; `accent` is for the
    /// part of a glyph that carries its meaning — the cut in a `cut`, the marker in a `rollback`.
    /// Exposed so an application's provider matches the built-ins without hard-coding them.
    [[nodiscard]] QColor line() const noexcept { return line_; }
    [[nodiscard]] QColor accent() const noexcept { return accent_; }
    void setColours(QColor line, QColor accent) noexcept;

private:
    IconSet();

    std::vector<GlyphPainter> providers_;
    QColor line_;
    QColor accent_;
};

/// Shorthand for `IconSet::instance().icon(...)`, which is how nearly every call site wants it.
[[nodiscard]] inline QIcon icon(const QString& name, int size = 32) {
    return IconSet::instance().icon(name, size);
}

}  // namespace proshell
