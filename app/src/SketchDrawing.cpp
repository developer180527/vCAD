#include "cad/app/SketchDrawing.h"

#include <cmath>
#include <vector>
#include <numbers>

namespace cad::app {

void SketchDrawing::setTool(Tool next) {
    if (tool_ == next) return;
    tool_ = next;
    endChain();
}

void SketchDrawing::closeChain(const Context& ctx) {
    if (ctx.sketch == nullptr || !lastGeo_ || !firstGeo_ || *lastGeo_ == *firstGeo_) return;
    ctx.sketch->coincident(*lastGeo_, lastPoint_, *firstGeo_, firstPoint_);
}

void SketchDrawing::endChain() {
    pending_.reset();
    hover_.reset();
    input_.clear();
    // The chain's last geometry goes with it. Kept, the NEXT run's first segment would be
    // constrained coincident to the end of a run the user has already finished — an invisible link
    // between two shapes that look separate, which the solver would then enforce.
    lastGeo_.reset();
    firstGeo_.reset();
    // The lock belonged to the run that just ended. Carried over, it would size a segment the user
    // never asked about.
    locked_.reset();
}

void SketchDrawing::clear(const Context& ctx) {
    if (ctx.sketch == nullptr) return;
    *ctx.sketch = sketch::Sketch(ctx.sketch->plane());
    endChain();
}

std::optional<SketchDrawing::Point> SketchDrawing::aimed() const {
    if (!pending_ || !hover_) return std::nullopt;
    const double dx = (*hover_)[0] - (*pending_)[0];
    const double dy = (*hover_)[1] - (*pending_)[1];
    const double length = std::sqrt(dx * dx + dy * dy);
    if (!locked_ || length < 1e-9) return hover_;
    const double scale = *locked_ / length;
    return Point{(*pending_)[0] + dx * scale, (*pending_)[1] + dy * scale};
}

std::optional<SketchDrawing::Point> SketchDrawing::snap(const Context& ctx, Point at) const {
    if (ctx.sketch == nullptr) return std::nullopt;

    // A tolerance in PIXELS converted to sketch units. A fixed millimetre tolerance snaps from
    // across the screen when zoomed out and never snaps when zoomed in.
    const double tolerance = ctx.worldPerPixel * kSnapPixels;

    std::optional<Point> best;
    double nearest = tolerance;
    const auto consider = [&](double u, double v) {
        const double dx = u - at[0];
        const double dy = v - at[1];
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= nearest) {
            nearest = distance;
            best = Point{u, v};
        }
    };

    for (const auto& g : ctx.sketch->geometry()) {
        switch (g.kind) {
            case sketch::GeoKind::Line:
                consider(g.p[0], g.p[1]);
                consider(g.p[2], g.p[3]);
                break;
            case sketch::GeoKind::Circle:
            case sketch::GeoKind::Arc:
                consider(g.p[0], g.p[1]);   // the centre
                break;
            case sketch::GeoKind::Point:
                consider(g.p[0], g.p[1]);
                break;
        }
    }
    // The sketch origin, which is what a user aims at to anchor a shape.
    consider(0.0, 0.0);
    return best;
}

bool SketchDrawing::closesLoop(const Context& ctx, Point at) const {
    if (ctx.sketch == nullptr) return false;
    // Exact, because the point has already been snapped: a loop closes when the click landed ON an
    // existing endpoint, not merely near one.
    int touching = 0;
    for (const auto& g : ctx.sketch->geometry()) {
        if (g.kind != sketch::GeoKind::Line) continue;
        if (std::abs(g.p[0] - at[0]) < 1e-9 && std::abs(g.p[1] - at[1]) < 1e-9) ++touching;
        if (std::abs(g.p[2] - at[0]) < 1e-9 && std::abs(g.p[3] - at[1]) < 1e-9) ++touching;
    }
    // Two segments meeting here means the run has come back on itself. One is just the segment that
    // was only this moment drawn.
    return touching >= 2;
}

void SketchDrawing::infer(const Context& ctx, sketch::GeoId id, Point from, Point to) const {
    if (ctx.sketch == nullptr) return;

    // Horizontal and vertical only. They are the two a hand aims at constantly, and the two whose
    // absence leaves an otherwise careful sketch under-constrained — which is the difference between
    // a parametric sketch and a drawing.
    const double dx = std::abs(to[0] - from[0]);
    const double dy = std::abs(to[1] - from[1]);
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-9) return;

    // A few degrees of tolerance: a user aiming at horizontal misses by a pixel or two, and a rule
    // that only fires on an exact match never fires at all.
    constexpr double kTolerance = 0.05;   // sin of ~3 degrees
    if (dy / length < kTolerance) {
        ctx.sketch->horizontal(id);
    } else if (dx / length < kTolerance) {
        ctx.sketch->vertical(id);
    }
}

