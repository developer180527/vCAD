#pragma once

/// What the home page needs to know about an application.
///
/// # Why this is an interface and the frame was not
///
/// `ShellWindow` deliberately has no document model: it hands over widgets, because any interface
/// describing documents would have been shaped by the only application that exists. The home page
/// is the opposite case, and the difference is worth being explicit about.
///
/// A home page is not a container the application fills in — it is a finished screen with a
/// layout, a card grid, a search box, a sort control, an empty state and a rail, all of which are
/// settled interaction copied from Inventor. What varies between applications is not the
/// arrangement, it is four small pieces of DATA: what the product is called, what kinds of
/// document it makes, what was opened recently, and what it wants to say about the current
/// workspace. Those four are worth naming, because they are genuinely all of them.
///
/// So the test is not "is an interface good" but "do I know the whole surface". Here, yes.
///
/// # Nothing here is a domain type
///
/// Kinds are opaque integers the application casts back to its own enum; recent documents are
/// paths and icon names. The home page never learns what a Part is, and an application whose
/// documents are city blocks passes its own enum through the same `int`.

#include <QString>

#include <vector>

namespace proshell {

/// One kind of document the application can create.
struct DocumentKind {
    /// The application's own enumerator, cast to int. Opaque here; handed straight back on
    /// `HomePage::createRequested`.
    int id = 0;
    QString label;
    /// Glyph name, resolved through the icon set. Empty falls back to the lowercased label.
    QString iconName;
    /// False for a kind the application intends to have and does not yet.
    ///
    /// Shown anyway, greyed and marked. A user should be able to see the application's intended
    /// shape rather than wonder whether a kind exists, and hiding what is coming makes every
    /// later release rearrange the page under them.
    bool available = true;
};

/// One entry in the recent list.
struct RecentDocument {
    /// Absolute path. Opened as-is, and its filename is what the card shows.
    QString path;
    /// Glyph standing in for a thumbnail until there is something to render.
    QString iconName;
};

/// One segment of the strip along the top of the home content column.
struct SummaryField {
    QString text;

    /// A field whose value is a STATE — configured or not, connected or not — rather than a fact.
    /// Rendered in the status style and coloured when `on`.
    bool isStatus = false;
    bool on = false;
};

/// Implemented by the application. Every method is called on `HomePage::refresh()`, so an
/// implementation should be cheap rather than cached.
class HomeModel {
public:
    virtual ~HomeModel() = default;

    /// Large, at the top of the rail. "vCAD 0.1".
    [[nodiscard]] virtual QString productName() const = 0;
    /// Small, under it. A milestone, a build, an edition — or nothing.
    [[nodiscard]] virtual QString productDetail() const { return {}; }

    /// In the order they should appear. The first AVAILABLE kind is what the New button creates
    /// when it is clicked rather than dropped down, so put the everyday one first.
    [[nodiscard]] virtual std::vector<DocumentKind> documentKinds() const = 0;

    /// Most recent first. The page does its own filtering and sorting on top of this order.
    [[nodiscard]] virtual std::vector<RecentDocument> recent() const = 0;

    /// The strip above the recent list. Empty hides it entirely, which is the right result for an
    /// application with nothing to say there.
    [[nodiscard]] virtual std::vector<SummaryField> summary() const { return {}; }
};

}  // namespace proshell
