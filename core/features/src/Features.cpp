#include "cad/features/Builtins.h"

#include <numbers>
#include <tuple>

#include "cad/kernel/Booleans.h"
#include "cad/kernel/Shape.h"
#include "cad/naming/ElementName.h"
#include "cad/kernel/Fillet.h"
#include "cad/kernel/Primitives.h"
#include "cad/kernel/Transform.h"
#include "cad/sketch/Sketch.h"
#include "cad/io/Format.h"

namespace cad::recompute {
namespace {

using document::ObjectData;
using document::PropertyValue;
using kernel::Error;
using kernel::ErrorCode;
using units::Length;

/// Typed property access with a legible failure. Feature code should never index into
/// properties by position or assume a type.
template <class T>
kernel::Result<T> require(const ObjectData& o, std::string_view name) {
    const PropertyValue* v = o.find(name);
    if (v == nullptr) {
        return Error{ErrorCode::InvalidInput,
                     "'" + o.label() + "' is missing its " + std::string(name) + "."};
    }
    const T* typed = std::get_if<T>(v);
    if (typed == nullptr) {
        return Error{ErrorCode::InvalidInput,
                     "'" + o.label() + "' has the wrong kind of value for " +
                         std::string(name) + "."};
    }
    return *typed;
}

/// Resolves an element reference through the input's map, using resolveAll so a reference
/// to an edge that has since been split still covers every child. This is the M1 contract
/// showing up at the feature layer, which is where it has to be used to be worth anything.
kernel::Result<std::vector<kernel::Shape>> resolveEdges(const Output& input,
                                                        const ObjectData& object) {
    const PropertyValue* v = object.find("edges");
    if (v == nullptr) {
        return Error{ErrorCode::InvalidInput, "'" + object.label() + "' has no edges selected."};
    }

    std::vector<naming::ElementName> names;
    if (const auto* one = std::get_if<naming::ElementName>(v)) {
        names.push_back(*one);
    } else if (const auto* many = std::get_if<std::vector<naming::ElementName>>(v)) {
        names = *many;
    } else {
        return Error{ErrorCode::InvalidInput, "'edges' must be an element reference."};
    }

    std::vector<kernel::Shape> out;
    for (const auto& name : names) {
        auto found = input.map.resolveAll(name);
        if (found.empty()) {
            return Error{ErrorCode::NamingLost,
                         "An edge selected by '" + object.label() + "' no longer exists.",
                         name.toString()};
        }
        for (auto& e : found) out.push_back(std::move(e));
    }
    return out;
}

kernel::Result<Output> nameResult(const kernel::Operation& op,
                                  const std::vector<const Output*>& inputs,
                                  std::uint32_t serial) {
    std::vector<const kernel::Shape*> shapes;
    std::vector<const naming::ElementMap*> maps;
    shapes.reserve(inputs.size());
    maps.reserve(inputs.size());
    for (const Output* in : inputs) {
        shapes.push_back(&in->shape);
        maps.push_back(&in->map);
    }

    naming::NamingContext ctx(serial, 0);
    auto map = ctx.propagate(op, shapes, maps);
    if (!map) return map.error();
    return Output{op.shape(), std::move(map).value()};
}

// --- the built-in features ---------------------------------------------------------------

kernel::Result<Output> computeBox(const ComputeContext& ctx) {
    auto dx = require<Length>(ctx.object, "dx");
    if (!dx) return dx.error();
    auto dy = require<Length>(ctx.object, "dy");
    if (!dy) return dy.error();
    auto dz = require<Length>(ctx.object, "dz");
    if (!dz) return dz.error();

    auto built = kernel::makeBox(dx.value().base(), dy.value().base(), dz.value().base());
    if (!built) return built.error();

    naming::NamingContext naming(ctx.namingSerial, 0);
    auto map = naming.nameprimitive(built.value().op.shape(), built.value().taggedFaces);
    if (!map) return map.error();
    return Output{built.value().op.shape(), std::move(map).value()};
}

kernel::Result<Output> computeCylinder(const ComputeContext& ctx) {
    auto r = require<Length>(ctx.object, "radius");
    if (!r) return r.error();
    auto h = require<Length>(ctx.object, "height");
    if (!h) return h.error();

    auto op = kernel::makeCylinder(r.value().base(), h.value().base());
    if (!op) return op.error();

    // A cylinder's faces have no positional constructor tags the way a box's do, so they
    // fall through to the generic namer. Documented rather than hidden: adding tagged
    // accessors for the lateral/top/bottom faces is a real improvement waiting to be made.
    naming::NamingContext naming(ctx.namingSerial, 0);
    auto map = naming.nameprimitive(op.value().shape(), {});
    if (!map) return map.error();
    return Output{op.value().shape(), std::move(map).value()};
}

kernel::Result<Output> computeFillet(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 1) {
        return Error{ErrorCode::InvalidInput, "A fillet needs exactly one input shape."};
    }
    auto radius = require<Length>(ctx.object, "radius");
    if (!radius) return radius.error();
    auto edges = resolveEdges(*ctx.inputs[0], ctx.object);
    if (!edges) return edges.error();

