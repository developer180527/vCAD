#include "cad/recompute/Engine.h"

#include "cad/kernel/Booleans.h"
#include "cad/kernel/Fillet.h"
#include "cad/kernel/Primitives.h"
#include "cad/kernel/Transform.h"

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

}  // namespace

FeatureRegistry FeatureRegistry::builtins() {
    FeatureRegistry r;
    r.add({"Box", 1, computeBox});
    r.add({"Cylinder", 1, computeCylinder});
    r.add({"Fillet", 1, computeFillet});
    r.add({"Chamfer", 1, computeChamfer});
    r.add({"Cut", 1, computeBoolean<&kernel::booleanCut>});
    r.add({"Fuse", 1, computeBoolean<&kernel::booleanFuse>});
    r.add({"Common", 1, computeBoolean<&kernel::booleanCommon>});
    r.add({"Translate", 1, computeTranslate});
    return r;
}

}  // namespace cad::recompute
