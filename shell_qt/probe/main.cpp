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
#include "Viewport.h"

#include "cad/app/Controller.h"

#include <QAbstractButton>
#include <QApplication>
#include <QMouseEvent>
#include <QToolButton>

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

void move(QWidget* target, QPointF at) {
    const QPointF global = target->mapToGlobal(at);
    QMouseEvent moved(QEvent::MouseMove, at, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(target, &moved);
}

}   // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);

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

    std::printf("\n%s\n", failures == 0 ? "all shell wiring checks passed"
                                        : "SHELL WIRING IS BROKEN");
    return failures == 0 ? 0 : 1;
}
