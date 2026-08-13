// Implementation of the session half of cad_plugin_abi.h.
//
// Every exported function here is a firebreak: nothing may escape as an exception, no C++
// type may appear in a signature, and every handle is validated before use. The callers are
// other languages and other processes' idea of "undefined behaviour" is our crash report.

#include "cad/abi/cad_plugin_abi.h"

#include "cad/document/Document.h"
#include "cad/kernel/Primitives.h"
#include "cad/naming/ElementMap.h"
#include "cad/io/DocumentStore.h"
#include "cad/sketch/Dxf.h"
#include "cad/sketch/Infer.h"
#include "cad/sketch/Sketch.h"
#include "cad/io/Format.h"
#include "cad/recompute/DdcCache.h"
#include "cad/recompute/Engine.h"
#include "cad/render/Camera.h"
#include "cad/render/NullBackend.h"
#include "cad/render/Scene.h"
#include "cad/render/Tessellate.h"
#include "cad/units/Units.h"

#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

using cad::document::Document;
using cad::document::History;
using cad::document::ObjectData;
using cad::document::ObjectId;
using cad::document::ObjectState;
using cad::naming::ElementName;
using cad::recompute::Engine;
using cad::recompute::FeatureRegistry;
using cad::recompute::MemoryCache;

struct Session {
    FeatureRegistry registry = FeatureRegistry::builtins();
    cad::io::FormatRegistry formats = cad::io::FormatRegistry::builtins();
    std::unique_ptr<cad::recompute::Cache> cache;

    /// Mesh blobs go through a BlobStore, not the Output cache — a tessellated mesh is derived
    /// data but it is not a shape (see the note on BlobStore). When a disk cache is configured
    /// ONE DdcCache serves both roles, so meshes reach the shared tier exactly like cooked
    /// features do.
    ///
    /// NON-OWNING. The TieredCache owns it. An earlier draft held it in a second unique_ptr
    /// as well, which compiles perfectly and double-frees on session release.
    cad::recompute::DdcCache* ddc = nullptr;
    std::unique_ptr<cad::recompute::BlobStore> memoryBlobs;
    std::unique_ptr<cad::render::MeshCache> meshes;

    /// A NullBackend, always. The session is the headless surface: shells create their own
    /// real backend and drive the same SceneBuilder. This one exists so the scene layer is
    /// exercisable — and tested — with no graphics stack at all.
    cad::render::NullBackend backend;
    std::unique_ptr<cad::render::SceneBuilder> scene;
    cad::render::CameraController camera;
    std::vector<cad::render::Placement> placements;

    History history{Document{}};

    /// Sketches, by handle. std::map rather than a vector so a released handle does not shift the
    /// others -- handles are opaque to the caller and must stay valid until released.
    std::map<std::uint64_t, cad::sketch::Sketch> sketches;
    std::uint64_t nextSketch = 1;

    explicit Session(const std::string& cacheDir) {
        auto l0 = std::make_unique<MemoryCache>();
        if (cacheDir.empty()) {
            cache = std::move(l0);
            memoryBlobs = std::make_unique<cad::recompute::MemoryBlobStore>();
            meshes = std::make_unique<cad::render::MeshCache>(*memoryBlobs);
        } else {
            auto owned = std::make_unique<cad::recompute::DdcCache>(cacheDir);
            ddc = owned.get();
            cache = std::make_unique<cad::recompute::TieredCache>(std::move(l0),
                                                                 std::move(owned));
            meshes = std::make_unique<cad::render::MeshCache>(*ddc);
        }
        scene = std::make_unique<cad::render::SceneBuilder>(*meshes, backend.resources);
        cad::render::Viewport vp;
        vp.width = 1280;
        vp.height = 800;
        scene->setViewport(vp);
        viewport = vp;
    }

    cad::render::Viewport viewport;

    /// Returned strings live here. Valid until the next call on this session, exactly as
    /// the header promises — a single slot makes that promise impossible to accidentally
    /// break by holding two results at once.
    std::string scratch;
    std::string lastError;

    /// The mesh from the most recent tessellate call, so element slots can be read back
    /// without re-entering the cache per slot.
    cad::render::RenderMeshPtr lastMesh;
    CadObject lastMeshObject = 0;

    [[nodiscard]] const Document& doc() const { return history.current(); }
};

std::mutex g_mutex;
std::unordered_map<std::uint64_t, std::unique_ptr<Session>> g_sessions;
std::uint64_t g_nextSession = 1;

Session* lookup(CadSession handle) {
    const auto it = g_sessions.find(handle);
    return it == g_sessions.end() ? nullptr : it->second.get();
}

CadStatus toStatus(cad::kernel::ErrorCode c) {
    switch (c) {
        case cad::kernel::ErrorCode::Ok:              return CAD_OK;
        case cad::kernel::ErrorCode::InvalidInput:    return CAD_ERR_INVALID_INPUT;
        case cad::kernel::ErrorCode::NotDone:         return CAD_ERR_NOT_DONE;
        case cad::kernel::ErrorCode::InvalidResult:   return CAD_ERR_INVALID_RESULT;
        case cad::kernel::ErrorCode::BooleanFailed:   return CAD_ERR_BOOLEAN_FAILED;
        case cad::kernel::ErrorCode::Unsupported:     return CAD_ERR_UNSUPPORTED;
        case cad::kernel::ErrorCode::NamingLost:      return CAD_ERR_NAMING_LOST;
        case cad::kernel::ErrorCode::KernelException: return CAD_ERR_KERNEL_EXC;
        case cad::kernel::ErrorCode::Cancelled:       return CAD_ERR_CANCELLED;
        case cad::kernel::ErrorCode::Internal:        return CAD_ERR_INTERNAL;
    }
    return CAD_ERR_INTERNAL;
}