void SketchDrawing::join(const Context& ctx, sketch::GeoId next, sketch::PointRef nextPoint,
                         bool smooth) {
    // The first segment of a run has nothing to join to, and that is exactly when to remember where
    // the run began — every path that extends a chain comes through here.
    if (!lastGeo_) {
        firstGeo_ = next;
        firstPoint_ = nextPoint;
    }
    if (ctx.sketch == nullptr || !lastGeo_) return;
    ctx.sketch->coincident(*lastGeo_, lastPoint_, next, nextPoint);

    // Tangency when an ARC joins the chain — Shapr3D applies exactly this, and only then.
    //
    // Not for line-to-line: two straight segments meeting tangentially are one straight segment,
    // so the constraint would either be redundant or would flatten a corner the user drew on
    // purpose. It is the arc that carries the intent, because an arc leaving a line at an angle is
    // almost always a mistake and an arc leaving it smoothly is almost always a fillet.
    if (!smooth) return;
    const auto* previous = ctx.sketch->find(*lastGeo_);
    const auto* current = ctx.sketch->find(next);
    if (previous == nullptr || current == nullptr) return;
    if (previous->kind == sketch::GeoKind::Point || current->kind == sketch::GeoKind::Point) return;
    ctx.sketch->tangent(*lastGeo_, lastPoint_, next, nextPoint);
}

bool SketchDrawing::addRectangle(const Context& ctx, Point a, Point b) {
    if (ctx.sketch == nullptr) return false;

    // A locked length sizes the DIAGONAL, because that is the number the preview is showing.
    //
    // A rectangle has two dimensions and Tab supplies one, so something has to decide what the one
    // number means. `measure()` already reports the corner-to-corner distance and its angle for
    // this tool, and its own comment says the rubber band and the result must not disagree — so the
    // locked value is the thing on screen, and the drag direction still chooses the proportions.
    //
    // Honouring it at all is the fix: the lock used to be dropped in silence by endChain(), which
    // is the same "the padlock the shell was showing meant nothing" bug already fixed for strokes.
    if (locked_) {
        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double drawn = std::hypot(dx, dy);
        if (drawn > 1e-9) {
            const double scale = *locked_ / drawn;
            b = Point{a[0] + dx * scale, a[1] + dy * scale};
        }
    }

    // Degenerate in either axis is not a rectangle. Refused rather than drawn, because a
    // zero-width one is four invisible lines the user can neither see nor select. Checked AFTER the
    // scaling above, which cannot rescue a drag that was already flat in one axis but must not be
    // allowed to introduce one either.
    if (std::abs(b[0] - a[0]) < 1e-9 || std::abs(b[1] - a[1]) < 1e-9) return false;

    // Corners in order, so consecutive lines share an endpoint and the run closes.
    const Point corners[4]{{a[0], a[1]}, {b[0], a[1]}, {b[0], b[1]}, {a[0], b[1]}};
    sketch::GeoId ids[4]{};
    for (int i = 0; i < 4; ++i) {
        const Point& from = corners[i];
        const Point& to = corners[(i + 1) % 4];
        ids[i] = ctx.sketch->addLine(from[0], from[1], to[0], to[1]);
        // Horizontal and vertical by CONSTRUCTION, not by inference: the tool knows which is which,
        // so there is no tolerance to get wrong and no chance of a rectangle that is a degree off.
        if (i % 2 == 0) {
            ctx.sketch->horizontal(ids[i]);
        } else {
            ctx.sketch->vertical(ids[i]);
        }
    }
    // Four corners, four coincidences — including the last back to the first, which is what keeps
    // the profile closed when a dimension moves later.
    for (int i = 0; i < 4; ++i) {
        ctx.sketch->coincident(ids[i], sketch::PointRef::End, ids[(i + 1) % 4],
                               sketch::PointRef::Start);
    }

    if (locked_) {
        // DRIVING, for the same reason the line path gives: a locked length the solver may undo was
        // never locked. Corner 0 is ids[0]'s start and corner 2 is ids[2]'s start, so this is the
        // diagonal the scaling above produced. It leaves three degrees of freedom — position and
        // one proportion — which is a rectangle of a fixed size, not an over-constrained one.
        ctx.sketch->distance(ids[0], sketch::PointRef::Start, ids[2], sketch::PointRef::Start,
                             *locked_);
        locked_.reset();
    }
    return true;
}

