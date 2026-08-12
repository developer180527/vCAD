#include "cad/render/Tessellate.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <limits>
#include <cmath>

namespace cad::render {
namespace {

using kernel::Error;
using kernel::ErrorCode;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= kFnvPrime;
    }
}

std::uint64_t quantise(double v) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(v * 1e9)));
}

void expand(Bounds& b, const gp_Pnt& p) {
    const float xyz[3]{static_cast<float>(p.X()), static_cast<float>(p.Y()),
                       static_cast<float>(p.Z())};
    for (int i = 0; i < 3; ++i) {
        b.min[i] = std::min(b.min[i], xyz[i]);
        b.max[i] = std::max(b.max[i], xyz[i]);
    }
}

}  // namespace

std::uint64_t TessellationSettings::digest() const noexcept {
    std::uint64_t h = kFnvOffset;
    mix(h, quantise(deflection));
    mix(h, quantise(angularDeflection));
    mix(h, relativeToSize ? 1u : 0u);
    return h;
}

kernel::Result<RenderMeshPtr> tessellate(const document::Output& output,
                                         const TessellationSettings& settings) {
    if (output.shape.isNull()) {
        return Error{ErrorCode::InvalidInput, "There is no geometry to display."};
    }
    if (settings.deflection <= 0.0 || settings.angularDeflection <= 0.0) {
        return Error{ErrorCode::InvalidInput, "Tessellation tolerances must be positive."};
    }

    auto built = kernel::guard("tessellate", [&]() -> std::shared_ptr<RenderMesh> {
        auto mesh = std::make_shared<RenderMesh>();
        for (int i = 0; i < 3; ++i) {
            mesh->bounds.min[i] = std::numeric_limits<float>::infinity();
            mesh->bounds.max[i] = -std::numeric_limits<float>::infinity();
        }

        TopoDS_Shape& shape = kernel::occt(const_cast<kernel::Shape&>(output.shape));

        // Clean first. This is NOT optional and the reason is easy to miss:
        //
        // OCCT stores triangulation as an attribute ON the shape, and
        // BRepMesh_IncrementalMesh is *incremental* — it keeps an existing triangulation it
        // considers adequate. Tessellating the same shape again with a finer angular tolerance
        // therefore returns the OLD mesh, unchanged. The symptom is worse than an obvious bug:
        // the cache correctly registers a miss and hands back geometry identical to the coarse
        // version, so a display-quality setting appears to work and silently does nothing.
        // Caught by the angular-tolerance test, which asserted more triangles and got 48 = 48.
        //
        // Cleaning mutates the shape's triangulation attribute. That is inherent to OCCT's
        // model — meshing writes triangulation into the shape regardless — and it is safe here
        // because nothing downstream reads the shape's own triangulation; we extract into our
        // RenderMesh and never look back.
        BRepTools::Clean(shape);

        // isInParallel = true. OCCT meshes faces concurrently, and for a part with hundreds
        // of faces that is most of the win available inside a single shape. Parallelism
        // ACROSS shapes is the caller's job (MeshCache) and matters far more at assembly
        // scale, where there are ~1000 unique shapes to get through.
        BRepMesh_IncrementalMesh mesher(shape, settings.deflection, settings.relativeToSize,
                                        settings.angularDeflection, /*isInParallel*/ true);
        mesher.Perform();
        if (!mesher.IsDone()) {
            throw std::runtime_error("meshing did not complete");
        }

        // ── Faces ────────────────────────────────────────────────────────────────────────
        //
        // Each face gets its OWN vertex block, never shared with a neighbour, and normals are
        // averaged only within a face. That is what makes edges crisp: sharing vertices
        // across a face boundary would average the normals across it and round off every
        // hard edge in the model. Costs duplicate vertices along seams; entirely worth it.
        TopTools_IndexedMapOfShape faceMap;
        TopExp::MapShapes(shape, TopAbs_FACE, faceMap);

        for (int fi = 1; fi <= faceMap.Extent(); ++fi) {
            const TopoDS_Face face = TopoDS::Face(faceMap(fi));
            TopLoc_Location loc;
            const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull() || tri->NbTriangles() == 0) continue;

            const auto name = output.map.nameOf(kernel::wrap(face));
            if (!name) {
                // An unnamed face is a naming-layer failure that already surfaced upstream;
                // drawing it would give the user something unselectable. Skip and let the
                // caller report the count.
                continue;
            }
            const std::uint32_t element = static_cast<std::uint32_t>(mesh->elements.size());
            mesh->elements.push_back(*name);

            const gp_Trsf trsf = loc.Transformation();
            const bool reversed = face.Orientation() == TopAbs_REVERSED;
            const std::uint32_t vertexBase = static_cast<std::uint32_t>(mesh->vertices.size());
            const int nbNodes = tri->NbNodes();

            for (int n = 1; n <= nbNodes; ++n) {
                gp_Pnt p = tri->Node(n);
                if (!loc.IsIdentity()) p.Transform(trsf);
                expand(mesh->bounds, p);
                CadVertex v{};
                v.position[0] = static_cast<float>(p.X());
                v.position[1] = static_cast<float>(p.Y());
                v.position[2] = static_cast<float>(p.Z());
                v.element = element;
                mesh->vertices.push_back(v);
            }

            const std::uint32_t indexBase = static_cast<std::uint32_t>(mesh->indices.size());
            std::vector<gp_Vec> accumulated(static_cast<std::size_t>(nbNodes), gp_Vec(0, 0, 0));

            for (int t = 1; t <= tri->NbTriangles(); ++t) {
                int a = 0, b = 0, c = 0;
                tri->Triangle(t).Get(a, b, c);
                if (reversed) std::swap(b, c);

                const auto& va = mesh->vertices[vertexBase + static_cast<std::uint32_t>(a - 1)];
                const auto& vb = mesh->vertices[vertexBase + static_cast<std::uint32_t>(b - 1)];
                const auto& vc = mesh->vertices[vertexBase + static_cast<std::uint32_t>(c - 1)];
                const gp_Vec e1(vb.position[0] - va.position[0], vb.position[1] - va.position[1],
                                vb.position[2] - va.position[2]);
                const gp_Vec e2(vc.position[0] - va.position[0], vc.position[1] - va.position[1],
                                vc.position[2] - va.position[2]);
                const gp_Vec n = e1.Crossed(e2);
                // Area-weighted: the un-normalised cross product is proportional to area,
                // which is the right weighting for a mesh with wildly varying triangle sizes
                // — and OCCT produces exactly that near curved seams.
                accumulated[static_cast<std::size_t>(a - 1)] += n;
                accumulated[static_cast<std::size_t>(b - 1)] += n;
                accumulated[static_cast<std::size_t>(c - 1)] += n;

                mesh->indices.push_back(vertexBase + static_cast<std::uint32_t>(a - 1));
                mesh->indices.push_back(vertexBase + static_cast<std::uint32_t>(b - 1));
                mesh->indices.push_back(vertexBase + static_cast<std::uint32_t>(c - 1));
            }

            for (int n = 0; n < nbNodes; ++n) {
                gp_Vec v = accumulated[static_cast<std::size_t>(n)];
                const double len = v.Magnitude();
                auto& vert = mesh->vertices[vertexBase + static_cast<std::uint32_t>(n)];
                if (len > 1e-12) {
                    v /= len;
                    vert.normal[0] = static_cast<float>(v.X());
                    vert.normal[1] = static_cast<float>(v.Y());
                    vert.normal[2] = static_cast<float>(v.Z());
                } else {
                    vert.normal[2] = 1.0f;   // degenerate triangle fan; harmless placeholder
                }
            }

            FaceRange range;
            range.indexOffset = indexBase;
            range.indexCount = static_cast<std::uint32_t>(mesh->indices.size()) - indexBase;
            range.element = element;
            mesh->faces.push_back(range);
        }

        // ── Edges ────────────────────────────────────────────────────────────────────────
        //
        // Sampled from the EXACT curve with GCPnts_TangentialDeflection, not extracted from
        // the triangle mesh. The mesh's edge polylines inherit the surface tessellation's
        // error, so a cylinder's silhouette comes out visibly faceted at the exact place a
        // user is looking. This is the difference between "a 3D view" and "a CAD viewport"
        // (ADR 0007 decision 5), and it is cheap: edges are 1D.
        TopTools_IndexedMapOfShape edgeMap;
        TopExp::MapShapes(shape, TopAbs_EDGE, edgeMap);

        for (int ei = 1; ei <= edgeMap.Extent(); ++ei) {
            const TopoDS_Edge edge = TopoDS::Edge(edgeMap(ei));
            if (BRep_Tool::Degenerated(edge)) continue;   // seam poles have no visible edge

            const auto name = output.map.nameOf(kernel::wrap(edge));
            if (!name) continue;

            BRepAdaptor_Curve curve(edge);
            GCPnts_TangentialDeflection sampler(curve, settings.angularDeflection,
                                                settings.deflection);
            if (sampler.NbPoints() < 2) continue;

            const std::uint32_t element = static_cast<std::uint32_t>(mesh->elements.size());
            mesh->elements.push_back(*name);

            EdgeRange range;
            range.vertexOffset = static_cast<std::uint32_t>(mesh->edgeVertices.size() / 3);
            range.element = element;
            for (int p = 1; p <= sampler.NbPoints(); ++p) {
                const gp_Pnt pt = sampler.Value(p);
                expand(mesh->bounds, pt);
                mesh->edgeVertices.push_back(static_cast<float>(pt.X()));
                mesh->edgeVertices.push_back(static_cast<float>(pt.Y()));
                mesh->edgeVertices.push_back(static_cast<float>(pt.Z()));
            }
            range.vertexCount = static_cast<std::uint32_t>(mesh->edgeVertices.size() / 3)
                                - range.vertexOffset;
            mesh->edges.push_back(range);
        }

        if (mesh->vertices.empty() && mesh->edgeVertices.empty()) {
            throw std::runtime_error("nothing tessellated");
        }

        // Key = source shape content hash, folded with the settings. Two shapes that hash
        // alike tessellate alike, which is exactly what makes dedupe across an assembly work.
        const kernel::ShapeHash source = naming::contentHash(output.shape, output.map);
        mesh->contentHash = source;
        mesh->contentHash.lanes[0] ^= settings.digest();
        return mesh;
    });

    if (!built) return built.error();
    return RenderMeshPtr(std::move(built).value());
}

}  // namespace cad::render
