// Picking a face to sketch on, driven from the layer both shells share.
//
// Step 1d.1 of the sketch-plane work. The pick pass has existed in the bgfx backend since M3 and
// has never been driven from above, so the shell had no way to say "on that face" even after the
// core learned to accept one.
//
// Headless on purpose, and the null picker is scripted rather than rasterised. What this layer owns
// is turning an element slot into a name, a name into topology, and a face into a plane -- with a
// REASON when that is impossible. Whether a GPU writes correct ids into the id buffer is a separate
// claim that needs a GPU to answer honestly; this project's history with pixel checks that looked
// right and were not is the reason for not blurring the two.
//
// Note what the tests below do NOT do: reach into the document to look up topology. They classify a
// slot by what `pickSketchFace` ANSWERS, because that answer is the whole contract -- accepted, or
// refused with a distinguishable reason. A test that consulted the element map itself would be
// asserting against its own second implementation of the thing under test.

#include "cad/app/Controller.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using cad::app::Controller;
using Catch::Approx;

namespace {

/// How the pick layer answered for one element slot.
struct Verdict {
    std::uint32_t slot = 0;
    bool picked = false;                    ///< the slot carried a resolvable name at all
    bool accepted = false;                  ///< a flat face: a sketch can go here
    cad::kernel::ErrorCode refusal{};       ///< meaningful when picked && !accepted
    std::string message;
};

/// Every slot the scene actually carries for `id`, with the pick layer's verdict on each.
///
/// Walks slots rather than assuming an order. Slot 0 happens to be a face today; asserting that
/// would test the tessellator's output ordering, and a test that fails when an unrelated ordering
/// changes gets deleted rather than fixed.
std::vector<Verdict> verdicts(Controller& app, cad::document::ObjectId id) {
    std::vector<Verdict> out;
    for (std::uint32_t slot = 0; slot < 2048; ++slot) {
        app.scriptNextPick(slot);
        const Controller::Pick pick = app.pickAt(10, 10);
        if (!pick.hit || pick.element.isNull() || pick.object != id) continue;

        Verdict v;
        v.slot = slot;
        v.picked = true;
        app.scriptNextPick(slot);
        const auto face = app.pickSketchFace(10, 10);
        v.accepted = static_cast<bool>(face);
        if (!face) {
            v.refusal = face.error().code;
            v.message = face.error().message;
        }
        out.push_back(std::move(v));
    }
    return out;
}

/// Invokes a ribbon command by id and returns the object it created.
///
/// Through the command registry rather than a direct call, because that is the route a click takes.
/// It also keeps this test off Controller's private surface: a test that needs a wider API than the
/// shell does is testing something the shell cannot do.
cad::document::ObjectId create(Controller& app, const std::string& commandId) {
    for (const auto& command : app.commands()) {
        if (command.id != commandId) continue;
        command.invoke();
        break;
    }
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

}  // namespace

TEST_CASE("a pick with nothing under it is refused with something to do", "[pick][sketch]") {
    Controller app;
    create(app, "feature.box");

    app.scriptNextPick(0, /*valid=*/false);
    CHECK_FALSE(app.pickAt(10, 10).hit);

    app.scriptNextPick(0, /*valid=*/false);
    const auto face = app.pickSketchFace(10, 10);
    REQUIRE_FALSE(face);
    // Not merely "failed": the message has to say what to do instead. A click that does nothing and
    // says nothing is how a working feature reads as a broken one.
    CHECK(face.error().code == cad::kernel::ErrorCode::NotDone);
    CHECK_FALSE(face.error().message.empty());
}

TEST_CASE("a pick on a box face yields a stable name and a usable plane", "[pick][sketch]") {
    Controller app;
    const auto id = create(app, "feature.box");

    const auto all = verdicts(app, id);
    REQUIRE_FALSE(all.empty());

    std::uint32_t faceSlot = 0;
    bool found = false;
    for (const Verdict& v : all) {
        if (v.accepted) {
            faceSlot = v.slot;
            found = true;
            break;
        }
    }
    INFO("slots carrying a name: " << all.size());
    REQUIRE(found);

    app.scriptNextPick(faceSlot);
    const Controller::Pick pick = app.pickAt(10, 10);
    REQUIRE(pick.hit);
    CHECK(pick.object == id);
    CHECK_FALSE(pick.element.isNull());
    CHECK(pick.slot == faceSlot);

    app.scriptNextPick(faceSlot);
    const auto face = app.pickSketchFace(10, 10);
    REQUIRE(face);
    CHECK(face.value().object == id);
    // The two entry points must agree. If `pickSketchFace` resolved a different element than
    // `pickAt` reported, the highlight the user sees and the face the sketch lands on would differ.
    CHECK(face.value().face == pick.element);

    // The frame has to be USABLE, not merely present. A zero u or v axis passes a "did it return
    // something" check and then places every sketch curve at the origin.
    const auto length = [](const double a[3]) {
        return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    };
    CHECK(length(face.value().frame.u) == Approx(1.0).margin(1e-9));
    CHECK(length(face.value().frame.v) == Approx(1.0).margin(1e-9));
    const auto n = face.value().frame.normal();
    CHECK(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) == Approx(1.0).margin(1e-9));