    auto op = kernel::filletEdges(ctx.inputs[0]->shape, edges.value(), radius.value().base());
    if (!op) return op.error();
    return nameResult(op.value(), ctx.inputs, ctx.namingSerial);
}

kernel::Result<Output> computeChamfer(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 1) {
        return Error{ErrorCode::InvalidInput, "A chamfer needs exactly one input shape."};
    }
    auto distance = require<Length>(ctx.object, "distance");
    if (!distance) return distance.error();
    auto edges = resolveEdges(*ctx.inputs[0], ctx.object);
    if (!edges) return edges.error();

    auto op = kernel::chamferEdges(ctx.inputs[0]->shape, edges.value(),
                                   distance.value().base());
    if (!op) return op.error();
    return nameResult(op.value(), ctx.inputs, ctx.namingSerial);
}

template <kernel::Result<kernel::Operation> (*Op)(const kernel::Shape&, const kernel::Shape&)>
kernel::Result<Output> computeBoolean(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 2) {
        return Error{ErrorCode::InvalidInput, "This operation needs exactly two shapes."};
    }
    auto op = Op(ctx.inputs[0]->shape, ctx.inputs[1]->shape);
    if (!op) return op.error();
    return nameResult(op.value(), ctx.inputs, ctx.namingSerial);
}

/// A sketch as a document feature.
///
/// The sketch itself lives in a TEXT property, serialised by cad::sketch::Sketch. That is why the
/// document format needed no schema change to store sketches: a sketch is a string as far as
/// persistence is concerned, and the format already round-trips those exactly (ADR 0003).
///
/// The output is the profile FACE, not the wire, because that is what an extrude consumes. A sketch
/// whose curves do not close therefore fails HERE, with a message about the profile, rather than
/// inside a modelling operation two features later.
/// kernel::PlaneFrame and sketch::SketchFrame are separate types with the same shape, because
/// core/sketch must not depend on core/kernel. This is the one layer that legitimately sees both,
/// so the conversion lives here and exactly once -- two copies would eventually differ by an axis
/// convention, and the symptom would be geometry landing somewhere the user did not draw it.
sketch::SketchFrame frameOf(const kernel::PlaneFrame& measured) {
    sketch::SketchFrame frame;
    for (int i = 0; i < 3; ++i) {
        frame.origin[i] = measured.origin[i];
        frame.u[i] = measured.u[i];
        frame.v[i] = measured.v[i];
    }
    return frame;
}

/// Plane: an origin datum, as a real object in the document.
///
/// Not a special case hidden in the shell. A datum plane has to be pickable, highlightable,
/// nameable and referenceable by a sketch — which is the entire job description of a feature — and
/// making it one means sketch-on-a-face already works on it with no new code: the sketch resolves
/// the plane's face through the same element map as any other body's.
///
/// It produces a bounded square face because an unbounded plane cannot be picked, highlighted or
/// fitted to a view. The bound is display, not meaning: a sketch placed on it takes the PLANE, and
/// nothing stops geometry running past the visible edge.
kernel::Result<Output> computePlane(const ComputeContext& ctx) {
    std::int64_t plane = 0;
    if (const auto* stored = ctx.object.find("plane")) {
        if (const auto* v = std::get_if<std::int64_t>(stored)) plane = *v;
    }
    double size = 100.0;
    if (const auto* stored = ctx.object.find("size")) {
        if (const auto* v = std::get_if<units::Length>(stored)) size = v->base();
    }

    auto face = kernel::makePlane(static_cast<int>(plane), size);
    if (!face) return face.error();

    // nameprimitive, like Box and Cylinder: a datum has no input to inherit provenance from, so
    // its face is a primitive of this feature.
    naming::NamingContext naming(ctx.namingSerial, 0);
    auto map = naming.nameprimitive(face.value(), {});
    if (!map) return map.error();

    Output out;
    out.shape = face.value();
    out.map = std::move(map.value());
    return out;
}

