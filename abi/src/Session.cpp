// Implementation of the session half of cad_plugin_abi.h.
//
// Every exported function here is a firebreak: nothing may escape as an exception, no C++
// type may appear in a signature, and every handle is validated before use. The callers are
// other languages and other processes' idea of "undefined behaviour" is our crash report.

#include "cad/abi/cad_plugin_abi.h"

#include "cad/document/Document.h"
#include "cad/kernel/Primitives.h"
#include "cad/naming/ElementMap.h"
#include "cad/io/Format.h"
#include "cad/recompute/DdcCache.h"
#include "cad/recompute/Engine.h"
#include "cad/render/Tessellate.h"
#include "cad/units/Units.h"

#include <exception>
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

    History history{Document{}};

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
    }

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
