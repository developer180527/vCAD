#include "Model.h"

#include <algorithm>

namespace cadtest {

using cad::kernel::BoxFace;
using cad::naming::NamingContext;
using cad::naming::NameStep;
using cad::naming::Provenance;

Result<Model> box(double dx, double dy, double dz, std::uint32_t serial) {
    auto built = cad::kernel::makeBox(dx, dy, dz);
    if (!built) return built.error();

    NamingContext ctx(serial, /*opTag*/ 0);
    auto map = ctx.nameprimitive(built.value().op.shape(), built.value().taggedFaces);
    if (!map) return map.error();

    return Model{built.value().op.shape(), std::move(map).value(), serial};
}

Result<ElementName> faceName(const Model& m, BoxFace f) {
    // Reconstructs the name the primitive namer assigned: the model's primitive serial,
    // Primitive provenance, discriminator = the positional face tag.
    NameStep s;
    s.featureSerial = m.primitiveSerial;
    s.opTag = 0;
    s.provenance = Provenance::Primitive;
    s.discriminator = static_cast<std::uint32_t>(f);
    ElementName name({s});
    if (!m.map.resolve(name)) {
        return Error{ErrorCode::NamingLost, "Box face not found in element map.",
                     name.toString()};
    }
    return name;
}

Result<ElementName> edgeBetween(const Model& m, BoxFace a, BoxFace b) {
    auto na = faceName(m, a);
    if (!na) return na.error();
    auto nb = faceName(m, b);
    if (!nb) return nb.error();

    // A boundary name is a pure function of the bounding faces' names — no feature serial.
    NameStep s;
    s.provenance = Provenance::Boundary;
    s.parents = {na.value().digest(), nb.value().digest()};
    ElementName edge({s});

    if (m.map.resolveAll(edge).empty()) {
        return Error{ErrorCode::NamingLost, "No edge bounded by those two faces.",
                     edge.toString()};
    }
    return edge;
}

Result<Model> fillet(const Model& base, const ElementName& edge, double radius,
                     std::uint32_t serial) {
    // resolveAll, not resolve: if the edge was split by an intervening feature, the user's
    // intent ("fillet this edge") covers every child.
    const auto edges = base.map.resolveAll(edge);
    if (edges.empty()) {
        return Error{ErrorCode::NamingLost,
                     "The edge selected for this fillet no longer exists.",
                     edge.toString()};
    }

    auto op = cad::kernel::filletEdges(base.shape, edges, radius);
    if (!op) return op.error();

    NamingContext ctx(serial, 0);
    auto map = ctx.propagate(op.value(), {&base.shape}, {&base.map});
    if (!map) return map.error();

    return Model{op.value().shape(), std::move(map).value(), base.primitiveSerial};
}

Result<Model> chamfer(const Model& base, const ElementName& edge, double distance,
                      std::uint32_t serial) {
    const auto edges = base.map.resolveAll(edge);
    if (edges.empty()) {
        return Error{ErrorCode::NamingLost,
                     "The edge selected for this chamfer no longer exists.",
                     edge.toString()};
    }

    auto op = cad::kernel::chamferEdges(base.shape, edges, distance);
    if (!op) return op.error();

    NamingContext ctx(serial, 0);
    auto map = ctx.propagate(op.value(), {&base.shape}, {&base.map});
    if (!map) return map.error();

    return Model{op.value().shape(), std::move(map).value(), base.primitiveSerial};
}

Result<Model> fuseOnly(const Model& a, const Model& b, std::uint32_t serial) {
    auto op = cad::kernel::booleanFuse(a.shape, b.shape);
    if (!op) return op.error();

    NamingContext ctx(serial, 0);
    auto map = ctx.propagate(op.value(), {&a.shape, &b.shape}, {&a.map, &b.map});
    if (!map) return map.error();

    return Model{op.value().shape(), std::move(map).value(), a.primitiveSerial};
}

Result<Model> cut(const Model& base, const Model& tool, std::uint32_t serial) {
    auto op = cad::kernel::booleanCut(base.shape, tool.shape);
    if (!op) return op.error();

    NamingContext ctx(serial, 0);
    auto map = ctx.propagate(op.value(), {&base.shape, &tool.shape}, {&base.map, &tool.map});
    if (!map) return map.error();

    return Model{op.value().shape(), std::move(map).value(), base.primitiveSerial};
}

Result<Model> fuseAndUnify(const Model& a, const Model& b, std::uint32_t serial) {
    auto fused = cad::kernel::booleanFuse(a.shape, b.shape);
    if (!fused) return fused.error();

    NamingContext ctx1(serial, 0);
    auto fusedMap = ctx1.propagate(fused.value(), {&a.shape, &b.shape}, {&a.map, &b.map});
    if (!fusedMap) return fusedMap.error();
    const Model fusedModel{fused.value().shape(), std::move(fusedMap).value(),
                           a.primitiveSerial};

    auto unified = cad::kernel::unifySameDomain(fusedModel.shape);
    if (!unified) return unified.error();

    NamingContext ctx2(serial, 1);
    auto map = ctx2.propagate(unified.value(), {&fusedModel.shape}, {&fusedModel.map});
    if (!map) return map.error();

    return Model{unified.value().shape(), std::move(map).value(), a.primitiveSerial};
}

Result<Model> translated(const Model& m, double dx, double dy, double dz,
                         std::uint32_t serial) {
    auto op = cad::kernel::translate(m.shape, dx, dy, dz);
    if (!op) return op.error();

    NamingContext ctx(serial, 0);
    auto map = ctx.propagate(op.value(), {&m.shape}, {&m.map});
    if (!map) return map.error();

    return Model{op.value().shape(), std::move(map).value(), m.primitiveSerial};
}

Result<Shape> openShellSolid(double dx, double dy, double dz) {
    auto built = cad::kernel::makeBox(dx, dy, dz);
    if (!built) return built.error();
    return cad::kernel::makeOpenShellSolid(built.value().op.shape());
}

std::vector<std::string> nameStrings(const Model& m) {
    std::vector<std::string> out;
    for (const auto& n : m.map.allNames()) out.push_back(n.toString());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace cadtest