kernel::Result<Output> computeSketch(const ComputeContext& ctx) {
    const auto* text = ctx.object.find("sketch");
    if (text == nullptr) {
        return Error{ErrorCode::InvalidInput, "This sketch has no geometry yet."};
    }
    const auto* serialized = std::get_if<std::string>(text);
    if (serialized == nullptr) {
        return Error{ErrorCode::InvalidInput, "'sketch' must be the serialised sketch text."};
    }

    auto sketch = sketch::Sketch::deserialize(*serialized);
    if (!sketch) return sketch.error();

    // A sketch placed on a face has to be LOCATED before it means anything. Until this runs, its
    // 2D coordinates have no 3D interpretation, and Sketch::toWire refuses rather than falling
    // back to a global plane — a fallback would build the profile somewhere the user never drew
    // it, silently, with every downstream feature agreeing.
    //
    // This is also where the face reference becomes a real dependency. The face's owning feature
    // is an INPUT, so its output arrives in ctx.inputs with its element map, AND Engine::cacheKeyOf
    // folds that feature's cache key into this one. Naming the face in text alone would leave the
    // sketch cached against the face's old position after an edit moved it — the Import bug in its
    // third costume.
    if (sketch.value().needsResolution()) {
        // The FIRST input. A sketch has exactly one body today, so this is unambiguous — but it
        // is unambiguous by convention rather than by construction, and a second ObjectId property
        // on a sketch would silently change which body the face is resolved against. If a sketch
        // ever gains another reference, this must select by property name instead.
        if (ctx.inputs.empty() || ctx.inputs.front() == nullptr) {
            return Error{ErrorCode::InvalidInput,
                         "This sketch is placed on a face, but nothing tells it which body.",
                         "SketchPlane::Kind::Face with no input feature to resolve against"};
        }

        const std::string& text = sketch.value().placement().face;
        if (text.empty()) {
            return Error{ErrorCode::InvalidInput,
                         "This sketch is placed on a face it does not name."};
        }

        const auto found = ctx.inputs.front()->map.resolve(naming::ElementName::parse(text));
        if (!found) {
            // NamingLost, not a fallback. The face was deleted or an edit changed it beyond
            // recognition, and the naming layer exists precisely so this is detectable rather
            // than being papered over by moving the user's sketch somewhere else.
            return Error{ErrorCode::NamingLost,
                         "The face this sketch is drawn on no longer exists.",
                         "could not resolve element '" + text + "'"};
        }

        const auto measured = kernel::planeOf(*found);
        if (!measured) return measured.error();
        sketch.value().setResolvedFrame(frameOf(measured.value()));
    }

    // Solved on every recompute rather than trusting the stored coordinates. The stored positions
    // are a starting point; the CONSTRAINTS are the definition. If a dimension was edited, this is
    // where the geometry catches up -- and it is why a sketch is parametric at all.
    const auto report = sketch.value().solve();
    if (!report.solved) {
        return Error{ErrorCode::NotDone, "This sketch could not be solved.", report.message};
    }
    if (!report.conflicting.empty()) {
        return Error{ErrorCode::InvalidInput,
                     "This sketch is over-constrained.", report.message};
    }

    // A face when the curves close, the curves themselves when they do not.
    //
    // A sketch is valid geometry on its own. Requiring a closed profile HERE made drawing a single
    // line an error on the feature: the moment the user drew anything that was not yet a loop, the
    // model tree showed the sketch as failed and the viewport lost it. The closed-profile
    // requirement belongs to whatever consumes the sketch, and computeExtrude now states it.
    // A sketch with nothing drawn on it is not an error — it is the state every sketch starts in,
    // one keystroke after Start Sketch. Failing here put an ERR badge on a feature the user had
    // only just created and had no way to fix except by drawing something.
    if (sketch.value().geometry().empty()) {
        auto empty = kernel::compound({});
        if (!empty) return empty.error();
        Output out;
        out.shape = empty.value();
        return out;
    }

    auto face = sketch.value().toFace();
    if (!face) {
        auto edges = sketch.value().toEdges();
        // Only an EMPTY sketch fails now — and only because a feature that produced nothing at all
        // would be indistinguishable from one that had not been computed.
        if (!edges) return edges.error();
        naming::NamingContext openNaming(ctx.namingSerial, 0);
        auto openMap = openNaming.nameprimitive(edges.value(), {});
        if (!openMap) return openMap.error();
        Output open;
        open.shape = edges.value();
        open.map = std::move(openMap.value());
        return open;
    }

    // nameprimitive, not propagate: a sketch has no input shape to inherit provenance from, exactly
    // like a Box. Its faces and edges are primitives of this feature.
    naming::NamingContext naming(ctx.namingSerial, 0);
    auto map = naming.nameprimitive(face.value(), {});
    if (!map) return map.error();

    Output out;
    out.shape = face.value();
    out.map = std::move(map.value());
    return out;
}

/// Revolve: a profile swept about an axis.
///
/// The axis is a named EDGE, resolved through the profile's own element map — which is how every CAD
/// application does it, because "revolve about this line" is a thing the user can point at. It is
/// NOT defaulted: a revolve with a guessed axis produces a solid somewhere the user did not ask for,
/// and unlike a wrong distance that is not obvious from looking at it.
kernel::Result<Output> computeRevolve(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 1) {
        return Error{ErrorCode::InvalidInput, "A revolve needs exactly one profile."};
    }
    if (ctx.inputs[0]->shape.type() != kernel::ShapeType::Face) {
        return Error{ErrorCode::InvalidInput,
                     "A revolve needs a closed profile. This sketch's curves do not form one.",
                     "input shape is not a face"};
    }

    const auto* named = ctx.object.find("axis");
    const auto* axisName = named != nullptr ? std::get_if<naming::ElementName>(named) : nullptr;
    if (axisName == nullptr) {
        return Error{ErrorCode::InvalidInput,
                     "A revolve needs an axis. Select a straight edge to turn the profile about."};
    }

    const auto edge = ctx.inputs[0]->map.resolve(*axisName);
    if (!edge) {
        // NamingLost rather than a fallback, for the same reason a sketch's face reference is: the
        // edge was deleted or changed beyond recognition, and revolving about something else would
        // silently produce a different part.
        return Error{ErrorCode::NamingLost, "The axis this revolve turns about no longer exists."};
    }
    const auto line = kernel::lineOf(*edge);
    if (!line) return line.error();

    // Defaults to a full turn, which is what a revolve usually is.
    double angle = 2.0 * std::numbers::pi;
    if (const auto* stored = ctx.object.find("angle")) {
        if (const auto* v = std::get_if<units::Angle>(stored)) angle = v->base();
    }

    auto op = kernel::revolve(ctx.inputs[0]->shape, line.value().origin, line.value().direction,
                              angle);
    if (!op) return op.error();
    return nameResult(op.value(), ctx.inputs, ctx.namingSerial);
}