SketchDrawing::Outcome SketchDrawing::stroke(const Context& ctx, std::span<const Point> points) {
    Outcome out;
    if (ctx.sketch == nullptr || points.size() < 2) return out;
    // Trim and Dimension act on geometry that already exists, so they are driven by a CLICK and a
    // stroke means nothing to them. Left out, they would fall through to the line/arc branch below
    // and draw — the same silent mismatch the Circle tool had, where the tool the user chose was
    // ignored and the status bar reported "Line".
    if (tool_ == Tool::Select || tool_ == Tool::Trim || tool_ == Tool::Dimension) return out;

    // The tolerance is the HAND's, in pixels, converted here. A wobble of a few pixels is not an
    // arc at any zoom; a fixed millimetre tolerance would call everything an arc when zoomed out
    // and everything a line when zoomed in.
    // EIGHT pixels, not four. Four is a mouse tolerance; a stylus on glass, moving at speed, wobbles
    // further than that and every one of those wobbles used to become an arc.
    constexpr double kStraightPixels = 8.0;
    const double tolerance = ctx.worldPerPixel * kStraightPixels;

    // Endpoints snapped, interior points left alone. The ends are what joins to other geometry;
    // the middle only decides the shape, and snapping it would drag the fit towards whatever
    // happened to be nearby.
    std::vector<Point> sampled(points.begin(), points.end());
    if (const auto snapped = snap(ctx, sampled.front())) sampled.front() = *snapped;
    if (const auto snapped = snap(ctx, sampled.back())) sampled.back() = *snapped;

    // A chain in progress wins over the stroke's own start: lifting the pen and starting the next
    // stroke a little away from the last endpoint must still continue the chain, or a hand that is
    // one pixel off produces an open profile — the failure MODELLING_UX.md §2b documents.
    if (pending_) sampled.front() = *pending_;

    const StrokeFit fit = fitStroke(sampled, tolerance);
    out.used = true;

    if (fit.kind == StrokeKind::Nothing) {
        out.status = "That stroke was too short to draw anything.";
        return out;
    }

    // The RECTANGLE tool means a rectangle, from the stroke's two ends — one drag, which is what
    // every other CAD application makes it and what four separate lines are not.
    if (tool_ == Tool::Rectangle) {
        if (!addRectangle(ctx, fit.start, fit.end)) {
            out.status = "A rectangle needs two opposite corners.";
            return out;
        }
        endChain();
        out.geometryChanged = true;
        out.status = "Rectangle";
        return out;
    }

    // The CIRCLE tool means a circle, whatever shape the stroke was.
    //
    // Only Select was rejected above, so a stroke drawn with the circle tool active silently came
    // out as a line — the tool the user chose ignored, and the status bar cheerfully reporting
    // "Line". Dragged from the centre outwards, which is the same thing the two-click form means.
    if (tool_ == Tool::Circle) {
        const double radius = std::hypot(fit.end[0] - fit.start[0], fit.end[1] - fit.start[1]);
        if (radius < 1e-9) {
            out.status = "A circle needs a radius.";
            return out;
        }
        const auto id = ctx.sketch->addCircle(fit.start[0], fit.start[1], radius);
        if (locked_) {
            ctx.sketch->radius(id, *locked_);
            locked_.reset();
        }
        endChain();
        out.geometryChanged = true;
        out.status = "Circle";
        return out;
    }

    // Where the segment actually ENDED, which the locked-length branch below may move away from
    // the stroke's own last point. The chain continues from here.
    Point end = fit.end;

    if (fit.kind == StrokeKind::Arc) {
        const auto id = ctx.sketch->addArc(fit.centre[0], fit.centre[1], fit.radius, fit.startAngle,
                                           fit.endAngle);
        // Which END of the arc the stroke started at, since `fitStroke` may have swapped them to
        // express a clockwise stroke as a counter-clockwise arc. Joining the wrong one connects the
        // chain to the far end and the profile crosses itself.
        const bool startIsFirst =
            std::hypot(fit.centre[0] + fit.radius * std::cos(fit.startAngle) - fit.start[0],
                       fit.centre[1] + fit.radius * std::sin(fit.startAngle) - fit.start[1])
            < std::hypot(fit.centre[0] + fit.radius * std::cos(fit.endAngle) - fit.start[0],
                         fit.centre[1] + fit.radius * std::sin(fit.endAngle) - fit.start[1]);
        join(ctx, id, startIsFirst ? sketch::PointRef::Start : sketch::PointRef::End,
             /*smooth=*/true);
        lastGeo_ = id;
        lastPoint_ = startIsFirst ? sketch::PointRef::End : sketch::PointRef::Start;
        out.status = "Arc";
    } else {
        // A LOCKED length applies to a stroke exactly as it applies to a click: the hand chose the
        // direction, Tab chose the size. Dropping it silently — which is what this did — made the
        // padlock the shell was showing mean nothing.
        Point at = fit.end;
        const double drawn = std::hypot(at[0] - fit.start[0], at[1] - fit.start[1]);
        if (locked_ && drawn > 1e-9) {
            const double scale = *locked_ / drawn;
            at = Point{fit.start[0] + (at[0] - fit.start[0]) * scale,
                       fit.start[1] + (at[1] - fit.start[1]) * scale};
        }

        const auto id = ctx.sketch->addLine(fit.start[0], fit.start[1], at[0], at[1]);
        infer(ctx, id, fit.start, at);
        join(ctx, id, sketch::PointRef::Start);
        lastGeo_ = id;
        lastPoint_ = sketch::PointRef::End;
        if (locked_) {
            // DRIVING, as in click(): a locked length the solver may undo was never locked.
            ctx.sketch->distance(id, sketch::PointRef::Start, id, sketch::PointRef::End, *locked_);
            locked_.reset();
        }
        end = at;
        out.status = "Line";
    }

    out.geometryChanged = true;
    pending_ = end;
    hover_ = end;
    input_.clear();

    if (closesLoop(ctx, end)) {
        // The constraint that makes "closed" survive an edit, not merely look right today.
        closeChain(ctx);
        endChain();
        out.status += ": profile closed";
    }
    return out;
}

