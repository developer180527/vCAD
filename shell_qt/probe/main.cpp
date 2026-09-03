/// Does the desktop shell actually DO what its controls promise?
///
/// # Why this exists
///
/// The selection filter bar sat in the quick-access strip for months, looked correct, and was wired
/// to nothing: clicking "Edge" re-rendered the status text and never told the Controller. Hover
/// pre-highlight was the same story from the other end — `Controller::hoverAt` maintained a hovered
/// element and fed the highlight table, and no shell ever called it.
///
/// Neither could have survived a test, and neither had one. Every automated check in this project
/// stops at `app/`: the acceptance suite drives the Controller directly, so a shell that never calls
/// the Controller passes everything. That gap is exactly the size of both bugs.
///
/// So this drives the REAL widgets with synthetic events and asks the Controller what happened. It
/// is not a rendering test — it never looks at a pixel — it is a wiring test, and wiring is what
/// keeps breaking.
///
/// Runs offscreen, so it works in CI and over ssh.

#include "MainWindow.h"
#include "ParametersDialog.h"
#include "Viewport.h"

#include "cad/app/Controller.h"
#include "cad/render/BgfxBackend.h"

#include <QAbstractButton>
#include <QApplication>

#include <filesystem>
#include <system_error>
#include <QMouseEvent>
#include <QAction>
#include <QTableWidget>
#include <QToolButton>

#include <algorithm>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

/// A press and a release at one point, as Qt delivers them.
void click(QWidget* target, QPointF at, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const QPointF global = target->mapToGlobal(at);
    QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton, mods);
    QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton, mods);
    QApplication::sendEvent(target, &press);
    QApplication::sendEvent(target, &release);
}

/// A double click, as Qt actually delivers one: press, release, DOUBLE CLICK, release.
///
/// The trailing release is the whole point. A test that sent only press/release/doubleClick would
/// pass over the bug this exists for -- the body was selected by the double click and then replaced
/// by the face under the pointer when that last release ran the ordinary selection again.
void doubleClick(QWidget* target, QPointF at, cad::app::Controller* controller,
                 Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const QPointF global = target->mapToGlobal(at);
    QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton, mods);
    QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton, mods);
    QMouseEvent second(QEvent::MouseButtonDblClick, at, global, Qt::LeftButton, Qt::LeftButton,
                       mods);
    // TWO release objects, not one sent twice. An event carries its accept/ignore state across
    // sends, so re-sending the first one would hand the second dispatch whatever the first left
    // behind -- which is not what Qt delivers, and this helper exists to deliver what Qt does.
    QMouseEvent trailing(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton, mods);
    QApplication::sendEvent(target, &press);
    QApplication::sendEvent(target, &release);
    QApplication::sendEvent(target, &second);
    QApplication::sendEvent(target, &trailing);
}