/// Hole: a cylindrical cut, drilled into a face.
///
/// Placed at the centre of a named planar face and running along its INWARD normal, which is the
/// simplest placement that is never ambiguous. Fusion and Inventor also place holes at sketch
/// points, which is the fuller version and needs a sketch-point picker first; "a hole in the middle
/// of this face" is a real operation on its own and is reachable today.
kernel::Result<Output> computeHole(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 1) {
        return Error{ErrorCode::InvalidInput, "A hole needs exactly one body to drill into."};
    }

    const auto* named = ctx.object.find("face");
    const auto* faceName = named != nullptr ? std::get_if<naming::ElementName>(named) : nullptr;
    if (faceName == nullptr) {
        return Error{ErrorCode::InvalidInput, "A hole needs a flat face to be drilled into."};
    }
    const auto face = ctx.inputs[0]->map.resolve(*faceName);
    if (!face) {
        return Error{ErrorCode::NamingLost, "The face this hole is drilled into no longer exists."};
    }
    const auto plane = kernel::planeOf(*face);
    if (!plane) return plane.error();

    auto diameter = require<Length>(ctx.object, "diameter");
    if (!diameter) return diameter.error();
    auto depth = require<Length>(ctx.object, "depth");
    if (!depth) return depth.error();

    // The face's centre, not its plane's origin: a plane's origin is wherever OCCT parameterised it
    // from, which can be far outside the face itself.
    const auto centre = face->measure();
    const double at[3]{centre.cx, centre.cy, centre.cz};

    const auto& f = plane.value();
    double normal[3]{f.u[1] * f.v[2] - f.u[2] * f.v[1], f.u[2] * f.v[0] - f.u[0] * f.v[2],
                     f.u[0] * f.v[1] - f.u[1] * f.v[0]};

    // INWARD. The face normal may point either way depending on how the face was built, and a hole
    // drilled outward cuts nothing at all — it would report success and change the part not at all,
    // which is the worst kind of failure. Decided by asking which direction has material: the body's
    // centre of mass is on the inside.
    const auto body = ctx.inputs[0]->shape.measure();
    const double toCentre[3]{body.cx - at[0], body.cy - at[1], body.cz - at[2]};
    if (normal[0] * toCentre[0] + normal[1] * toCentre[1] + normal[2] * toCentre[2] < 0.0) {
        for (double& n : normal) n = -n;
    }

    auto tool = kernel::makeCylinderAt(at, normal, diameter.value().base() * 0.5,
                                       depth.value().base());
    if (!tool) return tool.error();

    auto cut = kernel::booleanCut(ctx.inputs[0]->shape, tool.value().shape());
    if (!cut) return cut.error();
    return nameResult(cut.value(), ctx.inputs, ctx.namingSerial);
}

/// Extrude: a profile swept into a solid.
///
/// Takes its direction from the sketch's PLANE normal rather than a user-supplied vector. An
/// extrude perpendicular to its profile is what the command means in every CAD application, and
/// letting the two disagree produces a sheared solid nobody asked for.
kernel::Result<Output> computeExtrude(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 1) {
        return Error{ErrorCode::InvalidInput, "An extrude needs exactly one profile."};
    }
    // The requirement lives HERE, not on the sketch. A sketch whose curves do not close is a
    // perfectly good sketch; it is only this operation that cannot use one, and saying so here
    // means the user sees the complaint against the extrude they just asked for rather than
    // against the sketch they were drawing.
    if (ctx.inputs[0]->shape.type() != kernel::ShapeType::Face) {
        return Error{ErrorCode::InvalidInput,
                     "An extrude needs a closed profile. This sketch's curves do not form one.",
                     "input shape is not a face"};
    }
    auto distance = require<Length>(ctx.object, "distance");
    if (!distance) return distance.error();
    if (distance.value().base() == 0.0) {
        return Error{ErrorCode::InvalidInput, "An extrude needs a non-zero distance."};
    }

    const double d = distance.value().base();
    double dx = 0.0;
    double dy = 0.0;
    double dz = d;

    // Where the direction comes from, and why it is not one rule.
    //
    // A stored plane index (0=XY, 1=XZ, 2=YZ) only ever described the three GLOBAL planes. A sketch
    // drawn on a face of the model has no index that means anything, so it extruded along whichever
    // global axis the index happened to hold -- and for a sketch on a side face that direction lies
    // IN the profile's own plane. Sweeping a face along a direction it contains produces a
    // zero-volume sheet, so the feature reported success and built nothing.
    //
    // So: no index means measure the profile, which is what this function's contract always claimed
    // to do and is right for any plane, global or not.
    //
    // An index that IS present still wins, and that is a compatibility decision rather than a
    // geometric one. The three global planes have always grown towards the positive axis, and the
    // XZ frame's own normal is -Y (u x v with u=x, v=z), so measuring it would silently reverse
    // every existing XZ extrude -- models built before today would come back inside out. The index
    // is the record of what the document meant when it was written, and it is honoured.
    if (const auto* stored = ctx.object.find("plane")) {
        std::int64_t plane = 0;
        if (const auto* v = std::get_if<std::int64_t>(stored)) plane = *v;
        if (plane == 1) {        // XZ: the sketch spans x and z, so it grows along y
            dy = d;
            dz = 0.0;
        } else if (plane == 2) { // YZ
            dx = d;
            dz = 0.0;
        }
    } else if (const auto measured = kernel::planeOf(ctx.inputs[0]->shape)) {
        const auto& f = measured.value();
        // u x v: the profile's normal, in the same right-handed order PlaneFrame stores.
        dx = (f.u[1] * f.v[2] - f.u[2] * f.v[1]) * d;
        dy = (f.u[2] * f.v[0] - f.u[0] * f.v[2]) * d;
        dz = (f.u[0] * f.v[1] - f.u[1] * f.v[0]) * d;
    }

    auto op = kernel::extrude(ctx.inputs[0]->shape, dx, dy, dz);
    if (!op) return op.error();
    return nameResult(op.value(), ctx.inputs, ctx.namingSerial);
}