SketchDrawing::Outcome SketchDrawing::click(const Context& ctx, Point at) {
    Outcome out;
    if (ctx.sketch == nullptr || tool_ == Tool::Select) return out;

    // Snapped BEFORE anything else uses it, so the point that starts a segment and the point that
    // ends one are the same point when they should be.
    if (const auto snapped = snap(ctx, at)) at = *snapped;

    out.used = true;

    if (!pending_) {
        pending_ = at;
        hover_ = at;
        out.status = tool_ == Tool::Line ? "Line: click the next point, Escape to finish"
                                        : "Circle: click to set the radius";
        return out;
    }

    const Point first = *pending_;
    double dx = at[0] - first[0];
    double dy = at[1] - first[1];
    double length = std::sqrt(dx * dx + dy * dy);

    // A locked length moves the click along the direction it was aiming: the pointer chose the
    // heading, Tab chose the size.
    if (locked_ && length > 1e-9) {
        const double scale = *locked_ / length;
        at = Point{first[0] + dx * scale, first[1] + dy * scale};
        dx *= scale;
        dy *= scale;
        length = *locked_;
    }

    if (length < 1e-9) {
        // A second click on the same point is how a user says "done" with the mouse alone.
        endChain();
        return out;
    }

    if (tool_ == Tool::Rectangle) {
        if (!addRectangle(ctx, first, at)) {
            out.status = "A rectangle needs two opposite corners.";
            return out;
        }
        endChain();
        out.geometryChanged = true;
        out.status = "Rectangle";
        return out;
    }

    if (tool_ == Tool::Circle) {
        ctx.sketch->addCircle(first[0], first[1], length);
        endChain();
    } else {
        const auto id = ctx.sketch->addLine(first[0], first[1], at[0], at[1]);
        infer(ctx, id, first, at);
        // The same join a stroke makes. Clicking and drawing are two ways to extend ONE chain, so
        // they must leave it in the same state — otherwise a click after a stroke silently loses
        // the connection and the profile will not close.
        join(ctx, id, sketch::PointRef::Start);
        lastGeo_ = id;
        lastPoint_ = sketch::PointRef::End;
        if (locked_) {
            // DRIVING, like a typed dimension. A locked length the solver may undo was never
            // locked — it was a coincidence that held until something moved.
            ctx.sketch->distance(id, sketch::PointRef::Start, id, sketch::PointRef::End, *locked_);
            locked_.reset();
        }

        // CHAINING: the endpoint becomes the next segment's start. Click, click, click draws a
        // connected run, which is the whole behaviour of a CAD line tool.
        pending_ = at;
        hover_ = at;
        input_.clear();

        // Back onto a point the chain already used: the loop is closed and the run is over. Checked
        // after the segment is added, so the closing segment itself is drawn.
        if (closesLoop(ctx, at)) {
            closeChain(ctx);
            endChain();
        }
    }

    out.geometryChanged = true;
    return out;
}