    // u and v perpendicular. Not pedantry: `to3d` treats them as an orthonormal basis, so a skewed
    // pair would silently distort every sketch drawn on this face.
    const auto& f = face.value().frame;
    CHECK(f.u[0] * f.v[0] + f.u[1] * f.v[1] + f.u[2] * f.v[2] == Approx(0.0).margin(1e-9));
}

TEST_CASE("the same face picked twice gives the same name", "[pick][sketch]") {
    // The property that makes a picked face worth storing in a sketch at all. If a pick returned a
    // different name each time, a sketch placed by clicking would carry a reference that cannot be
    // looked up again -- which is precisely the failure the naming layer exists to prevent, and the
    // one FreeCAD spent years unwinding.
    Controller app;
    const auto id = create(app, "feature.box");

    const auto all = verdicts(app, id);
    std::uint32_t slot = 0;
    bool found = false;
    for (const Verdict& v : all) {
        if (v.accepted) { slot = v.slot; found = true; break; }
    }
    REQUIRE(found);

    app.scriptNextPick(slot);
    const auto first = app.pickSketchFace(10, 10);
    app.scriptNextPick(slot);
    const auto second = app.pickSketchFace(10, 10);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first.value().face.toString() == second.value().face.toString());

    for (int i = 0; i < 3; ++i) {
        CHECK(first.value().frame.origin[i] == Approx(second.value().frame.origin[i]));
        CHECK(first.value().frame.u[i] == Approx(second.value().frame.u[i]));
        CHECK(first.value().frame.v[i] == Approx(second.value().frame.v[i]));
    }
}

TEST_CASE("a box offers six distinct sketchable faces", "[pick][sketch]") {
    // Distinct NAMES, not six accepted slots: a box has six faces, and if two of them resolved to
    // the same name then clicking one would place a sketch on the other. Counting acceptances alone
    // would not notice that.
    Controller app;
    const auto id = create(app, "feature.box");

    std::vector<std::string> names;
    for (const Verdict& v : verdicts(app, id)) {
        if (!v.accepted) continue;
        app.scriptNextPick(v.slot);
        const auto face = app.pickSketchFace(10, 10);
        REQUIRE(face);
        const std::string name = face.value().face.toString();
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    CHECK(names.size() == 6);
}

TEST_CASE("a curved face is refused with the kernel's own reason", "[pick][sketch]") {
    // The refusal PICKUP names, and the one most likely to be swallowed. `kernel::planeOf` already
    // measures the surface and says no to a cylinder's side with ErrorCode::Unsupported; what is
    // tested here is that the reason SURVIVES the trip up to the shell rather than turning into a
    // click that quietly does nothing.
    Controller app;
    const auto id = create(app, "feature.cylinder");

    std::size_t curvedRefusals = 0;
    std::size_t acceptances = 0;
    for (const Verdict& v : verdicts(app, id)) {
        if (v.accepted) {
            ++acceptances;                                    // a flat cap
        } else if (v.refusal == cad::kernel::ErrorCode::Unsupported) {
            ++curvedRefusals;                                 // the round side
            CHECK_FALSE(v.message.empty());
        }
    }

    // Both, from one solid. A cylinder that refused everything would look like a working refusal
    // while actually being a broken picker; one that accepted everything would mean planeOf's answer
    // is thrown away somewhere between the kernel and here. Only the pair rules out both.
    INFO("acceptances: " << acceptances << ", curved refusals: " << curvedRefusals);
    CHECK(curvedRefusals > 0);
    CHECK(acceptances > 0);
}

TEST_CASE("a non-face element is refused differently from a curved face", "[pick][sketch]") {
    // Two refusals that must not collapse into one. "That is an edge" and "that face is not flat"
    // send the user to different actions, so they carry different codes -- and the shell can only
    // say the right thing if the distinction reaches it.
    Controller app;
    const auto id = create(app, "feature.box");

    std::size_t nonFace = 0;
    for (const Verdict& v : verdicts(app, id)) {
        if (!v.accepted && v.refusal == cad::kernel::ErrorCode::InvalidInput) ++nonFace;
        // A box has no curved faces, so an Unsupported here would mean planeOf is being asked
        // about something that is not the face that was picked.
        CHECK(v.refusal != cad::kernel::ErrorCode::Unsupported);
    }
    if (nonFace == 0) {
        // Legitimate: the scene need not put edges in the id buffer. It must not read as a passing
        // check of a refusal that never ran.
        SKIP("no non-face element in the id buffer on this build");
    }
    CHECK(nonFace > 0);
}