kernel::Result<Output> computeTranslate(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 1) {
        return Error{ErrorCode::InvalidInput, "A move needs exactly one input shape."};
    }
    auto dx = require<Length>(ctx.object, "dx");
    if (!dx) return dx.error();
    auto dy = require<Length>(ctx.object, "dy");
    if (!dy) return dy.error();
    auto dz = require<Length>(ctx.object, "dz");
    if (!dz) return dz.error();

    auto op = kernel::translate(ctx.inputs[0]->shape, dx.value().base(), dy.value().base(),
                                dz.value().base());
    if (!op) return op.error();
    return nameResult(op.value(), ctx.inputs, ctx.namingSerial);
}

/// Merges the coplanar faces a join leaves behind, and carries the names across.
///
/// # Why a join needs this at all
///
/// Fusing two boxes that meet leaves the walls they share as SEPARATE faces lying on one plane.
/// The solid is correct and its volume is right, but it is drawn with a line across every place two
/// copies met -- so a pattern of three overlapping boxes came out looking like four bands welded
/// together rather than one slab. Measured on exactly that: 18 faces before, 6 after, identical
/// volume.
///
/// # Why the names have to be propagated through it
///
/// `unifySameDomain` changes topology, so every name bound to a face it merges is a name for
/// something that no longer exists. Naming the unified result is not optional and not cosmetic --
/// it is the same ordering rule the importer follows for healing, for the same reason: name AFTER
/// the topology stops changing, or the map describes a shape nobody has.
///
/// The operation reports its merges as `Modified`, which is exactly what `propagate` reads, so the
/// two faces that became one arrive as a merge and both references stay alive.
kernel::Result<Output> mergedFaces(const kernel::Shape& shape, naming::ElementMap map,
                                   std::uint32_t serial, std::uint16_t opTag) {
    auto unified = kernel::unifySameDomain(shape);
    if (!unified) return unified.error();

    naming::NamingContext naming(serial, opTag);
    auto merged = naming.propagate(unified.value(), {&shape}, {&map});
    if (!merged) return merged.error();
    return Output{unified.value().shape(), std::move(merged).value()};
}

/// Mirrors a body about the plane of one of its own faces, and joins the reflection to it.
///
/// # Why a face rather than three numbers
///
/// A mirror needs a plane, and the input vocabulary describes selections and typed values -- there
/// is no way to declare "a plane" and no honest way to spell one as three lengths a user would have
/// to work out. A face IS a plane, the user already has one under the pointer, and "reflect this
/// part about that face" is how the operation is asked for out loud.
///
/// It also makes the common case the default one. Mirroring a part about its own mating face is the
/// symmetric-bracket case -- the mirrored half welded to its original -- and the copy lands exactly
/// against the face it was reflected in, so joining them is what the user meant rather than a
/// second body sharing a surface.
///
/// # Why the copy is NAMED rather than propagated
///
/// A rigid transform reports its elements as `Modified` -- the same element, moved -- which is
/// right for a move and wrong for a copy. Propagating would give the reflection the source's own
/// names, and the fuse that follows would then find two elements answering to one name and refuse.
/// `nameCopy` stamps the reflection as a Generated copy of what it came from, which is both true
/// and distinct. See ADR 0005 and tests/acceptance/mirror_copy.cpp.
kernel::Result<Output> computeMirror(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 1) {
        return Error{ErrorCode::InvalidInput, "A mirror needs exactly one body."};
    }

    const auto* named = ctx.object.find("face");
    const auto* faceName = named != nullptr ? std::get_if<naming::ElementName>(named) : nullptr;
    if (faceName == nullptr) {
        return Error{ErrorCode::InvalidInput, "A mirror needs a flat face to reflect about."};
    }
    const auto face = ctx.inputs[0]->map.resolve(*faceName);
    if (!face) {
        return Error{ErrorCode::NamingLost, "The face this mirror reflects about no longer exists."};
    }
    const auto plane = kernel::planeOf(*face);
    if (!plane) return plane.error();

    auto reflected = kernel::mirror(ctx.inputs[0]->shape, plane.value().origin, plane.value().normal);
    if (!reflected) return reflected.error();

    // opTag 1 for the copy, 2 for the join. Distinct, because both steps name elements of the same
    // feature and a shared tag would let the two produce identical names.
    naming::NamingContext copyNaming(ctx.namingSerial, 1);
    auto copyMap = copyNaming.nameCopy(reflected.value(), ctx.inputs[0]->shape, ctx.inputs[0]->map,
                                       {1, 0});
    if (!copyMap) return copyMap.error();
    const kernel::Shape copyShape = reflected.value().shape();

    auto joined = kernel::booleanFuse(ctx.inputs[0]->shape, copyShape);
    if (!joined) return joined.error();

    naming::NamingContext joinNaming(ctx.namingSerial, 2);
    const kernel::Shape* shapes[] = {&ctx.inputs[0]->shape, &copyShape};
    const naming::ElementMap* maps[] = {&ctx.inputs[0]->map, &copyMap.value()};
    auto map = joinNaming.propagate(joined.value(), {shapes[0], shapes[1]}, {maps[0], maps[1]});
    if (!map) return map.error();
    return mergedFaces(joined.value().shape(), std::move(map).value(), ctx.namingSerial, 3);
}