/// Wraps every export. Locks, validates the handle, and converts any escaping exception
/// into a status code plus a readable message.
template <class Fn>
CadStatus withSession(CadSession handle, Fn&& fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = lookup(handle);
    if (s == nullptr) return CAD_ERR_BAD_HANDLE;
    try {
        s->lastError.clear();
        return fn(*s);
    } catch (const std::exception& e) {
        s->lastError = e.what();
        return CAD_ERR_INTERNAL;
    } catch (...) {
        s->lastError = "unknown exception crossing the ABI";
        return CAD_ERR_INTERNAL;
    }
}

/// For the const char*-returning calls, which have no status channel.
template <class Fn>
const char* withSessionStr(CadSession handle, Fn&& fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = lookup(handle);
    if (s == nullptr) return "";
    try {
        s->scratch.clear();
        fn(*s);
        return s->scratch.c_str();
    } catch (const std::exception& e) {
        s->lastError = e.what();
        s->scratch.clear();
        return "";
    } catch (...) {
        s->scratch.clear();
        return "";
    }
}

CadStatus fail(Session& s, CadStatus code, std::string message) {
    s.lastError = std::move(message);
    return code;
}

/// Resolves a sketch handle, or reports a bad handle. Every sketch entry point goes through this:
/// a stale handle must be an error the caller sees, not a crash.
template <class Fn>
CadStatus withSketch(Session& s, CadSketch handle, Fn&& fn) {
    const auto it = s.sketches.find(handle);
    if (it == s.sketches.end()) {
        return fail(s, CAD_ERR_BAD_HANDLE, "That sketch no longer exists.");
    }
    return fn(it->second);
}

cad::sketch::Plane planeOf(std::int32_t raw) {
    switch (raw) {
        case CAD_SKETCH_PLANE_XZ: return cad::sketch::Plane::XZ;
        case CAD_SKETCH_PLANE_YZ: return cad::sketch::Plane::YZ;
        default:                  return cad::sketch::Plane::XY;
    }
}

CadSketch registerSketch(Session& s, cad::sketch::Sketch sketch) {
    const CadSketch handle = s.nextSketch++;
    s.sketches.emplace(handle, std::move(sketch));
    return handle;
}



/// Edits `id` in place through the persistent document, marks the subgraph dirty, and
/// commits a new history version.
template <class Fn>
CadStatus edit(Session& s, CadObject id, const char* label, Fn&& fn) {
    const ObjectId oid{id};
    const auto object = s.doc().find(oid);
    if (!object) return fail(s, CAD_ERR_BAD_HANDLE, "No such object.");

    ObjectData updated = *object;
    if (CadStatus st = fn(updated); st != CAD_OK) return st;

    Document next = s.doc().replace(std::make_shared<const ObjectData>(std::move(updated)));
    next = Engine::invalidate(next, oid);
    s.history.commit(std::move(next), label ? label : "Edit");
    return CAD_OK;
}

const cad::document::Output* outputOf(Session& s, CadObject id) {
    const auto object = s.doc().find(ObjectId{id});
    return object ? object->output() : nullptr;
}

}  // namespace

extern "C" {

static CadSession createSession(const std::string& cacheDir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        const std::uint64_t id = g_nextSession++;
        g_sessions[id] = std::make_unique<Session>(cacheDir);
        return id;
    } catch (...) {
        return 0;
    }
}

void cad_abi_version(std::uint32_t* major, std::uint32_t* minor) {
    if (major != nullptr) *major = CAD_ABI_VERSION_MAJOR;
    if (minor != nullptr) *minor = CAD_ABI_VERSION_MINOR;
}

CadSession cad_session_create(void) { return createSession({}); }

CadSession cad_session_create_cached(const char* dir) {
    return createSession(dir == nullptr ? std::string{} : std::string(dir));
}

void cad_session_release(CadSession handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sessions.erase(handle);   // erase of a missing key is a no-op: release(0) is fine
}

const char* cad_session_last_error(CadSession handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = lookup(handle);
    return s ? s->lastError.c_str() : "invalid session handle";
}

CadStatus cad_object_add(CadSession handle, const char* type, CadObject* out) {
    return withSession(handle, [&](Session& s) {
        if (type == nullptr || out == nullptr) {
            return fail(s, CAD_ERR_INVALID_INPUT, "Missing type or output pointer.");
        }
        if (s.registry.find(type) == nullptr) {
            return fail(s, CAD_ERR_UNSUPPORTED,
                        std::string("Unknown feature type '") + type + "'.");
        }
        auto [next, id] = s.doc().add(type);
        s.history.commit(std::move(next), std::string("Add ") + type);
        *out = id.value;
        return CAD_OK;
    });
}