void move(QWidget* target, QPointF at) {
    const QPointF global = target->mapToGlobal(at);
    QMouseEvent moved(QEvent::MouseMove, at, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(target, &moved);
}

}   // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);

    // Shaders resolve against the EXECUTABLE, exactly as main.cpp does it.
    //
    // Without this the probe inherited the working directory instead, so it passed when run from
    // build/shell_qt and reported "SHELL WIRING IS BROKEN" when run from the repo root -- no
    // renderer came up, so every check that needed one failed. A diagnostic whose answer depends on
    // where you stood when you asked it is worse than no diagnostic: it was chased twice as a real
    // regression before anyone noticed the pattern.
    if (argc > 0 && argv[0] != nullptr && *argv[0] != '\0') {
        std::error_code ec;
        const std::filesystem::path exe =
            std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), ec);
        if (!ec && exe.has_parent_path()) {
            cad::render::setShaderDirectory((exe.parent_path() / "shaders").string());
        }
    }

    cadqt::MainWindow window;
    window.resize(1200, 800);
    window.show();
    // A document with two bodies in it, which is what the shell's own demo opens.
    window.openDemoDocument();
    QApplication::processEvents();

    auto* controller = window.probeController();
    auto* viewport = window.probeViewport();
    check(controller != nullptr, "the demo document has a controller");
    check(viewport != nullptr, "the demo document has a viewport");
    if (controller == nullptr || viewport == nullptr) return 1;

    // ── the selection filter is connected ────────────────────────────────────────────────
    //
    // Found by NAME rather than by index: the bar gained an "Auto" entry at the front, and a test
    // that counted positions would have silently started checking the wrong button.
    using Level = cad::app::Controller::SelectionLevel;
    check(controller->selectionLevel() == Level::Auto,
          "a new document starts at the Auto selection level");

    bool clickedEdge = false;
    for (auto* button : window.findChildren<QToolButton*>()) {
        if (button->text() != QStringLiteral("Edge")) continue;
        button->click();
        clickedEdge = true;
        break;
    }
    check(clickedEdge, "the filter bar offers an Edge button");
    QApplication::processEvents();
    check(controller->selectionLevel() == Level::Edge,
          "clicking Edge tells the Controller — the bar is not decoration");

    // Back to Auto for the rest, since that is what the viewport tests assume.
    for (auto* button : window.findChildren<QToolButton*>()) {
        if (button->text() == QStringLiteral("Auto")) {
            button->click();
            break;
        }
    }
    QApplication::processEvents();

    // ── the viewport forwards hover and clicks ───────────────────────────────────────────
    //
    // Whether the GPU pick finds anything depends on a working renderer, which an offscreen CI
    // machine may not have. So the assertions are about the WIRING: the events must reach the
    // Controller. With no renderer attached the pick reports nothing and both calls are no-ops,
    // which is reported as a skip rather than a pass.
    // Printed before the assertions, because "picked nothing" has several causes and they are
    // indistinguishable from the outcome alone: no renderer, no geometry, or a viewport whose size
    // the renderer disagrees with.
    const auto stats = controller->stats();
    std::printf("     viewport %dx%d, renderer %s, presenting %s, %zu instance(s)\n",
                viewport->width(), viewport->height(), controller->rendererName().c_str(),
                controller->presentsDirectly() ? "directly" : "by blit", stats.instances);

    const QPointF centre(viewport->width() / 2.0, viewport->height() / 2.0);
    controller->clearSelection();

    // The pick, called DIRECTLY, before and after presenting a frame.
    //
    // Narrowing where the difference between the two presentation paths lives: if the pick only
    // works once a frame has been presented, the fault is ordering rather than the pick itself.
    const auto dpr = viewport->devicePixelRatioF();
    const auto px = static_cast<std::uint32_t>(centre.x() * dpr);
    const auto py = static_cast<std::uint32_t>(centre.y() * dpr);
    std::printf("     candidates before presenting: %zu\n", controller->candidatesAt(px, py, 8).size());
    controller->presentFrame();
    std::printf("     candidates after  presenting: %zu\n", controller->candidatesAt(px, py, 8).size());

    move(viewport, centre);
    QApplication::processEvents();
    const bool hovered = controller->hoveredElement().has_value();

    click(viewport, centre);
    QApplication::processEvents();
    const bool selected = !controller->selection().empty() || !controller->elementSelection().empty();

    // Skipped ONLY when there is nothing to pick — no renderer, or no geometry. With a live
    // renderer and instances on screen, "picked nothing" is a failure and not an environment.
    //
    // This mattered: an earlier version skipped whenever the pick found nothing, and that skip hid
    // a real bug for a whole run. The pick target was never rebuilt when the window resized, so
    // every desktop pick read a stale texture — dead selection, dead hover, and a green probe.
    const bool canPick = controller->rendererAttached() && stats.instances > 0;
    if (!canPick) {
        std::printf("SKIP no renderer or no geometry on this machine, so picking cannot be "
                    "observed here\n");
    } else {
        check(hovered, "moving the pointer pre-highlights what is under it");
        check(selected, "clicking the viewport selects what is under the pointer");
    }

    // ── drawing in the viewport ──────────────────────────────────────────────────────────
    //
    // Reported from the desktop: "multiple lines appear when drawing". Three clicks along a chain
    // must produce exactly two segments, and counting them is the only way to tell a rendering
    // artefact from geometry that is really there.
    //
    // NOT gated on canPick, unlike the block above, because drawing does not go through the picker.
    // A click in a sketch reaches `Controller::sketchPointAt`, which intersects a camera ray with
    // the sketch plane -- pure maths, needing no renderer, no id buffer and no instances on screen.
    //
    // It WAS gated on canPick, and on a machine where the renderer comes up null that skipped these
    // two checks while the probe still printed "all shell wiring checks passed". A check that
    // quietly does not run is worth less than no check, because it is counted as evidence. This
    // block needs a controller and a viewport with a size, and both were asserted above.
    {
        for (const auto& command : controller->commands()) {
            if (command.id == "feature.sketch" && command.invoke) {
                command.invoke();
                break;
            }
        }
        QApplication::processEvents();
        check(controller->environment() == cad::app::Environment::Sketch, "Start Sketch opens one");
        controller->setSketchTool(cad::app::Controller::SketchTool::Line);

        const double w = viewport->width();
        const double h = viewport->height();
        click(viewport, QPointF(w * 0.35, h * 0.40));
        QApplication::processEvents();
        click(viewport, QPointF(w * 0.60, h * 0.40));
        QApplication::processEvents();
        click(viewport, QPointF(w * 0.60, h * 0.62));
        QApplication::processEvents();

        const auto* sketch = controller->activeSketch();
        const std::size_t drawn = sketch != nullptr ? sketch->geometry().size() : 0;
        std::printf("     three clicks produced %zu piece(s) of geometry\n", drawn);
        check(drawn == 2, "three chained clicks draw exactly two segments");

        // Finish, then re-open the same sketch. While it is being edited the overlay draws its
        // curves — so if the FEATURE is drawn as well, every line is on screen twice.
        controller->finishSketch();
        QApplication::processEvents();
        cad::document::ObjectId sketchId;
        for (const auto& item : controller->tree()) {
            if (item.type == "Sketch") sketchId = item.id;
        }
        if (sketchId != cad::document::ObjectId{}) {
            controller->editSketch(sketchId);
            QApplication::processEvents();
            bool featureDrawn = false;
            for (const auto& item : controller->tree()) {
                if (item.id == sketchId) featureDrawn = item.visible;
            }
            std::printf("     while editing, the sketch feature is %s\n",
                        featureDrawn ? "ALSO drawn" : "hidden");
            check(!featureDrawn, "the sketch being edited is not drawn twice");
            controller->finishSketch();
        }
    }

    // ── dimensions drawn in the viewport ─────────────────────────────────────────────────
    //
    // The labels are separate top-level windows, because a child widget over the Metal layer paints
    // into a surface nothing clears (see Viewport's own comment). That makes them invisible to a
    // window grab, so a screenshot cannot prove they are on screen — this is the check that can.
    {
        for (const auto& command : controller->commands()) {
            if (command.id == "feature.sketch" && command.invoke) {
                command.invoke();
                break;
            }
        }
        QApplication::processEvents();

        if (auto* sketch = controller->activeSketch()) {
            const auto line = sketch->addLine(-40, -20, 40, -20);
            sketch->distance(line, cad::sketch::PointRef::Start, line, cad::sketch::PointRef::End,
                             80);
            const auto circle = sketch->addCircle(-10, 10, 14);
            sketch->radius(circle, 14);

            viewport->markDirty();
            viewport->repaint();
            QApplication::processEvents();

            const auto labels = viewport->visibleDimensionLabelsForProbe();
            std::printf("     %zu dimension label(s) on screen\n", labels.size());
            for (const QString& text : labels) {
                std::printf("       \"%s\"\n", qPrintable(text));
            }
            check(labels.size() == 2, "both dimensions are drawn in the viewport");

            const bool sawRadius = std::any_of(labels.begin(), labels.end(), [](const QString& s) {
                return s.startsWith(QStringLiteral("R"));
            });
            check(sawRadius, "a radius dimension is marked R");

            controller->finishSketch();
            QApplication::processEvents();
            viewport->repaint();
            QApplication::processEvents();
            check(viewport->visibleDimensionLabelsForProbe().empty(),
                  "the labels go away when the sketch is finished");
        }
    }

    // ── the parameters table ──────────────────────────────────────────────────────────────
    //
    // Driven through the REAL dialog's real table widget, because the failure this guards against
    // is precisely a table that looks right and reaches nothing. Editing a cell must move geometry.
    {
        // Through the RIBBON ACTION, not by calling the slot. The connect() is the thing that was
        // missing in every wiring bug this probe exists for.
        QAction* open = nullptr;
        for (QAction* action : window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("Parameters")) { open = action; break; }
        }
        check(open != nullptr, "the ribbon has a Parameters button");
        if (open != nullptr) open->trigger();
        QApplication::processEvents();

        auto* dialog = window.findChild<cadqt::ParametersDialog*>();
        check(dialog != nullptr, "the Parameters button opens the parameters table");

        auto* table = dialog != nullptr ? dialog->findChild<QTableWidget*>() : nullptr;
        check(table != nullptr, "the parameters table exists");

        if (table != nullptr) {
            check(controller->setParameter("width", "40mm"), "a parameter can be added");
            dialog->refresh();
            QApplication::processEvents();
            check(table->rowCount() == 1, "the new parameter appears as a row");

            // A feature driven by it, entered exactly as a user would type it.
            for (const auto& command : controller->commands()) {
                if (command.id == "feature.box" && command.invoke) { command.invoke(); break; }
            }
            controller->beginCommand("feature.box");
            controller->setCommandParameter("dx", "width * 2");
            controller->commitCommand();
            QApplication::processEvents();

            const auto box = controller->selection().empty() ? cad::document::ObjectId{}
                                                             : controller->selection().front();
            const auto widthOf = [&] {
                const auto object = controller->document().find(box);
                if (!object) return 0.0;
                const auto* value = object->find("dx");
                if (value == nullptr) return 0.0;
                return std::get<cad::units::Length>(*value).base();
            };
            check(widthOf() == 80.0, "a feature created with `width * 2` is 80mm");

            // THE POINT OF THE WHOLE FEATURE: type into the table, geometry moves.
            const int row = 0;
            table->item(row, 1)->setText(QStringLiteral("60mm"));
            QApplication::processEvents();
            std::printf("     dx after editing width to 60mm: %.1f\n", widthOf());
            check(widthOf() == 120.0, "editing the parameter in the table rebuilds the feature");

            // And a cycle typed into the table is refused rather than crashing or being stored.
            check(controller->setParameter("a", "b + 1mm") == false,
                  "a parameter naming something that does not exist is refused");
            controller->setParameter("a", "10mm");
            controller->setParameter("b", "a + 1mm");
            check(controller->setParameter("a", "b + 1mm") == false,
                  "a parameter that would close a cycle is refused");
        }
    }

    // ── a double click selects the whole body ────────────────────────────────────────────
    //
    // Reported from the UI: "I CANNOT completely select the whole box by double clicking it."
    // Everything below the shell was right -- Controller::tapAt at Body level selects the body and
    // says "Selected Box" -- so the bug lived entirely in the four events Qt sends for a double
    // click: press, release, DOUBLE CLICK, release. The double click selected the body and the
    // release after it ran the ordinary selection again, putting the face back.
    //
    // Driven through real QMouseEvents against the real Viewport, because that trailing release is
    // the entire bug and nothing below the shell can see it.
    //
    // A FRAME IS PRESENTED FIRST and a picking point is hunted for, because the id buffer is filled
    // by rendering: without both, every click lands on nothing, and a gesture test aimed at nothing
    // passes whatever the gesture does. That is not hypothetical -- the first version of this check
    // passed with the fix removed, which is the only reason the point below is chosen rather than
    // assumed.
    {
        // BACK TO THE MODEL ENVIRONMENT FIRST. An earlier section of this probe opens a sketch and
        // leaves it open, and in a sketch a double click ends the current chain rather than
        // selecting a body -- so without this the block below exercised a different gesture
        // entirely and reported it as this one failing. Found by tracing the handler, not by
        // reading it.
        // TWO pieces of state, and cancelling the sketch clears only one of them. An earlier
        // section also leaves the app AWAITING A SKETCH PLANE, and in that state the next click is
        // the answer to "which plane?" -- so the first click below silently re-entered the sketch
        // environment and the double click ended a chain instead of selecting a body. The check
        // that we were in Model passed, because the click that broke it came afterwards.
        controller->cancelSketchPlanePick();
        if (controller->environment() == cad::app::Environment::Sketch) controller->cancelSketch();
        check(controller->environment() == cad::app::Environment::Model
                  && !controller->awaitingSketchPlane(),
              "the double click checks run in the model environment, with no sketch pending");

        controller->clearSelection();
        controller->setSelectionLevel(Level::Auto);
        controller->presentFrame();

        const auto dpr = viewport->devicePixelRatioF();
        QPointF target;
        bool found = false;
        for (int gy = 1; gy < 8 && !found; ++gy) {
            for (int gx = 1; gx < 8 && !found; ++gx) {
                const QPointF p(viewport->width() * gx / 8.0, viewport->height() * gy / 8.0);
                const auto px = static_cast<std::uint32_t>(p.x() * dpr);
                const auto py = static_cast<std::uint32_t>(p.y() * dpr);
                if (controller->candidatesAt(px, py, 8).empty()) continue;
                // A point where one click resolves to an ELEMENT, not straight to a body. The
                // Shift check below is about a face and a body ending up selected together, and at
                // a point that already picks a body there is no face for it to go wrong with --
                // the assertion would pass whatever the code did.
                controller->clearSelection();
                click(viewport, p);
                if (!controller->elementSelection().empty()) {
                    target = p;
                    found = true;
                }
                controller->presentFrame();
            }
        }

        if (!found) {
            // Said out loud rather than passed. This machine cannot fill an id buffer, so the
            // gesture cannot be exercised here at all -- and a green line claiming otherwise is
            // exactly the lying-pass this file exists to avoid.
            std::printf("SKIP nothing pickable in the viewport, so the double click gesture "
                        "cannot be exercised on this machine\n");
        } else {
            controller->clearSelection();
            click(viewport, target);
            const bool oneClickPicks =
                !controller->elementSelection().empty() || !controller->selection().empty();

            controller->clearSelection();
            doubleClick(viewport, target, controller);
            const bool bodySelected = controller->selection().size() == 1;
            const bool noStrayElement = controller->elementSelection().empty();

            check(oneClickPicks, "one click at Auto selects what is under the pointer");
            check(bodySelected,
                  "a double click selects the whole body, and the release after it does not "
                  "undo that");
            check(noStrayElement, "and the body selection is not left mixed with a face selection");

            // Shift held, on ONE point. This says the modifier does not leave a face selected
            // beside the body, and that is all it says.
            //
            // It does NOT cover the accumulation the clearSelection above guards against: that is
            // two shift double clicks on two DIFFERENT bodies, and it needs a fixture with two of
            // them placed where both can be picked. Removing that clearSelection leaves this line
            // green, which is stated here rather than left for someone to assume otherwise.
            controller->clearSelection();
            doubleClick(viewport, target, controller, Qt::ShiftModifier);
            check(controller->selection().size() == 1 && controller->elementSelection().empty(),
                  "shift double click leaves a body selected and no stray face");
        }
    }

    std::printf("\n%s\n", failures == 0 ? "all shell wiring checks passed"
                                        : "SHELL WIRING IS BROKEN");
    return failures == 0 ? 0 : 1;
}