/// Repeats a body along a direction, and joins the copies to it.
///
/// # What a count means here
///
/// `count` is the TOTAL number of bodies including the original, which is what every CAD system
/// means by it and what a user counts when they look at the result. A count of 1 is therefore a
/// legal no-op rather than an error: it is what the panel shows before anyone has typed, and
/// refusing it would make the field impossible to clear.
///
/// # Why the instance coordinate, and not a running index
///
/// Each copy is named with its STEP ALONG THE DIRECTION, so instance 3 means "three spacings from
/// the seed" whatever the spacing is and whichever way it points. Naming them 1, 2, 3 in creation
/// order would reassign every reference the moment the direction flipped -- a fillet placed on the
/// third instance would silently move to a different copy, which is the exact failure the naming
/// layer exists to prevent. `NamingContext::Instance` carries that coordinate.
kernel::Result<Output> computePattern(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 1) {
        return Error{ErrorCode::InvalidInput, "A pattern needs exactly one body to repeat."};
    }
    auto count = require<std::int64_t>(ctx.object, "count");
    if (!count) return count.error();
    if (count.value() < 1) {
        return Error{ErrorCode::InvalidInput, "A pattern needs a count of at least one."};
    }
    // An upper bound, and not a decorative one.
    //
    // Each instance costs a boolean fuse on the CALLING thread, and recompute is synchronous with
    // no cancellation, so a four-figure count is not a slow pattern -- it is an application that
    // has stopped answering, with no way back. The naming has a harder limit underneath: the op tag
    // is 16 bits and each instance uses two, so a count past 32768 wraps and two instances mint
    // identical names. Refusing here means that arithmetic can never be reached, rather than
    // relying on nobody being patient enough to get there.
    constexpr std::int64_t kMostInstances = 1000;
    if (count.value() > kMostInstances) {
        return Error{ErrorCode::InvalidInput,
                     "A pattern of more than " + std::to_string(kMostInstances)
                         + " is more than this can build.",
                     "each instance is a boolean on the calling thread; see computePattern"};
    }
    auto dx = require<Length>(ctx.object, "dx");
    if (!dx) return dx.error();
    auto dy = require<Length>(ctx.object, "dy");
    if (!dy) return dy.error();
    auto dz = require<Length>(ctx.object, "dz");
    if (!dz) return dz.error();

    // The original, unchanged, is instance 0. Its names are the ones every existing reference
    // already uses, so the seed keeps them and only the copies are stamped.
    kernel::Shape shape = ctx.inputs[0]->shape;
    naming::ElementMap map = ctx.inputs[0]->map;

    const double step[3] = {dx.value().base(), dy.value().base(), dz.value().base()};
    if (count.value() > 1 && step[0] == 0.0 && step[1] == 0.0 && step[2] == 0.0) {
        // Refused rather than built. Every copy would land on the original, the fuse would return
        // one body, and the feature would report success having done nothing visible -- which reads
        // as the pattern being broken rather than as the spacing being unset.
        return Error{ErrorCode::InvalidInput,
                     "A pattern needs a spacing, or every copy lands on the original."};
    }

    for (std::int64_t i = 1; i < count.value(); ++i) {
        const auto offset = static_cast<double>(i);
        auto moved = kernel::translate(ctx.inputs[0]->shape, step[0] * offset, step[1] * offset,
                                       step[2] * offset);
        if (!moved) return moved.error();

        // Two op tags per instance, and they must not collide across instances: the copy of
        // instance i and the join that follows it are separate naming operations.
        const auto tag = static_cast<std::uint16_t>(i * 2);
        naming::NamingContext copyNaming(ctx.namingSerial, tag);
        auto copyMap = copyNaming.nameCopy(moved.value(), ctx.inputs[0]->shape, ctx.inputs[0]->map,
                                           {static_cast<std::uint16_t>(i), 0});
        if (!copyMap) return copyMap.error();
        const kernel::Shape copyShape = moved.value().shape();

        auto joined = kernel::booleanFuse(shape, copyShape);
        if (!joined) return joined.error();

        naming::NamingContext joinNaming(ctx.namingSerial,
                                         static_cast<std::uint16_t>(tag + 1));
        auto next = joinNaming.propagate(joined.value(), {&shape, &copyShape},
                                         {&map, &copyMap.value()});
        if (!next) return next.error();
        shape = joined.value().shape();
        map = std::move(next).value();
    }

    if (count.value() == 1) return Output{shape, std::move(map)};   // nothing was joined
    return mergedFaces(shape, std::move(map), ctx.namingSerial,
                       static_cast<std::uint16_t>(count.value() * 2));
}