CadStatus cad_object_remove(CadSession handle, CadObject id) {
    return withSession(handle, [&](Session& s) {
        const ObjectId oid{id};
        if (!s.doc().contains(oid)) return fail(s, CAD_ERR_BAD_HANDLE, "No such object.");
        // Dependents are invalidated, not silently repaired: they will report a missing
        // reference on the next recompute, which is what the user needs to see.
        Document next = s.doc();
        for (const ObjectId dep : next.dependents(oid)) next = Engine::invalidate(next, dep);
        next = next.remove(oid);
        s.history.commit(std::move(next), "Delete");
        return CAD_OK;
    });
}

#define CAD_SETTER(fnName, cType, convert, label)                                       \
    CadStatus fnName(CadSession handle, CadObject id, const char* prop, cType value) {  \
        return withSession(handle, [&](Session& s) {                                    \
            if (prop == nullptr) {                                                      \
                return fail(s, CAD_ERR_INVALID_INPUT, "Missing property name.");        \
            }                                                                           \
            return edit(s, id, label, [&](ObjectData& o) {                              \
                o = o.withProperty(prop, convert);                                      \
                return CAD_OK;                                                          \
            });                                                                         \
        });                                                                             \
    }

CAD_SETTER(cad_object_set_length, double, cad::units::millimetres(value), "Change dimension")
CAD_SETTER(cad_object_set_real, double, value, "Change value")
CAD_SETTER(cad_object_set_int, int64_t, static_cast<std::int64_t>(value), "Change value")
CAD_SETTER(cad_object_set_bool, int32_t, value != 0, "Change value")
#undef CAD_SETTER

CadStatus cad_object_set_text(CadSession handle, CadObject id, const char* prop,
                              const char* value) {
    return withSession(handle, [&](Session& s) {
        if (prop == nullptr || value == nullptr) {
            return fail(s, CAD_ERR_INVALID_INPUT, "Missing property name or value.");
        }
        return edit(s, id, "Change text", [&](ObjectData& o) {
            o = o.withProperty(prop, std::string(value));
            return CAD_OK;
        });
    });
}

CadStatus cad_object_set_object(CadSession handle, CadObject id, const char* prop,
                                CadObject target) {
    return withSession(handle, [&](Session& s) {
        if (prop == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing property name.");
        if (!s.doc().contains(ObjectId{target})) {
            return fail(s, CAD_ERR_BAD_HANDLE, "The referenced object does not exist.");
        }
        return edit(s, id, "Change input", [&](ObjectData& o) {
            o = o.withProperty(prop, ObjectId{target});
            return CAD_OK;
        });
    });
}

CadStatus cad_object_set_element(CadSession handle, CadObject id, const char* prop,
                                 const char* elementName) {
    return withSession(handle, [&](Session& s) {
        if (prop == nullptr || elementName == nullptr) {
            return fail(s, CAD_ERR_INVALID_INPUT, "Missing property name or element name.");
        }
        const ElementName parsed = ElementName::parse(elementName);
        if (parsed.isNull()) {
            return fail(s, CAD_ERR_INVALID_INPUT,
                        std::string("'") + elementName + "' is not a valid element name.");
        }
        return edit(s, id, "Change selection", [&](ObjectData& o) {
            o = o.withProperty(prop, parsed);
            return CAD_OK;
        });
    });
}

CadStatus cad_object_set_cosmetic(CadSession handle, CadObject id, const char* prop,
                                  int32_t cosmetic) {
    return withSession(handle, [&](Session& s) {
        if (prop == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing property name.");
        const auto object = s.doc().find(ObjectId{id});
        if (!object) return fail(s, CAD_ERR_BAD_HANDLE, "No such object.");
        const auto* existing = object->find(prop);
        if (existing == nullptr) {
            return fail(s, CAD_ERR_INVALID_INPUT, "No such property.");
        }
        return edit(s, id, "Change property flags", [&](ObjectData& o) {
            o = o.withProperty(prop, *o.find(prop), cosmetic != 0);
            return CAD_OK;
        });
    });
}

CadStatus cad_object_state(CadSession handle, CadObject id, int32_t* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        const auto object = s.doc().find(ObjectId{id});
        if (!object) return fail(s, CAD_ERR_BAD_HANDLE, "No such object.");
        *out = static_cast<int32_t>(object->state());
        return CAD_OK;
    });
}

const char* cad_object_error(CadSession handle, CadObject id) {
    return withSessionStr(handle, [&](Session& s) {
        if (const auto object = s.doc().find(ObjectId{id})) {
            s.scratch = object->error().message;
        }
    });
}

CadStatus cad_object_face_count(CadSession handle, CadObject id, uint64_t* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        const auto* output = outputOf(s, id);
        if (output == nullptr) return fail(s, CAD_ERR_NOT_DONE, "Object has no geometry yet.");
        *out = output->shape.subShapes(cad::kernel::ShapeType::Face).size();
        return CAD_OK;
    });
}

CadStatus cad_object_edge_count(CadSession handle, CadObject id, uint64_t* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        const auto* output = outputOf(s, id);
        if (output == nullptr) return fail(s, CAD_ERR_NOT_DONE, "Object has no geometry yet.");
        *out = output->shape.subShapes(cad::kernel::ShapeType::Edge).size();
        return CAD_OK;
    });
}

