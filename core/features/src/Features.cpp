#include "cad/features/Builtins.h"

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

        // kernel::PlaneFrame and sketch::SketchFrame are separate types with the same shape,
        // because core/sketch must not depend on core/kernel. This is the one layer that
        // legitimately sees both, so the copy belongs here.
        sketch::SketchFrame frame;
        for (int i = 0; i < 3; ++i) {
            frame.origin[i] = measured.value().origin[i];
            frame.u[i] = measured.value().u[i];
            frame.v[i] = measured.value().v[i];
        }
        sketch.value().setResolvedFrame(frame);
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

    auto face = sketch.value().toFace();
    if (!face) return face.error();

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

/// Extrude: a profile swept into a solid.
///
/// Takes its direction from the sketch's PLANE normal rather than a user-supplied vector. An
/// extrude perpendicular to its profile is what the command means in every CAD application, and
/// letting the two disagree produces a sheared solid nobody asked for.
kernel::Result<Output> computeExtrude(const ComputeContext& ctx) {
    if (ctx.inputs.size() != 1) {
        return Error{ErrorCode::InvalidInput, "An extrude needs exactly one profile."};
    }
    auto distance = require<Length>(ctx.object, "distance");
    if (!distance) return distance.error();
    if (distance.value().base() == 0.0) {
        return Error{ErrorCode::InvalidInput, "An extrude needs a non-zero distance."};
    }

    // The plane is recorded on the extrude so it does not have to re-parse the sketch text. Defaults
    // to XY, which matches Sketch's own default.
    std::int64_t plane = 0;
    if (const auto* stored = ctx.object.find("plane")) {
        if (const auto* v = std::get_if<std::int64_t>(stored)) plane = *v;
    }
    const double d = distance.value().base();
    double dx = 0.0;
    double dy = 0.0;
    double dz = d;
    if (plane == 1) {        // XZ: the sketch spans x and z, so it grows along y
        dy = d;
        dz = 0.0;
    } else if (plane == 2) { // YZ
        dx = d;
        dz = 0.0;
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

    naming::NamingContext naming(ctx.namingSerial, 0);
    auto map = naming.nameprimitive(imported.value().shape, {});
    if (!map) {
        // Foreign geometry that we cannot fully name is usable but not safely referenceable.
        // Say which, rather than failing the import outright — the user can still see and
        // export the part.
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

recompute::FeatureRegistry builtins() {
    using namespace cad::recompute;
    FeatureRegistry r;
    r.add({"Box", 1, computeBox});
    r.add({"Cylinder", 1, computeCylinder});
    r.add({"Fillet", 1, computeFillet});
    r.add({"Chamfer", 1, computeChamfer});
    r.add({"Cut", 1, computeBoolean<&kernel::booleanCut>});
    r.add({"Fuse", 1, computeBoolean<&kernel::booleanFuse>});
    r.add({"Common", 1, computeBoolean<&kernel::booleanCommon>});
    r.add({"Sketch", 1, computeSketch});
    r.add({"Extrude", 1, computeExtrude});
    r.add({"Translate", 1, computeTranslate});
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