kernel::Result<Output> computeImport(const ComputeContext& ctx) {
    auto path = require<std::string>(ctx.object, "path");
    if (!path) return path.error();

    // The ordering that matters: read -> heal -> name. Healing changes topology, so naming
    // an imported shape before healing it would silently invalidate every name.
    // cad::io::importFile guarantees the first two; we do the third.
    static const io::FormatRegistry registry = io::FormatRegistry::builtins();

    io::ImportOptions options;
    if (const auto* assumed = ctx.object.find("assumedUnits")) {
        if (const auto* v = std::get_if<std::int64_t>(assumed)) {
            options.assumedUnits = static_cast<units::UnitSystem>(*v);
        }
    }

    auto imported = io::importFile(registry, path.value(), options);
    if (!imported) return imported.error();

    // A MESH is not named at all, and that is not a shortcut.
    //
    // A mesh format carries no B-rep topology, so the reader turns every triangle into a face. One
    // NASA STL in the corpus is 37,827 of them. Naming those means measuring every triangle,
    // sorting them, and deriving a boundary name for each of ~113,000 edges and their vertices --
    // measured at 13 s to read and over a minute to name, for names that are worthless:
    //
    //   * nobody references "triangle 24,912"; a triangle is not a feature;
    //   * any re-tessellation renumbers all of them, so the names are not stable either;
    //   * and no feature can be built on one, which is what a name is FOR.
    //
    // So a mesh comes in as geometry you can see, measure and export onward, with no element names.
    // The format itself says which it is -- Capabilities::solids is documented as "true B-rep;
    // false means the format is mesh-only" -- so this asks the provider rather than guessing from
    // face counts. A STEP file with 37,000 faces is a real B-rep and still gets named.
    if (const auto* provider = registry.forPath(path.value());
        provider != nullptr && !provider->capabilities().solids) {
        return Output{imported.value().shape, naming::ElementMap{}};
    }

    // BEST EFFORT, because this is geometry we read rather than geometry we built.
    //
    // Foreign files routinely carry duplicate or coincident faces that no measurement can tell
    // apart. Refusing the import over them means a defect in a corner of the part stops the user
    // opening the file at all -- and what they usually want is to look at it, measure it, and
    // export it onward. So the part opens, the faces that could not be identified are simply
    // absent from the map, and attaching a feature to one of them fails at that moment, with a
    // message about that face rather than about the file.
    naming::NamingContext naming(ctx.namingSerial, 0);
    auto map = naming.nameprimitive(imported.value().shape, {},
                                    naming::NamingContext::Naming::BestEffort);
    if (!map) {
        // Still reachable: a collision is a fault in the naming layer, not a property of the file.
        return Error{ErrorCode::NamingLost,
                     "This file was read, but its geometry could not be identified well "
                     "enough to attach features to.",
                     map.error().detail};
    }
    return Output{imported.value().shape, std::move(map).value()};
}

}  // namespace

}  // namespace cad::recompute