CadStatus cad_object_cache_key(CadSession handle, CadObject id, uint64_t* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        const auto object = s.doc().find(ObjectId{id});
        if (!object) return fail(s, CAD_ERR_BAD_HANDLE, "No such object.");
        *out = object->cacheKey();
        return CAD_OK;
    });
}

CadStatus cad_object_is_valid_shape(CadSession handle, CadObject id, int32_t* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        const auto* output = outputOf(s, id);
        if (output == nullptr) return fail(s, CAD_ERR_NOT_DONE, "Object has no geometry yet.");
        *out = output->shape.validate().ok() ? 1 : 0;
        return CAD_OK;
    });
}

CadStatus cad_object_volume(CadSession handle, CadObject id, double* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        const auto* output = outputOf(s, id);
        if (output == nullptr) return fail(s, CAD_ERR_NOT_DONE, "Object has no geometry yet.");
        *out = output->shape.volume();
        return CAD_OK;
    });
}

const char* cad_object_content_hash(CadSession handle, CadObject id) {
    return withSessionStr(handle, [&](Session& s) {
        if (const auto* output = outputOf(s, id)) {
            s.scratch = cad::naming::contentHash(output->shape, output->map).hex();
        }
    });
}

const char* cad_box_face_name(CadSession handle, CadObject id, int32_t face) {
    return withSessionStr(handle, [&](Session& s) {
        const auto object = s.doc().find(ObjectId{id});
        if (!object || object->type() != "Box") return;
        if (face < 0 || face >= static_cast<int32_t>(cad::kernel::BoxFaceCount)) return;
        const auto* output = object->output();
        if (output == nullptr) return;

        // Mirrors what the primitive namer assigned: the object id is the naming serial,
        // and the discriminator is the positional face tag.
        cad::naming::NameStep step;
        step.featureSerial = static_cast<std::uint32_t>(id);
        step.opTag = 0;
        step.provenance = cad::naming::Provenance::Primitive;
        step.discriminator = static_cast<std::uint32_t>(face);
        const ElementName name({step});
        if (output->map.resolve(name)) s.scratch = name.toString();
    });
}

const char* cad_box_edge_between(CadSession handle, CadObject id, int32_t a, int32_t b) {
    return withSessionStr(handle, [&](Session& s) {
        const auto object = s.doc().find(ObjectId{id});
        if (!object || object->type() != "Box") return;
        const auto* output = object->output();
        if (output == nullptr) return;
        if (a < 0 || b < 0 || a >= static_cast<int32_t>(cad::kernel::BoxFaceCount) ||
            b >= static_cast<int32_t>(cad::kernel::BoxFaceCount)) {
            return;
        }

        const auto faceName = [&](int32_t f) {
            cad::naming::NameStep step;
            step.featureSerial = static_cast<std::uint32_t>(id);
            step.opTag = 0;
            step.provenance = cad::naming::Provenance::Primitive;
            step.discriminator = static_cast<std::uint32_t>(f);
            return ElementName({step});
        };

        cad::naming::NameStep step;
        step.provenance = cad::naming::Provenance::Boundary;  // no feature serial, by design
        step.parents = {faceName(a).digest(), faceName(b).digest()};
        const ElementName edge({step});
        if (!output->map.resolveAll(edge).empty()) s.scratch = edge.toString();
    });
}

CadStatus cad_recompute(CadSession handle, CadRecomputeReport* out) {
    return withSession(handle, [&](Session& s) {
        Engine engine(s.registry, *s.cache);
        auto result = engine.recompute(s.doc());
        if (!result) {
            s.lastError = result.error().message;
            return toStatus(result.error().code);
        }
        // Recompute results are not an undoable edit: replace the current version in place
        // rather than pushing a history entry the user never asked for.
        s.history.replaceCurrent(std::move(result.value().first));
        if (out != nullptr) {
            const auto& r = result.value().second;
            out->computed = r.computed;
            out->cached = r.cached;
            out->skipped = r.skipped;
            out->failed = r.failed;
            out->blocked = r.blocked;
        }
        return CAD_OK;
    });
}

CadStatus cad_cache_stats(CadSession handle, uint64_t* hits, uint64_t* misses) {
    return withSession(handle, [&](Session& s) {
        if (hits != nullptr) *hits = s.cache->hits();
        if (misses != nullptr) *misses = s.cache->misses();
        return CAD_OK;
    });
}

CadStatus cad_cache_reset_stats(CadSession handle) {
    return withSession(handle, [&](Session& s) {
        s.cache->resetStats();
        return CAD_OK;
    });
}

CadStatus cad_undo(CadSession handle, int32_t* out) {
    return withSession(handle, [&](Session& s) {
        const bool did = s.history.undo();
        if (out != nullptr) *out = did ? 1 : 0;
        return CAD_OK;
    });
}

CadStatus cad_redo(CadSession handle, int32_t* out) {
    return withSession(handle, [&](Session& s) {
        const bool did = s.history.redo();
        if (out != nullptr) *out = did ? 1 : 0;
        return CAD_OK;
    });
}

CadStatus cad_document_digest(CadSession handle, uint64_t* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        *out = s.doc().digest();
        return CAD_OK;
    });
}

CadStatus cad_object_count(CadSession handle, uint64_t* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        *out = s.doc().size();
        return CAD_OK;
    });
}