bool SketchDrawing::hover(const Context& ctx, Point at) {
    // Only while a shape is half-drawn. Before the first click the pointer says nothing about what
    // is being made, and following it anyway would draw a band from the origin to the mouse.
    if (!pending_ || ctx.sketch == nullptr) return false;
    if (hover_ && std::abs((*hover_)[0] - at[0]) < 1e-9 && std::abs((*hover_)[1] - at[1]) < 1e-9) {
        return false;   // no movement worth a repaint
    }
    hover_ = at;
    return true;
}

bool SketchDrawing::type(char c) {
    // Only while something is pending: a digit typed with nothing half-drawn is a shortcut, not a
    // dimension, and swallowing it would make the keyboard feel dead.
    if (!pending_) return false;
    const bool digit = c >= '0' && c <= '9';
    const bool separator = c == '.' || c == ',';
    // Unit letters are accepted so "12mm" parses; units::parseLength decides what is actually
    // valid, and it is the one place that knows.
    const bool unit = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!digit && !separator && !unit) return false;
    input_.push_back(c);
    return true;
}

void SketchDrawing::backspace() {
    if (!input_.empty()) input_.pop_back();
}

void SketchDrawing::clearInput() { input_.clear(); }

bool SketchDrawing::lock(const Context& ctx) {
    if (!pending_ || !hover_) return false;

    if (!input_.empty()) {
        auto parsed = units::parseLength(input_, ctx.displayUnits);
        // Text left alone rather than cleared, so the user can fix a typo instead of retyping.
        if (!parsed || !(parsed.value().base() > 0.0)) return false;
        locked_ = parsed.value().base();
        input_.clear();
        return true;
    }

    // Tab on an empty field locks what is currently SHOWN, which is what a user means when they
    // have dragged to roughly the right size and want it to stop moving.
    const Measure current = measure();
    if (!current.valid || !(current.length > 0.0)) return false;
    locked_ = current.length;
    return true;
}

SketchDrawing::Outcome SketchDrawing::commitTyped(const Context& ctx) {
    Outcome out;
    if (!pending_ || !hover_ || input_.empty() || ctx.sketch == nullptr) return out;

    auto parsed = units::parseLength(input_, ctx.displayUnits);
    if (!parsed) {
        out.status = parsed.error().message;
        return out;
    }
    const double value = parsed.value().base();
    if (!(value > 0.0)) {
        out.status = "A dimension has to be greater than zero.";
        return out;
    }

    // The DIRECTION comes from the pointer and the SIZE from the number. That is what makes typing
    // feel like drawing: you aim with the mouse and say how far with the keyboard.
    const double dx = (*hover_)[0] - (*pending_)[0];
    const double dy = (*hover_)[1] - (*pending_)[1];
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-9) {
        out.status = "Point the cursor in the direction first.";
        return out;
    }

    const Point first = *pending_;
    const Point at{first[0] + dx / length * value, first[1] + dy / length * value};

    if (tool_ == Tool::Circle) {
        const auto id = ctx.sketch->addCircle(first[0], first[1], value);
        // A DRIVING dimension, not merely geometry that happens to be this size. Without the
        // constraint the number is forgotten the instant anything else moves.
        ctx.sketch->radius(id, value);
        endChain();
    } else {
        const auto id = ctx.sketch->addLine(first[0], first[1], at[0], at[1]);
        infer(ctx, id, first, at);
        // The same join a stroke makes. Clicking and drawing are two ways to extend ONE chain, so
        // they must leave it in the same state — otherwise a click after a stroke silently loses
        // the connection and the profile will not close.
        join(ctx, id, sketch::PointRef::Start);
        lastGeo_ = id;
        lastPoint_ = sketch::PointRef::End;
        ctx.sketch->distance(id, sketch::PointRef::Start, id, sketch::PointRef::End, value);
        // Typing a length ends the segment but CONTINUES the chain, exactly as a click does.
        pending_ = at;
        hover_ = at;
        input_.clear();
        locked_.reset();
        if (closesLoop(ctx, at)) endChain();
    }

    out.used = true;
    out.geometryChanged = true;
    return out;
}