namespace cad::features {

bool resolveSketchFrame(const document::Document& doc, document::ObjectId sketchId,
                        sketch::Sketch& sketch) {
    if (!sketch.needsResolution()) return true;   // a global-plane sketch already knows where it is

    const auto object = doc.find(sketchId);
    if (!object) return false;

    const std::string& text = sketch.placement().face;
    if (text.empty()) return false;

    // The body the face belongs to, found through the sketch's own inputs rather than by searching
    // the document: the input IS the record of which body this sketch was placed against, and
    // guessing would pick a different one after a copy-paste.
    for (const document::ObjectId input : object->inputs()) {
        const auto body = doc.find(input);
        if (!body || body->output() == nullptr) continue;
        const auto found = body->output()->map.resolve(naming::ElementName::parse(text));
        if (!found) continue;
        const auto measured = kernel::planeOf(*found);
        if (!measured) continue;
        // Qualified: the converter lives beside the feature that first needed it, in
        // cad::recompute, and this is the same translation unit.
        sketch.setResolvedFrame(recompute::frameOf(measured.value()));
        return true;
    }
    return false;
}

recompute::FeatureRegistry builtins() {
    using namespace cad::recompute;
    using Of = FeatureInputs::Requirement::Of;
    using Kind = FeatureInputs::Value::Kind;

    // Every requirement and every default, in ONE place per feature. The command's enablement, its
    // panel, its refusal message and the property names it stores are all read from here -- see
    // FeatureInputs.h for the four-way drift this replaces.
    const auto lengths = [](std::initializer_list<std::tuple<const char*, const char*, double>> v) {
        std::vector<FeatureInputs::Value> out;
        for (const auto& [name, label, mm] : v) out.push_back({name, label, Kind::Length, mm});
        return out;
    };

    FeatureRegistry r;
    r.add({"Box", 1, computeBox, nullptr,
           // No selection: a primitive is the one kind of feature that needs nothing to exist.
           {{}, {}, lengths({{"dx", "Length", 100.0}, {"dy", "Width", 60.0},
                             {"dz", "Height", 40.0}})}});
    r.add({"Cylinder", 1, computeCylinder, nullptr,
           {{}, {}, lengths({{"radius", "Radius", 25.0}, {"height", "Height", 80.0}})}});
    r.add({"Fillet", 1, computeFillet, nullptr,
           // Picked edges, or a whole body whose edges we take wholesale. The body alternative
           // additionally needs the body to HAVE edges, which is a question about geometry rather
           // than about the selection -- so Fillet keeps a supplementary predicate for that half.
           {{{Of::Edge, 1, 0, ""}, {Of::Object, 1, 1, ""}},
            "Select edges, or one body to round every edge of.",
            lengths({{"radius", "Radius", 5.0}})}});
    r.add({"Chamfer", 1, computeChamfer, nullptr,
           {{{Of::Edge, 1, 0, ""}, {Of::Object, 1, 1, ""}},
            "Select edges, or one body to bevel every edge of.",
            lengths({{"distance", "Distance", 3.0}})}});
    r.add({"Cut", 1, computeBoolean<&kernel::booleanCut>, nullptr,
           {{{Of::Object, 2, 2, ""}}, "Select two bodies.", {}}});
    r.add({"Fuse", 1, computeBoolean<&kernel::booleanFuse>, nullptr,
           {{{Of::Object, 2, 2, ""}}, "Select two bodies.", {}}});
    r.add({"Common", 1, computeBoolean<&kernel::booleanCommon>, nullptr,
           {{{Of::Object, 2, 2, ""}}, "Select two bodies.", {}}});
    r.add({"Sketch", 1, computeSketch});
    r.add({"Plane", 1, computePlane});
    r.add({"Revolve", 1, computeRevolve, nullptr,
           // An edge OF A SKETCH. The axis is resolved in the profile's own element map, so an edge
           // of some other body names nothing there.
           //
           // A full turn by default, which is what a revolve usually is -- and what computeRevolve
           // falls back to when no angle is stored, so the panel and the compute agree.
           {{{Of::Edge, 1, 1, "Sketch"}},
            "Select one straight edge of a sketch to revolve it about.",
            {{"angle", "Angle", Kind::Angle, 2.0 * std::numbers::pi}}}});
    r.add({"Hole", 1, computeHole, nullptr,
           // One face is its ENTIRE geometric input: position and direction both come from it, so
           // there is nothing to pick afterwards and nothing to guess.
           //
           // 8 mm and 10 mm: an ordinary clearance hole rather than a round number that fits
           // nothing. Depth is second because it is what a user changes most, so Tab reaches it.
           {{{Of::Face, 1, 1, ""}}, "Select one flat face to put the hole in.",
            lengths({{"diameter", "Diameter", 8.0}, {"depth", "Depth", 10.0}})}});
    r.add({"Extrude", 1, computeExtrude, nullptr,
           // A SKETCH, not any object. Offering Extrude on a box lights a button that then refuses,
           // which is worse than a dim one.
           {{{Of::Object, 1, 1, "Sketch"}}, "Select a sketch to extrude.",
            lengths({{"distance", "Distance", 10.0}})}});
    r.add({"Mirror", 1, computeMirror, nullptr,
           // A FACE, and the body it belongs to is the one reflected. The face is the whole
           // geometric input -- it supplies the plane and identifies the body at once -- so there
           // is nothing left to type and nothing to guess.
           {{{Of::Face, 1, 1, ""}}, "Select a flat face to mirror the body about.", {}}});
    r.add({"Pattern", 1, computePattern, nullptr,
           // Count INCLUDES the original, which is what a user counts when looking at the result.
           // A count of 1 is a no-op and a spacing of 0 is refused, so neither is a useful default.
           //
           // 150 mm because the default Box is 100 mm long, so the default gesture produces three
           // SEPARATE copies with a gap between them -- which is what a pattern looks like. The
           // previous 50 made every copy overlap its neighbour and fuse into one slab: correct
           // arithmetic, and it read as the feature being broken. Arbitrary in the same way the
           // box's own 100 x 60 x 40 is arbitrary, and for the same reason -- a default is a
           // demonstration of what the tool does.
           //
           // A spacing derived from the body's own size would be better and is not expressible:
           // the declaration is read before anything is selected.
           {{{Of::Object, 1, 1, ""}}, "Select one body to repeat.",
            {{"count", "Count", Kind::Count, 3.0},
             {"dx", "X spacing", Kind::Length, 150.0},
             {"dy", "Y spacing", Kind::Length, 0.0},
             {"dz", "Z spacing", Kind::Length, 0.0}}}});
    r.add({"Translate", 1, computeTranslate, nullptr,
           // Zero, not a guess. A move is a vector the user has in mind; a non-zero default would
           // move the part the moment the panel opened.
           {{{Of::Object, 1, 1, ""}}, "Select one body to move.",
            lengths({{"dx", "X", 0.0}, {"dy", "Y", 0.0}, {"dz", "Z", 0.0}})}});
    // Import is the ONE built-in that reads outside the document, so it is the one that has to
    // declare it. Everything else is a pure function of its properties.
    r.add({"Import", 1, computeImport,
           [](const document::ObjectData& object) -> std::vector<std::string> {
               if (const auto* p = object.find("path")) {
                   if (const auto* path = std::get_if<std::string>(p)) return {*path};
               }
               return {};
           }});
    return r;
}

}  // namespace cad::features