CadStatus cad_sketch_create(CadSession handle, std::int32_t plane, CadSketch* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        *out = registerSketch(s, cad::sketch::Sketch(planeOf(plane)));
        return CAD_OK;
    });
}

void cad_sketch_release(CadSession handle, CadSketch sketch) {
    (void)withSession(handle, [&](Session& s) {
        s.sketches.erase(sketch);
        return CAD_OK;
    });
}

CadStatus cad_sketch_add_line(CadSession handle, CadSketch sk, double x1, double y1, double x2,
                              double y2, std::int32_t construction, std::uint32_t* out) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            const auto id = sketch.addLine(x1, y1, x2, y2, construction != 0);
            if (out != nullptr) *out = id;
            return CAD_OK;
        });
    });
}

CadStatus cad_sketch_add_circle(CadSession handle, CadSketch sk, double cx, double cy,
                                double radius, std::int32_t construction, std::uint32_t* out) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            const auto id = sketch.addCircle(cx, cy, radius, construction != 0);
            if (out != nullptr) *out = id;
            return CAD_OK;
        });
    });
}

CadStatus cad_sketch_add_arc(CadSession handle, CadSketch sk, double cx, double cy, double radius,
                             double startAngle, double endAngle, std::int32_t construction,
                             std::uint32_t* out) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            const auto id = sketch.addArc(cx, cy, radius, startAngle, endAngle, construction != 0);
            if (out != nullptr) *out = id;
            return CAD_OK;
        });
    });
}

CadStatus cad_sketch_constrain(CadSession handle, CadSketch sk, std::int32_t kind, std::uint32_t a,
                               std::int32_t aPoint, std::uint32_t b, std::int32_t bPoint,
                               double value, std::uint64_t* out) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            using PR = cad::sketch::PointRef;
            const auto ap = static_cast<PR>(aPoint);
            const auto bp = static_cast<PR>(bPoint);
            std::size_t index = 0;
            switch (kind) {
                case CAD_CON_COINCIDENT:    index = sketch.coincident(a, ap, b, bp); break;
                case CAD_CON_HORIZONTAL:    index = sketch.horizontal(a); break;
                case CAD_CON_VERTICAL:      index = sketch.vertical(a); break;
                case CAD_CON_PARALLEL:      index = sketch.parallel(a, b); break;
                case CAD_CON_PERPENDICULAR: index = sketch.perpendicular(a, b); break;
                case CAD_CON_DISTANCE:      index = sketch.distance(a, ap, b, bp, value); break;
                case CAD_CON_RADIUS:        index = sketch.radius(a, value); break;
                case CAD_CON_POINT_ON_LINE: index = sketch.pointOnLine(a, ap, b); break;
                case CAD_CON_EQUAL_LENGTH:  index = sketch.equalLength(a, b); break;
                case CAD_CON_LOCK_X:        index = sketch.lockX(a, ap, value); break;
                case CAD_CON_LOCK_Y:        index = sketch.lockY(a, ap, value); break;
                default:
                    return fail(s, CAD_ERR_UNSUPPORTED, "That constraint kind is not known.");
            }
            if (out != nullptr) *out = index;
            return CAD_OK;
        });
    });
}

CadStatus cad_sketch_solve(CadSession handle, CadSketch sk, CadSolveReport* out) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            const auto report = sketch.solve();
            if (out != nullptr) {
                out->solved = report.solved ? 1 : 0;
                out->dofs = report.dofs;
                out->conflicting = report.conflicting.size();
                out->redundant = report.redundant.size();
            }
            s.lastError = report.message;
            return CAD_OK;
        });
    });
}

CadStatus cad_sketch_geometry_count(CadSession handle, CadSketch sk, std::uint64_t* out) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
            *out = sketch.geometry().size();
            return CAD_OK;
        });
    });
}

CadStatus cad_sketch_geometry(CadSession handle, CadSketch sk, std::uint64_t index,
                              CadSketchGeo* out) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
            if (index >= sketch.geometry().size()) {
                return fail(s, CAD_ERR_INVALID_INPUT, "That geometry index is out of range.");
            }
            const auto& g = sketch.geometry()[index];
            out->kind = static_cast<std::int32_t>(g.kind);
            out->construction = g.construction ? 1 : 0;
            for (int i = 0; i < 5; ++i) out->p[i] = g.p[static_cast<std::size_t>(i)];
            return CAD_OK;
        });
    });
}

CadStatus cad_sketch_constraint_count(CadSession handle, CadSketch sk, std::uint64_t* out) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
            *out = sketch.constraints().size();
            return CAD_OK;
        });
    });
}

CadStr cad_sketch_serialize(CadSession handle, CadSketch sk) {
    CadStr result{nullptr, 0};
    (void)withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            s.scratch = sketch.serialize();
            result.data = s.scratch.c_str();
            result.len = s.scratch.size();
            return CAD_OK;
        });
    });
    return result;
}

CadStatus cad_sketch_deserialize(CadSession handle, const char* text, CadSketch* out) {
    return withSession(handle, [&](Session& s) {
        if (text == nullptr || out == nullptr) {
            return fail(s, CAD_ERR_INVALID_INPUT, "Missing text or output pointer.");
        }
        auto parsed = cad::sketch::Sketch::deserialize(text);
        if (!parsed) {
            s.lastError = parsed.error().message;
            return toStatus(parsed.error().code);
        }
        *out = registerSketch(s, std::move(parsed.value()));
        return CAD_OK;
    });
}