SketchDrawing::Measure SketchDrawing::measure() const {
    Measure out;
    if (!pending_ || !hover_) return out;

    const double dx = (*hover_)[0] - (*pending_)[0];
    const double dy = (*hover_)[1] - (*pending_)[1];
    // The lock wins: that is what locking means, and the rubber band has to show the same or the
    // preview and the result would disagree.
    out.length = locked_ ? *locked_ : std::sqrt(dx * dx + dy * dy);
    out.circle = tool_ == Tool::Circle;
    out.rectangle = tool_ == Tool::Rectangle;
    if (out.rectangle) {
        out.width = std::abs(dx);
        out.height = std::abs(dy);
    }
    // Degrees from the sketch's own +u axis, not the world's X. The number has to mean something in
    // the plane being drawn on, or it is nonsense on a tilted face.
    out.angle = out.circle ? 0.0 : std::atan2(dy, dx) * 180.0 / std::numbers::pi;
    out.valid = true;
    return out;
}

SketchDrawing::Text SketchDrawing::text(units::UnitSystem display) const {
    Text out;
    const Measure current = measure();
    if (!current.valid) return out;

    if (current.rectangle) {
        // "60 x 40 mm", not a length and an angle: those are the two numbers a rectangle is made
        // of, and they are the two the user is deciding while dragging.
        out.valid = true;
        out.length = units::format(units::millimetres(current.width), display) + " x "
                     + units::format(units::millimetres(current.height), display);
        out.angle.clear();
        return out;
    }

    // What the user TYPED wins, because that is the value that will be used — showing the measured
    // length beside a number being typed to replace it is showing two answers to one question.
    out.length = input_.empty() ? units::format(units::millimetres(current.length), display, 2)
                                : input_;
    if (current.circle) {
        out.length = "R " + out.length;
    } else {
        out.angle = units::format(units::degrees(current.angle), 1);
    }
    out.valid = true;
    return out;
}

std::vector<SketchDrawing::Point> SketchDrawing::previewSegments(const Context& ctx) const {
    std::vector<Point> out;
    const auto end = aimed();
    if (!pending_ || !end) return out;

    // DASHED, and dashed in screen terms. A preview must be distinguishable from committed geometry
    // at a glance, and a dash measured in world units becomes a solid line zoomed in and a row of
    // dots zoomed out — which is when it is least readable.
    const double dash = std::max(1e-6, ctx.worldPerPixel) * 6.0;   // ~6 px on, 6 px off
    const auto emit = [&](Point a, Point b) {
        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length < 1e-12) return;
        // Capped so a wildly zoomed-out view cannot ask for a million sub-pixel segments mid-drag,
        // where the dashes read as a solid line anyway.
        const int steps = static_cast<int>(std::min(2000.0, std::ceil(length / (dash * 2.0))));
        for (int i = 0; i < steps; ++i) {
            const double t0 = std::min(1.0, (i * 2.0 * dash) / length);
            const double t1 = std::min(1.0, (i * 2.0 * dash + dash) / length);
            out.push_back(Point{a[0] + dx * t0, a[1] + dy * t0});
            out.push_back(Point{a[0] + dx * t1, a[1] + dy * t1});
        }
    };

    if (tool_ == Tool::Rectangle) {
        // The four sides the tool WILL create, so the preview and the result are the same shape.
        // Without this a rectangle was drawn blind: the user dragged and saw nothing until they
        // let go, which is the one thing a rubber band exists to prevent.
        const Point a = *pending_;
        const Point b = *end;
        const Point corners[4]{{a[0], a[1]}, {b[0], a[1]}, {b[0], b[1]}, {a[0], b[1]}};
        for (int i = 0; i < 4; ++i) emit(corners[i], corners[(i + 1) % 4]);
        return out;
    }

    if (tool_ == Tool::Circle) {
        const double dx = (*end)[0] - (*pending_)[0];
        const double dy = (*end)[1] - (*pending_)[1];
        const double radius = std::sqrt(dx * dx + dy * dy);
        constexpr int kSegments = 64;
        for (int i = 0; i < kSegments; ++i) {
            const double a0 = 2.0 * std::numbers::pi * i / kSegments;
            const double a1 = 2.0 * std::numbers::pi * (i + 1) / kSegments;
            emit(Point{(*pending_)[0] + radius * std::cos(a0),
                       (*pending_)[1] + radius * std::sin(a0)},
                 Point{(*pending_)[0] + radius * std::cos(a1),
                       (*pending_)[1] + radius * std::sin(a1)});
        }
    } else {
        emit(*pending_, *end);
    }
    return out;
}

}  // namespace cad::app
