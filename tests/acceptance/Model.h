#pragma once

// Test-side modelling helpers. These stand in for core/document and core/features, which
// arrive at M2 — M1 has no document model, so the tests wire features together by hand.
// Deliberately thin: if these need to get clever, the core API is wrong.

#include "cad/kernel/Booleans.h"
#include "cad/kernel/Fillet.h"
#include "cad/kernel/Primitives.h"
#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"
#include "cad/kernel/Transform.h"
#include "cad/naming/ElementMap.h"

#include <optional>
#include <string>
#include <vector>

namespace cadtest {

using cad::kernel::Error;
using cad::kernel::ErrorCode;
using cad::kernel::Result;
using cad::kernel::Shape;
using cad::naming::ElementMap;
using cad::naming::ElementName;

/// A shape plus its names. This is what a DocObject will hold at M2.
struct Model {
    Shape shape;
    ElementMap map;
    /// Feature serial of the primitive at the root of this model. Needed to reconstruct a
    /// face name, and distinct per primitive: two boxes created with the same serial would
    /// produce IDENTICAL face names and collide the moment they meet in a boolean.
    std::uint32_t primitiveSerial = 1;
};

/// The base box. `serial` must be unique among primitives that will meet in one operation.
Result<Model> box(double dx, double dy, double dz, std::uint32_t serial = 1);

/// The name of the edge shared by two of the box's tagged faces, e.g. Top and Front.
/// Computed from the two face names rather than hardcoded, which is the point: the caller
/// never touches an index.
Result<ElementName> edgeBetween(const Model& m,
                                cad::kernel::BoxFace a,
                                cad::kernel::BoxFace b);

/// The name of one of the box's tagged faces.
Result<ElementName> faceName(const Model& m, cad::kernel::BoxFace f);

/// Feature `serial`: fillet the named edge, resolving through the element map. Uses
/// resolveAll, so a reference to an edge that has since been split applies to every child.
Result<Model> fillet(const Model& base, const ElementName& edge, double radius,
                     std::uint32_t serial = 2);

/// Feature `serial`: cut `tool` out of `base`.
Result<Model> cut(const Model& base, const Model& tool, std::uint32_t serial = 3);

/// Feature `serial`: fuse, then unify coplanar faces — the merge case.
Result<Model> fuseAndUnify(const Model& a, const Model& b, std::uint32_t serial = 4);

Result<Model> translated(const Model& m, double dx, double dy, double dz,
                         std::uint32_t serial = 5);

/// Every element name in the model, sorted, as text. The determinism fixture compares these.
std::vector<std::string> nameStrings(const Model&);

}  // namespace cadtest