CadStatus cad_sketch_import_dxf(CadSession handle, const char* path, std::int32_t plane,
                                double scale, CadSketch* out) {
    return withSession(handle, [&](Session& s) {
        if (path == nullptr || out == nullptr) {
            return fail(s, CAD_ERR_INVALID_INPUT, "Missing path or output pointer.");
        }
        cad::io::DxfImportOptions options;
        options.plane = planeOf(plane);
        if (scale > 0.0) options.scale = scale;
        cad::io::DxfImportReport report;
        auto imported = cad::io::importDxf(path, options, &report);
        if (!imported) {
            s.lastError = imported.error().message;
            return toStatus(imported.error().code);
        }
        // The report goes into lastError so a caller can read the fidelity summary -- including
        // which entity types were NOT imported -- rather than only learning it succeeded.
        s.lastError = report.summary();
        *out = registerSketch(s, std::move(imported.value()));
        return CAD_OK;
    });
}

CadStatus cad_sketch_infer(CadSession handle, CadSketch sk, double pointTolerance,
                           double angleTolerance, std::int32_t parallelPerpendicular,
                           CadInferReport* out) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            cad::sketch::InferenceOptions options;
            if (pointTolerance > 0.0) options.pointTolerance = pointTolerance;
            if (angleTolerance > 0.0) options.angleToleranceDeg = angleTolerance;
            options.parallelPerpendicular = parallelPerpendicular != 0;
            const auto report = cad::sketch::infer(sketch, options);
            if (out != nullptr) {
                out->coincident = report.coincident;
                out->horizontal = report.horizontal;
                out->vertical = report.vertical;
                out->parallel = report.parallel;
                out->perpendicular = report.perpendicular;
                out->dofs_before = report.dofsBefore;
                out->dofs_after = report.dofsAfter;
                out->conflicting = report.conflicting;
            }
            s.lastError = report.summary();
            return CAD_OK;
        });
    });
}

