#pragma once

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"

#include <span>

#include <vector>

namespace cad::kernel {

/// A primitive plus the faces its constructor can name for us.
///
/// The tags matter: BRepPrimAPI_MakeBox exposes BottomFace()/TopFace()/FrontFace()/
/// BackFace()/LeftFace()/RightFace(). Using those instead of explorer indices is what makes
/// "the top face" survive a width change. Never name a primitive's faces by iteration order.
struct BoxResult {
    Operation op;
    /// Indexed by BoxFace below. Face tags are positional and therefore stable.
    std::vector<Shape> taggedFaces;
};

/// Axis-relative face tags for a box spanning (0,0,0)..(dx,dy,dz).
///
/// Named by axis on purpose. OCCT's own accessors are named Front/Back/Left/Right, and
/// their meaning is NOT what a reader assumes: BRepPrimAPI_MakeBox::FrontFace() is the
/// **x = dx** face and BackFace() is **x = 0**, while LeftFace() is **y = 0** and
/// RightFace() is **y = dy**. Verified against OCCT 8.0.1 — see tests/acceptance/debug_dump.
/// Passing those names through would guarantee that someone eventually fillets the wrong
/// edge and blames the naming layer.
///
/// The numeric values are the discriminators baked into persisted element names, so they
/// are part of the file format. Do not reorder them.
enum BoxFace : std::size_t {
    ZMin = 0,   ///< OCCT BottomFace(), z = 0
    ZMax,       ///< OCCT TopFace(),    z = dz
    XMax,       ///< OCCT FrontFace(),  x = dx
    XMin,       ///< OCCT BackFace(),   x = 0
    YMin,       ///< OCCT LeftFace(),   y = 0
    YMax,       ///< OCCT RightFace(),  y = dy
    BoxFaceCount
};

Result<BoxResult> makeBox(double dx, double dy, double dz);

Result<Operation> makeCylinder(double radius, double height);

/// A square planar face centred on the origin, lying in one of the three global planes.
///
/// The geometry behind a datum plane. A real datum is unbounded; a bounded face is what every CAD
/// application draws instead, because an infinite plane cannot be picked, highlighted or fitted to
/// a view. `size` is the edge length, so the face spans -size/2..+size/2 on both of its axes.
///
/// `plane`: 0 = XY, 1 = XZ, 2 = YZ — the same encoding a sketch's plane property uses, so the two
/// cannot drift apart into two spellings of the same three values.
Result<Shape> makePlane(int plane, double size);

/// Several shapes as one compound, without fusing them.
///
/// NOT a boolean union: a union of two touching solids is one solid with the shared faces gone,
/// which is a modelling decision. A compound keeps each body exactly as it is and simply carries
/// them together — which is what "export these three bodies to one file" means, and what every
/// exchange format expects at the top level.
/// A cylinder standing at `base`, running along `axis` for `height`.
///
/// The unplaced `makeCylinder` sits at the origin pointing along +Z, which is fine for a primitive a
/// user then moves. A hole cannot use it: the tool has to start on the face being drilled and run
/// INTO the material, and composing that from a translate and two rotations at the call site is
/// three chances to get an orientation wrong for something the kernel can place directly.
Result<Operation> makeCylinderAt(const double base[3], const double axis[3], double radius,
                                 double height);

/// An empty span gives an empty compound, which is what a feature that has produced nothing yet
/// returns. Callers that require geometry check for themselves.
Result<Shape> compound(std::span<const Shape>);

}  // namespace cad::kernel