CadStatus cad_document_save(CadSession handle, const char* path) {
    return withSession(handle, [&](Session& s) {
        if (path == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing path.");
        auto r = cad::io::saveDocument(s.doc(), path);
        if (!r) {
            s.lastError = r.error().message;
            return toStatus(r.error().code);
        }
        return CAD_OK;
    });
}

CadStatus cad_document_open(CadSession handle, const char* path) {
    return withSession(handle, [&](Session& s) {
        if (path == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing path.");
        auto loaded = cad::io::loadDocument(path);
        if (!loaded) {
            s.lastError = loaded.error().message;
            return toStatus(loaded.error().code);
        }

        // Recompute BEFORE the document is installed. A file whose features cannot rebuild — a
        // missing referenced file, geometry a newer kernel produced differently — must not replace
        // the session's document with a broken one; the caller still has what it had.
        //
        // Note this does NOT reject a document with individually failed features. The engine
        // supports partial failure, so a part with one broken fillet opens with that fillet marked
        // and everything else intact, which is the behaviour a user needs when a file arrives
        // slightly wrong.
        Engine engine(s.registry, *s.cache);
        auto computed = engine.recompute(loaded.value());
        if (!computed) {
            s.lastError = computed.error().message;
            return toStatus(computed.error().code);
        }

        // A fresh History, not a commit: opening a file is not an edit, and being able to undo
        // past an open back into the previous document would be nonsense.
        s.history = History{std::move(computed.value().first)};
        s.placements.clear();
        return CAD_OK;
    });
}

CadStatus cad_object_export(CadSession handle, CadObject id, const char* path) {
    return withSession(handle, [&](Session& s) {
        if (path == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing path.");
        const auto* output = outputOf(s, id);
        if (output == nullptr) {
            return fail(s, CAD_ERR_NOT_DONE,
                        "This object has no geometry yet. Recompute first.");
        }
        auto r = cad::io::exportFile(s.formats, path, output->shape);
        if (!r) {
            s.lastError = r.error().message;
            return toStatus(r.error().code);
        }
        return CAD_OK;
    });
}

CadStatus cad_import_probe(CadSession handle, const char* path, int32_t assumed,
                           CadImportInfo* out) {
    return withSession(handle, [&](Session& s) {
        if (path == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing path.");
        if (assumed < 0 || assumed > 4) {
            return fail(s, CAD_ERR_INVALID_INPUT, "Unknown unit system.");
        }
        cad::io::ImportOptions options;
        options.assumedUnits = static_cast<cad::units::UnitSystem>(assumed);

        auto result = cad::io::importFile(s.formats, path, options);
        if (!result) {
            s.lastError = result.error().message;
            return toStatus(result.error().code);
        }
        const auto& report = result.value().report;
        s.scratch = report.summary();
        if (out != nullptr) {
            out->solids = report.solids;
            out->faces = report.faces;
            out->units_were_assumed = report.unitsWereAssumed ? 1 : 0;
            out->unsupported_count = static_cast<int32_t>(report.unsupported.size());
            out->warning_count = static_cast<int32_t>(report.warnings.size());
        }
        return CAD_OK;
    });
}

const char* cad_import_summary(CadSession handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = lookup(handle);
    // Deliberately does NOT clear the scratch: this reads what cad_import_probe just left.
    return s ? s->scratch.c_str() : "";
}

static const char* joinExtensions(CadSession handle, bool writable) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = lookup(handle);
    if (s == nullptr) return "";
    s->scratch.clear();
    const auto list = writable ? s->formats.writableExtensions()
                               : s->formats.readableExtensions();
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i) s->scratch += ",";
        s->scratch += list[i];
    }
    return s->scratch.c_str();
}

const char* cad_readable_extensions(CadSession h) { return joinExtensions(h, false); }
const char* cad_writable_extensions(CadSession h) { return joinExtensions(h, true); }

CadStatus cad_object_tessellate(CadSession handle, CadObject id, double deflection,
                                double angular, CadMeshInfo* out) {
    return withSession(handle, [&](Session& s) {
        const auto* output = outputOf(s, id);
        if (output == nullptr) {
            return fail(s, CAD_ERR_NOT_DONE,
                        "This object has no geometry yet. Recompute first.");
        }
        cad::render::TessellationSettings settings;
        if (deflection > 0.0) settings.deflection = deflection;
        if (angular > 0.0) settings.angularDeflection = angular;

        auto mesh = s.meshes->get(*output, settings);
        if (!mesh) {
            s.lastError = mesh.error().message;
            return toStatus(mesh.error().code);
        }
        s.lastMesh = mesh.value();
        s.lastMeshObject = id;

        if (out != nullptr) {
            const auto& m = *mesh.value();
            out->triangles = m.triangleCount();
            out->vertices = m.vertices.size();
            out->edgePolylines = m.edges.size();
            out->edgePoints = m.edgeVertices.size() / 3;
            out->elements = m.elements.size();
            for (int i = 0; i < 3; ++i) {
                out->boundsMin[i] = m.bounds.min[i];
                out->boundsMax[i] = m.bounds.max[i];
            }
        }
        return CAD_OK;
    });
}

const char* cad_mesh_element_name(CadSession handle, CadObject id, uint32_t slot) {
    return withSessionStr(handle, [&](Session& s) {
        // Reads the mesh from the last tessellate call on this session rather than
        // re-tessellating: the caller is iterating slots, and re-entering the cache per slot
        // would make an O(n) loop look like an O(n) cache workload in the stats.
        if (!s.lastMesh || s.lastMeshObject != id) return;
        if (slot >= s.lastMesh->elements.size()) return;
        s.scratch = s.lastMesh->elements[slot].toString();
    });
}

CadStatus cad_mesh_cache_stats(CadSession handle, uint64_t* hits, uint64_t* misses) {
    return withSession(handle, [&](Session& s) {
        if (hits != nullptr) *hits = s.meshes->hits();
        if (misses != nullptr) *misses = s.meshes->misses();
        return CAD_OK;
    });
}

CadStatus cad_mesh_cache_reset_stats(CadSession handle) {
    return withSession(handle, [&](Session& s) {
        s.meshes->resetStats();
        return CAD_OK;
    });
}

// ── scene ───────────────────────────────────────────────────────────────────────────────

CadStatus cad_scene_add_placement(CadSession handle, CadObject id, const float* transform12) {
    return withSession(handle, [&](Session& s) {
        if (!s.doc().contains(ObjectId{id})) {
            return fail(s, CAD_ERR_BAD_HANDLE, "No such object.");
        }
        cad::render::Placement p;
        p.object = ObjectId{id};
        if (transform12 != nullptr) std::copy(transform12, transform12 + 12, p.transform);
        s.placements.push_back(p);
        return CAD_OK;
    });
}

CadStatus cad_scene_clear_placements(CadSession handle) {
    return withSession(handle, [&](Session& s) {
        s.placements.clear();
        return CAD_OK;
    });
}

CadStatus cad_scene_update(CadSession handle, double deflection, double angular) {
    return withSession(handle, [&](Session& s) {
        cad::render::TessellationSettings settings;
        if (deflection > 0.0) settings.deflection = deflection;
        if (angular > 0.0) settings.angularDeflection = angular;

        auto r = s.scene->update(s.doc(), s.placements, settings);
        if (!r) {
            s.lastError = r.error().message;
            return toStatus(r.error().code);
        }
        s.scene->setCamera(s.camera.matrices(s.viewport));
        return CAD_OK;
    });
}

CadStatus cad_scene_submit(CadSession handle) {
    return withSession(handle, [&](Session& s) {
        s.scene->setCamera(s.camera.matrices(s.viewport));
        s.backend.frames.submit(s.scene->frame());
        return CAD_OK;
    });
}

CadStatus cad_scene_stats(CadSession handle, CadSceneStats* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        const auto& st = s.scene->stats();
        const auto frame = s.backend.frames.lastFrameStats();
        const auto& rec = s.backend.frames.recorded();
        out->rebuilds = st.rebuilds;
        out->uploads = st.uploads;
        out->gpu_uploads = s.backend.resources.uploadCount();
        out->gpu_deduped = s.backend.resources.dedupedCount();
        out->unique_meshes = st.uniqueMeshes;
        out->instances = st.instances;
        out->element_slots = st.elementSlots;
        out->draw_calls = frame.drawCalls;
        out->frame_instances = frame.instances;
        out->frame_triangles = frame.triangles;
        out->frames = s.backend.frames.frameCount();
        out->highlighted = rec.highlighted;
        out->orthographic = rec.orthographic ? 1 : 0;
        return CAD_OK;
    });
}

CadStatus cad_scene_reset_stats(CadSession handle) {
    return withSession(handle, [&](Session& s) {
        s.scene->resetStats();
        s.backend.resources.resetStats();
        return CAD_OK;
    });
}

CadStatus cad_camera_orbit(CadSession handle, float dx, float dy) {
    return withSession(handle, [&](Session& s) { s.camera.orbit(dx, dy); return CAD_OK; });
}

CadStatus cad_camera_pan(CadSession handle, float dx, float dy) {
    return withSession(handle, [&](Session& s) {
        s.camera.pan(dx, dy, s.viewport);
        return CAD_OK;
    });
}

CadStatus cad_camera_zoom(CadSession handle, float ticks) {
    return withSession(handle, [&](Session& s) { s.camera.zoom(ticks); return CAD_OK; });
}

CadStatus cad_camera_fit(CadSession handle) {
    return withSession(handle, [&](Session& s) {
        s.camera.fit(s.scene->bounds(), s.viewport);
        return CAD_OK;
    });
}

CadStatus cad_camera_set_orthographic(CadSession handle, int32_t ortho) {
    return withSession(handle, [&](Session& s) {
        s.camera.setOrthographic(ortho != 0);
        return CAD_OK;
    });
}

CadStatus cad_camera_set_viewport(CadSession handle, uint32_t w, uint32_t h) {
    return withSession(handle, [&](Session& s) {
        s.viewport.width = w;
        s.viewport.height = h;
        s.scene->setViewport(s.viewport);
        s.backend.frames.resize(s.viewport);
        return CAD_OK;
    });
}

CadStatus cad_camera_distance(CadSession handle, float* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        *out = s.camera.distance();
        return CAD_OK;
    });
}

CadStatus cad_camera_set_preset(CadSession handle, int32_t preset) {
    return withSession(handle, [&](Session& s) {
        if (preset < 0 || preset > 2) {
            return fail(s, CAD_ERR_INVALID_INPUT, "Unknown navigation preset.");
        }
        s.camera.setPreset(static_cast<cad::render::NavigationPreset>(preset));
        return CAD_OK;
    });
}

CadStatus cad_camera_drag_for(CadSession handle, int32_t button, int32_t shift, int32_t ctrl,
                              int32_t* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        *out = static_cast<int32_t>(s.camera.dragFor(button, shift != 0, ctrl != 0));
        return CAD_OK;
    });
}

CadStatus cad_scene_set_highlight(CadSession handle, const char* name, int32_t highlight) {
    return withSession(handle, [&](Session& s) {
        if (name == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing element name.");
        if (highlight < 0 || highlight > 3) {
            return fail(s, CAD_ERR_INVALID_INPUT, "Unknown highlight kind.");
        }
        const ElementName parsed = ElementName::parse(name);
        if (parsed.isNull()) {
            return fail(s, CAD_ERR_INVALID_INPUT, "That is not a valid element name.");
        }
        s.scene->setHighlight(parsed, static_cast<cad::render::Highlight>(highlight));
        return CAD_OK;
    });
}

CadStatus cad_scene_clear_highlights(CadSession handle) {
    return withSession(handle, [&](Session& s) {
        s.scene->clearHighlights();
        return CAD_OK;
    });
}

CadStatus cad_scene_set_next_hit(CadSession handle, uint32_t instance, uint32_t element,
                                 int32_t valid) {
    return withSession(handle, [&](Session& s) {
        cad::render::IPicker::Hit hit;
        hit.instance = instance;
        hit.element = element;
        hit.valid = valid != 0;
        s.backend.picker.setNextHit(hit);
        return CAD_OK;
    });
}

const char* cad_scene_pick(CadSession handle, uint32_t x, uint32_t y) {
    return withSessionStr(handle, [&](Session& s) {
        const auto hit = s.backend.picker.pick(s.scene->frame(), x, y);
        if (const auto name = s.scene->resolve(hit)) s.scratch = name->toString();
    });
}

CadStatus cad_scene_pick_owner(CadSession handle, uint32_t x, uint32_t y, CadObject* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        const auto hit = s.backend.picker.pick(s.scene->frame(), x, y);
        const auto owner = s.scene->objectOf(hit.element);
        if (!owner) return fail(s, CAD_ERR_NOT_DONE, "Nothing under the pointer.");
        *out = owner->value;
        return CAD_OK;
    });
}

CadStatus cad_parse_length(const char* text, int32_t system, double* out) {
    if (text == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    if (system < 0 || system > 4) return CAD_ERR_INVALID_INPUT;
    try {
        auto parsed = cad::units::parseLength(text, static_cast<cad::units::UnitSystem>(system));
        if (!parsed) return toStatus(parsed.error().code);
        *out = cad::units::toMillimetres(parsed.value());
        return CAD_OK;
    } catch (...) {
        return CAD_ERR_INTERNAL;
    }
}

}  // extern "C"
