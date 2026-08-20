// Implementation of the session half of cad_plugin_abi.h.
//
// Every exported function here is a firebreak: nothing may escape as an exception, no C++
// type may appear in a signature, and every handle is validated before use. The callers are
// other languages and other processes' idea of "undefined behaviour" is our crash report.

#include "Loader.h"
#include "cad/abi/cad_plugin_abi.h"

#include <bit>
#include <cstddef>
#include <cstdlib>

#include "cad/log/Log.h"

#include "cad/document/Document.h"
#include "cad/kernel/Booleans.h"
#include "cad/kernel/Primitives.h"
#include "cad/kernel/Shape.h"
#include "cad/naming/ElementMap.h"
#include "cad/io/DocumentStore.h"
#include "cad/sketch/Dxf.h"
#include "cad/sketch/Infer.h"
#include "cad/sketch/Sketch.h"
#include "cad/io/Format.h"
#include "cad/recompute/DdcCache.h"
#include "cad/features/Builtins.h"
#include "cad/recompute/Engine.h"
#include "cad/render/Camera.h"
#include "cad/render/NullBackend.h"
#include "cad/render/Scene.h"
#include "cad/render/Tessellate.h"
#include "cad/units/Units.h"

#include <exception>
#include <map>
#include <optional>
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
    FeatureRegistry registry = cad::features::builtins();

    /// Shapes handed out across the C boundary, by handle.
    ///
    /// Handles, never pointers, and this map is why: the host can VALIDATE one. A plugin that
    /// keeps a shape past its release gets CAD_ERR_BAD_HANDLE, which is a bug report; a plugin
    /// that keeps a pointer past its free gets a crash somewhere else entirely, which is a
    /// support ticket nobody can close. PLUGIN_CONTRACT.md 3.1 promises the former.
    ///
    /// Ids are never reused. Reuse would make a stale handle silently address a DIFFERENT shape,
    /// which is worse than either alternative: no error, wrong geometry.
    /// A shape and, when it has one, the element map that names its faces and edges. The map
    /// travels WITH the shape because a plugin holding a face reference across a rebuild is the
    /// entire point of the naming system, and a shape handed over without its names can only be
    /// referenced positionally -- which is precisely FreeCAD's topological naming failure.
    struct StoredShape {
        cad::kernel::Shape shape;
        cad::naming::ElementMap names;
    };
    std::unordered_map<std::uint64_t, StoredShape> shapes;
    std::uint64_t nextShape = 1;

    /// Computes currently on the stack. A CadComputeCtx indexes this rather than pointing at a
    /// ComputeContext, so a plugin that stores one and uses it later gets a clean rejection
    /// instead of a dangling reference into a frame that has returned.
    struct ActiveCompute {
        const cad::recompute::ComputeContext* ctx = nullptr;

        /// The object whose parameters are readable. Set from `ctx->object` during a compute, and
        /// set ALONE during external_inputs -- which runs at cache-key time, when no input has
        /// been computed and there is no ComputeContext to point at. Splitting it out is what lets
        /// one set of parameter accessors serve both moments.
        const cad::document::ObjectData* object = nullptr;
        CadShape output = 0;
        bool hasOutput = false;
        std::string failMessage;
        std::string failDetail;
        /// Incremented per host-built shape, and passed to NamingContext as the op tag.
        ///
        /// Without it every shape a compute builds would be named with the same (serial, 0) pair
        /// and collide. Deterministic because 4.1 already requires the plugin's calls to be — an
        /// order-dependent counter is only a problem for a compute that is already broken.
        std::uint16_t opTag = 0;
    };
    /// The ribbon a plugin has contributed to, in Fusion's vocabulary: tabs contain sections,
    /// sections contain commands. Held as data rather than as Qt widgets because `app/` and `abi/`
    /// must not know what a ribbon looks like — the shell reads this and draws it, and the iPad
    /// shell will read the same thing and draw something else.
    struct RibbonTab {
        std::string id, label;
        std::uint32_t order = 0;
        bool builtin = false;
    };
    struct RibbonSection {
        std::string id, label, tab;
        std::uint32_t order = 0;
        bool builtin = false;
    };
    struct RibbonCommand {
        std::string id, label, tooltip, icon, tab, section;
        std::uint32_t order = 0, placement = 0;
        void* pluginCtx = nullptr;
        std::int32_t (*enabled)(void*, CadCommandCtx) = nullptr;
        CadStatus (*invoke)(void*, CadCommandCtx) = nullptr;
    };
    std::vector<RibbonTab> tabs;
    std::vector<RibbonSection> sections;
    std::vector<RibbonCommand> ribbonCommands;

    /// Settings a plugin declared. Owned as std::string rather than as the plugin's pointers,
    /// because a plugin may be unloaded while its page is still on screen — and a settings window
    /// showing freed strings is a crash in the host that a user will blame on the host.
    struct StoredSetting {
        std::string id, label, description, defaultText;
        std::vector<std::string> choices;
        std::uint32_t kind = 0;
        double defaultValue = 0.0, minimum = 0.0, maximum = 0.0;
    };
    struct StoredSettingsPage {
        std::string id, label, iconName, groupLabel;
        std::vector<StoredSetting> settings;
    };
    std::vector<StoredSettingsPage> settingsPages;

    /// Scratch for the const char* a read-back hands out, so they stay valid until the next call.
    std::vector<const char*> choicePointers;

    /// Seeds the built-in ids so a plugin can name CAD_SECTION_CREATE and be validated against
    /// something. They are part of the ABI (see the header) rather than the shell's private
    /// business, which is why they are known here at all.
    void seedBuiltinRibbon() {
        if (!tabs.empty()) return;
        tabs.push_back({CAD_TAB_MODEL, "3D Model", 0, true});
        tabs.push_back({CAD_TAB_SKETCH, "Sketch", 1, true});
        tabs.push_back({CAD_TAB_INSPECT, "Inspect", 2, true});
        tabs.push_back({CAD_TAB_ANNOTATE, "Annotate", 3, true});
        tabs.push_back({CAD_TAB_MANAGE, "Manage", 4, true});
        tabs.push_back({CAD_TAB_VIEW, "View", 5, true});

        sections.push_back({CAD_SECTION_SKETCH, "Sketch", CAD_TAB_MODEL, 0, true});
        sections.push_back({CAD_SECTION_CREATE, "Create", CAD_TAB_MODEL, 1, true});
        sections.push_back({CAD_SECTION_PRIMITIVES, "Primitives", CAD_TAB_MODEL, 2, true});
        sections.push_back({CAD_SECTION_MODIFY, "Modify", CAD_TAB_MODEL, 3, true});
        sections.push_back({CAD_SECTION_PATTERN, "Pattern", CAD_TAB_MODEL, 4, true});
        sections.push_back({CAD_SECTION_EDIT, "Edit", CAD_TAB_MODEL, 5, true});
        sections.push_back({CAD_SECTION_HISTORY, "History", CAD_TAB_MODEL, 6, true});
    }

    [[nodiscard]] bool hasTab(const std::string& id) const {
        return std::any_of(tabs.begin(), tabs.end(),
                           [&](const RibbonTab& t) { return t.id == id; });
    }
    [[nodiscard]] bool hasSection(const std::string& id) const {
        return std::any_of(sections.begin(), sections.end(),
                           [&](const RibbonSection& x) { return x.id == id; });
    }

    std::unordered_map<std::uint64_t, ActiveCompute> computes;
    std::uint64_t nextCompute = 1;

    /// The compute currently on the stack, innermost last. A stack rather than a single slot
    /// because 4.6 forbidding re-entrancy is a rule for PLUGINS; the host should not corrupt its
    /// own bookkeeping when one breaks it.
    std::vector<std::uint64_t> computeStack;

    /// Feature descriptors registered by plugins, kept alive for the session.
    ///
    /// The strings are COPIED. A plugin's descriptor is usually a static, but nothing in the
    /// contract says so, and a registration that stored borrowed pointers would fail only for
    /// plugins that build their descriptor on the stack — the worst kind of bug to find in
    /// third-party code.
    struct RegisteredFeature {
        std::string type;
        void* pluginCtx = nullptr;
        CadStatus (*compute)(void*, CadComputeCtx) = nullptr;
        CadStatus (*externalInputs)(void*, CadFeatureCtx, CadPathSink, void*) = nullptr;
        std::uint32_t computeVersion = 1;
    };
    std::vector<std::unique_ptr<RegisteredFeature>> features;

    /// The vtable handed to plugins. Built once per session, on demand.
    /// Every plugin this session loaded, held for the life of the session.
    ///
    /// Held, not discarded, because a loaded plugin owns the memory its descriptors point at --
    /// its feature names, its ribbon labels, its settings. Dropping the LoadedPlugin would not
    /// unload the library (nothing ever does, see Loader.h) but it would drop the manifest, and
    /// the manager window needs it to say what is running.
    std::vector<cad::abi::LoadedPlugin> loadedPlugins;

    std::unique_ptr<CadHost> host;
    std::string hostError;
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
    std::mutex mutex;

    /// The mesh from the most recent tessellate call, so element slots can be read back
    /// without re-entering the cache per slot.
    cad::render::RenderMeshPtr lastMesh;
    CadObject lastMeshObject = 0;

    [[nodiscard]] const Document& doc() const { return history.current(); }
};

std::mutex g_mutex;
/// shared_ptr, not unique_ptr, and that is load-bearing for the per-session locking below.
///
/// A caller takes a COPY of the shared_ptr under g_mutex, then locks that session. Between those
/// two steps another thread may release the same handle -- with unique_ptr the Session would be
/// destroyed and the very next line would lock a dangling mutex. The shared_ptr keeps it alive
/// until the in-flight call finishes, so release() means "no new calls can find it" rather than
/// "destroy it now, whoever is inside".
std::unordered_map<std::uint64_t, std::shared_ptr<Session>> g_sessions;
std::uint64_t g_nextSession = 1;

/// Call with g_mutex held. Returns a shared owner, so the session cannot be destroyed while the
/// caller is using it.
std::shared_ptr<Session> lookup(CadSession handle) {
    const auto it = g_sessions.find(handle);
    return it == g_sessions.end() ? nullptr : it->second;
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
    // Shared owner taken under g_mutex, so a concurrent cad_session_release cannot destroy this
    // session out from under the lock we are about to take.
    std::shared_ptr<Session> owner;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        owner = lookup(handle);
    }
    if (!owner) return CAD_ERR_BAD_HANDLE;
    Session* s = owner.get();
    std::lock_guard<std::mutex> lock(s->mutex);
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
    // Shared owner taken under g_mutex, so a concurrent cad_session_release cannot destroy this
    // session out from under the lock we are about to take.
    std::shared_ptr<Session> owner;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        owner = lookup(handle);
    }
    if (!owner) return "";
    Session* s = owner.get();
    std::lock_guard<std::mutex> lock(s->mutex);
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
        g_sessions[id] = std::make_shared<Session>(cacheDir);
        return id;
    } catch (...) {
        return 0;
    }
}

void cad_abi_version(std::uint32_t* major, std::uint32_t* minor) {
    if (major != nullptr) *major = CAD_ABI_VERSION_MAJOR;
    if (minor != nullptr) *minor = CAD_ABI_VERSION_MINOR;
}

// ── the host vtable ─────────────────────────────────────────────────────────────────────
//
// The host side of the plugin boundary, implemented IN TERMS OF the Session API wherever it can
// be (ADR 0011, enforcement point 4) so the two cannot drift apart.
//
// Entries this configuration does not offer are left NULL, which the contract requires plugins to
// check for (PLUGIN_CONTRACT.md 4.5). That is deliberate rather than unfinished: a sandboxed tier
// will offer strictly fewer, so plugins must handle NULL from the first day there is anything to
// handle. Leaving them NULL now means the discipline is exercised immediately instead of being
// discovered later.
// C++ LINKAGE, restored explicitly.
//
// This anonymous namespace is nested inside the `extern "C" {` above. Anonymous-ness makes these
// helpers internal but does NOT undo the linkage specification, so every function here had C
// linkage -- and a C-linkage function may not return a C++ class type. Clang says so as a warning
// and carries on; MSVC treats it as an error, decides the function returns void, and then reports
// three more errors about returning a value from void and destructuring void.
//
// Found by the first Windows build that ever ran. The warning had been visible on macOS for a
// while and read as pedantry.
extern "C++" {
namespace {

Session* hostSession(void* ctx) { return static_cast<Session*>(ctx); }

void hostLog(void* ctx, std::int32_t level, const char* msg) {
    if (msg == nullptr) return;
    const auto category = cad::log::Category::Plugin;
    switch (level) {
        case 0: CAD_LOG(category, cad::log::Level::Debug) << msg; break;
        case 1: CAD_LOG(category, cad::log::Level::Info) << msg; break;
        case 2: CAD_LOG(category, cad::log::Level::Warning) << msg; break;
        default: CAD_LOG(category, cad::log::Level::Error) << msg; break;
    }
    (void)ctx;
}

CadStr hostLastError(void* ctx) {
    Session* s = hostSession(ctx);
    if (s == nullptr) return CadStr{"", 0};
    return CadStr{s->hostError.c_str(), s->hostError.size()};
}

/// Interns a shape and returns its handle. 0 is reserved for "none", so ids start at 1.
CadShape intern(Session& s, cad::kernel::Shape shape, cad::naming::ElementMap names = {}) {
    const std::uint64_t id = s.nextShape++;
    s.shapes.emplace(id, Session::StoredShape{std::move(shape), std::move(names)});
    return id;
}

Session::StoredShape* lookupStored(Session& s, CadShape handle) {
    const auto it = s.shapes.find(handle);
    return it == s.shapes.end() ? nullptr : &it->second;
}

cad::kernel::Shape* lookup(Session& s, CadShape handle) {
    Session::StoredShape* stored = lookupStored(s, handle);
    return stored == nullptr ? nullptr : &stored->shape;
}

/// A naming serial that depends only on WHAT is being built — never on session state.
///
/// The serial identifies the thing a name belongs to, and `NamingContext` puts it into every name
/// it mints. Built-in features pass their ObjectId, which is stable for the life of the feature,
/// so recomputing one produces the same names it produced last time.
///
/// Host calls used to pass `Session::nextShape`, a counter incremented on every intern. Two
/// identical `make_box` calls in one session therefore produced DIFFERENT names, and the same call
/// in a fresh session produced different names again depending on what had been interned first.
/// That collides head-on with PLUGIN_CONTRACT.md 4.1: recompute is content-addressed, so a
/// plugin feature whose output names vary by session state lets the DDC serve a cached result
/// whose names disagree with what recomputing would produce — the document silently disagreeing
/// with itself, across undo, across reload, and across machines sharing a cache.
///
/// Hashing the REQUEST rather than counting calls fixes it, and gives identical geometry identical
/// names, which is the same content-addressing the rest of the system already runs on. These are
/// intermediates handed to a plugin, not document features; when a plugin's output lands in a
/// document it is renamed with that feature's serial (PICKUP.md step 3b).
std::uint32_t serialFor(const char* op, std::initializer_list<double> values) {
    std::uint64_t h = 1469598103934665603ULL;   // FNV-1a, as everywhere else in this codebase
    const auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 1099511628211ULL;
        }
    };
    for (const char* c = op; *c != '\0'; ++c) mix(static_cast<std::uint64_t>(*c));
    for (const double v : values) mix(std::bit_cast<std::uint64_t>(v));
    // Folded to 32 bits because that is what NamingContext takes. Never zero: 0 is what an
    // uninitialised serial looks like, and a collision with it would be invisible.
    const auto folded = static_cast<std::uint32_t>((h >> 32) ^ (h & 0xFFFFFFFFULL));
    return folded == 0 ? 1u : folded;
}

/// The same, for an operation over shapes that already carry names. Named differently rather than
/// overloaded: both take an initializer_list, and a braced list at the call site cannot
/// disambiguate them.
std::uint32_t serialForShapes(const char* op,
                              std::initializer_list<const Session::StoredShape*> inputs) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 1099511628211ULL;
        }
    };
    for (const char* c = op; *c != '\0'; ++c) mix(static_cast<std::uint64_t>(*c));
    for (const Session::StoredShape* in : inputs) {
        if (in == nullptr) continue;
        mix(cad::naming::contentHash(in->shape, in->names).fold64());
    }
    const auto folded = static_cast<std::uint32_t>((h >> 32) ^ (h & 0xFFFFFFFFULL));
    return folded == 0 ? 1u : folded;
}

Session::ActiveCompute* activeCompute(Session& s, CadComputeCtx cc) {
    const auto it = s.computes.find(cc);
    return it == s.computes.end() ? nullptr : &it->second;
}

/// The compute a host geometry call is running inside, or null at top level.
///
/// This is what lets a shape built during a compute be named with its FEATURE's serial rather
/// than a hash of the request. The feature serial derives from the object id, so it is stable
/// across rebuilds and distinct between two features that happen to build identical geometry —
/// which a request hash alone cannot be.
Session::ActiveCompute* innermostCompute(Session& s) {
    if (s.computeStack.empty()) return nullptr;
    return activeCompute(s, s.computeStack.back());
}

/// The (serial, opTag) a host-built shape should be named with.
std::pair<std::uint32_t, std::uint16_t> namingIdentityFor(Session& s, const char* op,
                                                          std::initializer_list<double> args) {
    if (Session::ActiveCompute* active = innermostCompute(s); active != nullptr &&
                                                              active->ctx != nullptr) {
        return {active->ctx->namingSerial, active->opTag++};
    }
    return {serialFor(op, args), 0};
}

CadStatus hostMakeBox(void* ctx, double dx, double dy, double dz, CadShape* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    auto built = cad::kernel::makeBox(dx, dy, dz);
    if (!built) {
        s->hostError = built.error().message;
        return CAD_ERR_INVALID_INPUT;
    }

    // NAMED, not just built. A shape handed to a plugin without its element map can only be
    // referenced positionally, and a positional reference breaks on the next edit -- so a host
    // call that produced anonymous geometry would hand plugins the FreeCAD problem by default.
    // The serial is derived from the request, so identical calls name identically. See
    // serialFor: counting interns instead made these names session-dependent.
    // once register_feature lands, a plugin's compute output takes its feature's serial instead.
    const auto [serial, opTag] = namingIdentityFor(*s, "make_box", {dx, dy, dz});
    cad::naming::NamingContext naming(serial, opTag);
    auto map = naming.nameprimitive(built.value().op.shape(), built.value().taggedFaces);
    if (!map) {
        s->hostError = map.error().message;
        return CAD_ERR_NAMING_LOST;
    }
    *out = intern(*s, built.value().op.shape(), std::move(map).value());
    return CAD_OK;
}

CadStatus hostBoolean(void* ctx, CadShape a, CadShape b, CadShape* out, bool cut) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    Session::StoredShape* leftStored = lookupStored(*s, a);
    Session::StoredShape* rightStored = lookupStored(*s, b);
    if (leftStored == nullptr || rightStored == nullptr) {
        s->hostError = "That shape no longer exists.";
        return CAD_ERR_BAD_HANDLE;
    }
    auto result = cut ? cad::kernel::booleanCut(leftStored->shape, rightStored->shape)
                      : cad::kernel::booleanFuse(leftStored->shape, rightStored->shape);
    if (!result) {
        s->hostError = result.error().message;
        return CAD_ERR_BOOLEAN_FAILED;
    }

    // PROPAGATED, not dropped. This used to intern the result shape with no element map at all,
    // so a plugin that fused two named boxes got back anonymous geometry — nothing downstream
    // could reference a face of it, which is exactly the failure PLUGIN_CONTRACT.md 4.2 tells
    // plugins not to cause. The host must not cause it either.
    std::uint32_t serial = serialForShapes(cut ? "boolean_cut" : "boolean_fuse",
                                          {leftStored, rightStored});
    std::uint16_t opTag = 0;
    if (Session::ActiveCompute* active = innermostCompute(*s);
        active != nullptr && active->ctx != nullptr) {
        serial = active->ctx->namingSerial;
        opTag = active->opTag++;
    }
    cad::naming::NamingContext naming(serial, opTag);
    auto map = naming.propagate(result.value(),
                                {&leftStored->shape, &rightStored->shape},
                                {&leftStored->names, &rightStored->names});
    if (!map) {
        s->hostError = map.error().message;
        return CAD_ERR_NAMING_LOST;
    }
    *out = intern(*s, result.value().shape(), std::move(map).value());
    return CAD_OK;
}

CadStatus hostFuse(void* ctx, CadShape a, CadShape b, CadShape* out) {
    return hostBoolean(ctx, a, b, out, false);
}
CadStatus hostCut(void* ctx, CadShape a, CadShape b, CadShape* out) {
    return hostBoolean(ctx, a, b, out, true);
}

CadStatus hostValidate(void* ctx, CadShape handle) {
    Session* s = hostSession(ctx);
    if (s == nullptr) return CAD_ERR_INVALID_INPUT;
    cad::kernel::Shape* shape = lookup(*s, handle);
    if (shape == nullptr) {
        s->hostError = "That shape no longer exists.";
        return CAD_ERR_BAD_HANDLE;
    }
    auto valid = shape->validate();
    if (!valid) {
        s->hostError = valid.error().message;
        return CAD_ERR_INVALID_RESULT;
    }
    return CAD_OK;
}

// ── naming ──────────────────────────────────────────────────────────────────────────────
//
// The reason plugins can hold a reference to a face across a rebuild at all. Without these two,
// a plugin could only refer to geometry positionally -- "face 3" -- and every edit that changed
// face order would break it. That is FreeCAD's topological naming problem, and it is a PLUGIN API
// problem there rather than only a modelling one, because every addon holding a face reference
// suffers it.

/// Maps a CAD_SUB_* constant onto the kernel's enum. Returns false for anything unknown, so a
/// plugin built against a newer header that adds a kind gets a clean refusal rather than whatever
/// enumerator happens to sit at that integer.
bool subShapeKind(std::uint32_t kind, cad::kernel::SubShape& out) {
    switch (kind) {
        case CAD_SUB_FACE:   out = cad::kernel::SubShape::Face;   return true;
        case CAD_SUB_EDGE:   out = cad::kernel::SubShape::Edge;   return true;
        case CAD_SUB_VERTEX: out = cad::kernel::SubShape::Vertex; return true;
        default: return false;
    }
}

CadStatus hostShapeSubCount(void* ctx, CadShape handle, std::uint32_t kind, std::uint32_t* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    cad::kernel::SubShape wanted = cad::kernel::SubShape::Face;
    if (!subShapeKind(kind, wanted)) {
        s->hostError = "Unknown sub-shape kind.";
        return CAD_ERR_INVALID_INPUT;
    }
    Session::StoredShape* stored = lookupStored(*s, handle);
    if (stored == nullptr) {
        s->hostError = "That shape no longer exists.";
        return CAD_ERR_BAD_HANDLE;
    }
    *out = static_cast<std::uint32_t>(cad::kernel::subShapes(stored->shape, wanted).size());
    return CAD_OK;
}

CadStatus hostShapeSubAt(void* ctx, CadShape handle, std::uint32_t kind, std::uint32_t index,
                         CadShape* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    cad::kernel::SubShape wanted = cad::kernel::SubShape::Face;
    if (!subShapeKind(kind, wanted)) {
        s->hostError = "Unknown sub-shape kind.";
        return CAD_ERR_INVALID_INPUT;
    }
    Session::StoredShape* stored = lookupStored(*s, handle);
    if (stored == nullptr) {
        s->hostError = "That shape no longer exists.";
        return CAD_ERR_BAD_HANDLE;
    }
    auto subs = cad::kernel::subShapes(stored->shape, wanted);
    if (index >= subs.size()) {
        s->hostError = "There is no sub-shape at that index.";
        return CAD_ERR_INVALID_INPUT;
    }

    // Interned with an EMPTY element map. A sub-shape's name lives in its PARENT's map — that is
    // what element_name_of(parent, sub) looks up — so giving the handle a map of its own would
    // invite the belief that a face carries identity independently of the solid it belongs to. It
    // does not, and cannot: a name is only meaningful against the shape it names.
    *out = intern(*s, std::move(subs[index]));
    return CAD_OK;
}

// ── registration ────────────────────────────────────────────────────────────────────────

/// Whether to run every plugin compute TWICE and compare, per `CAD_PLUGIN_DETERMINISM_CHECK`.
///
/// Contract 4.1 requires compute to be deterministic, and everything downstream depends on it: the
/// content-addressed cache returns a stored result instead of recomputing, so a plugin that is
/// nondeterministic does not fail -- it produces a document whose geometry depends on whether a
/// cache entry happened to exist. That is the worst failure shape available, because it is
/// invisible, unreproducible, and appears as "the file is different on my machine".
///
/// Doubling the work is exactly why this is off by default. It is off by an ENVIRONMENT VARIABLE
/// rather than a build flag on purpose: a plugin author must be able to turn it on against a
/// shipped build of the application, without compiling anything, on the day their feature starts
/// behaving oddly. A check that needs a custom build is a check nobody outside this repository can
/// run.
///
/// Read once. The value cannot change usefully mid-session, and re-reading per compute would put a
/// getenv in the inner loop of a recompute.
bool determinismCheck() {
    static const bool enabled = [] {
        const char* value = std::getenv("CAD_PLUGIN_DETERMINISM_CHECK");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

CadStatus hostRegisterFeature(void* ctx, const CadFeatureDesc* desc, const CadParamDesc* params,
                              std::uint32_t paramCount) {
    Session* s = hostSession(ctx);
    if (s == nullptr || desc == nullptr) return CAD_ERR_INVALID_INPUT;

    // Not from inside a compute (contract 4.6).
    //
    // This is the one re-entrant call a plugin can actually reach today, and it is not a
    // theoretical hazard: registering a type mutates the registry the recompute engine is
    // currently walking, and the engine holds a reference INTO that registry for the feature it
    // is computing. A rehash while that reference is live is a dangling read -- a crash inside
    // third-party code, at a stack depth that names the host, which section 4.6 exists to prevent.
    //
    // Rejected rather than deferred. Queueing the registration until the compute finishes would
    // "work", and would mean a plugin's feature type appears at a moment it cannot predict, which
    // is a worse contract than a clear refusal with a fix the author can act on.
    if (!s->computeStack.empty()) {
        s->hostError =
            "register_feature cannot be called from inside compute. Register feature types during "
            "initialize, before any document is computed.";
        return CAD_ERR_REENTRANT;
    }

    // struct_size before anything else. A descriptor from a newer header is longer than ours and
    // must not be read past our own end; one from an older header is shorter and its trailing
    // members are simply absent. This is the negotiation ADR 0011 rests on, and reading a field
    // without checking is how it stops working.
    if (desc->struct_size < offsetof(CadFeatureDesc, compute) + sizeof(desc->compute)) {
        s->hostError = "This feature descriptor is too small to contain a compute function.";
        return CAD_ERR_INVALID_INPUT;
    }
    if (desc->type_name == nullptr || *desc->type_name == '\0' || desc->compute == nullptr) {
        s->hostError = "A feature needs a type name and a compute function.";
        return CAD_ERR_INVALID_INPUT;
    }
    if (paramCount > 0 && params == nullptr) {
        s->hostError = "A feature declaring parameters must supply them.";
        return CAD_ERR_INVALID_INPUT;
    }
    if (s->registry.find(desc->type_name) != nullptr) {
        // Refused, not replaced. Two plugins claiming one type name would silently decide between
        // themselves by load order, and a document referencing that type would mean different
        // geometry depending on what happened to be installed.
        s->hostError = std::string("A feature type called '") + desc->type_name +
                       "' is already registered.";
        return CAD_ERR_INVALID_INPUT;
    }

    auto stored = std::make_unique<Session::RegisteredFeature>();
    stored->type = desc->type_name;
    stored->pluginCtx = desc->plugin_ctx;
    stored->compute = desc->compute;
    stored->computeVersion = desc->compute_version;
    Session::RegisteredFeature* feature = stored.get();
    s->features.push_back(std::move(stored));

    // Read only if the descriptor is long enough to contain it. A plugin compiled against an
    // older header has no such member, and reading one would be reading past its end.
    if (desc->struct_size >= offsetof(CadFeatureDesc, external_inputs)
                                 + sizeof(desc->external_inputs)) {
        feature->externalInputs = desc->external_inputs;
    }

    cad::recompute::FeatureType type;
    type.name = feature->type;
    type.version = feature->computeVersion;

    // The step-2 correction, actually connected. Declared but unwired, this silently reintroduced
    // the Import bug through the plugin path: a plugin would correctly declare the files it reads,
    // the host would ignore the declaration, and the cache would serve geometry from the old file
    // contents with nothing to suggest anything was wrong.
    if (feature->externalInputs != nullptr) {
        type.externalInputs =
            [s, feature](const cad::document::ObjectData& object) -> std::vector<std::string> {
            // A parameters-only context: no ComputeContext exists at cache-key time, because no
            // input has been computed yet. That timing is the whole reason external_inputs is on
            // the descriptor rather than callable from inside compute.
            const CadFeatureCtx handle = s->nextCompute++;
            s->computes.emplace(handle,
                                Session::ActiveCompute{nullptr, &object, 0, false, {}, {}, 0});

            std::vector<std::string> paths;
            const auto sink = [](void* sinkCtx, const char* path, std::size_t len) {
                if (path == nullptr || len == 0) return;
                static_cast<std::vector<std::string>*>(sinkCtx)->emplace_back(path, len);
            };
            feature->externalInputs(feature->pluginCtx, handle, sink, &paths);

            s->computes.erase(handle);
            return paths;
        };
    }

    // The bridge. `s` and `feature` both outlive the registry: the session owns both, and the
    // registry is destroyed with it.
    //
    // determinismCheck() below turns contract 4.1 -- the strictest rule in the whole document --
    // from prose into something that fails.
    type.compute = [s, feature](const cad::recompute::ComputeContext& cc)
        -> cad::kernel::Result<cad::recompute::Output> {
        const CadComputeCtx handle = s->nextCompute++;
        s->computes.emplace(handle,
                            Session::ActiveCompute{&cc, &cc.object, 0, false, {}, {}, 0});
        s->computeStack.push_back(handle);

        const CadStatus status = feature->compute(feature->pluginCtx, handle);

        // Copied out before erasing: the plugin's message has to outlive its own frame.
        Session::ActiveCompute finished = s->computes[handle];
        s->computeStack.pop_back();
        s->computes.erase(handle);

        if (status != CAD_OK || !finished.hasOutput) {
            // compute_fail's message if the plugin left one, and a plain statement if it did not.
            // Never a bare status code: 3.5 promises a plugin's failures are as legible as a
            // built-in's, and "operation failed" in a model tree is what that promise exists to
            // prevent.
            std::string message = finished.failMessage;
            if (message.empty()) {
                message = status == CAD_OK
                              ? std::string("'") + feature->type + "' produced no shape."
                              : std::string("'") + feature->type + "' could not be computed.";
            }
            return cad::kernel::Error{cad::kernel::ErrorCode::Internal, std::move(message),
                                      std::move(finished.failDetail)};
        }

        Session::StoredShape* result = lookupStored(*s, finished.output);
        if (result == nullptr) {
            return cad::kernel::Error{cad::kernel::ErrorCode::Internal,
                                      std::string("'") + feature->type +
                                          "' returned a shape that no longer exists."};
        }
        if (result->names.size() == 0) {
            // A shape with no names cannot be built on: a fillet on one of its faces would have
            // nothing to reference and would break on the next edit. 4.2 requires plugins to name
            // what they create; this is where that requirement is actually enforced rather than
            // documented.
            return cad::kernel::Error{cad::kernel::ErrorCode::NamingLost,
                                      std::string("'") + feature->type +
                                          "' produced geometry with no element names."};
        }
        if (!determinismCheck()) {
            return cad::recompute::Output{result->shape, result->names};
        }

        // Run it again and compare. Compared by naming::contentHash rather than by anything
        // hand-rolled, because that is the SAME hash the content-addressed cache keys on: if two
        // runs agree under it, the cache cannot tell them apart, which is precisely the property
        // 4.1 is asking for. A weaker comparison would pass while the cache still saw two
        // different results.
        const cad::kernel::ShapeHash first = cad::naming::contentHash(result->shape, result->names);

        const CadComputeCtx second = s->nextCompute++;
        s->computes.emplace(second,
                            Session::ActiveCompute{&cc, &cc.object, 0, false, {}, {}, 0});
        s->computeStack.push_back(second);
        const CadStatus secondStatus = feature->compute(feature->pluginCtx, second);
        Session::ActiveCompute secondFinished = s->computes[second];
        s->computeStack.pop_back();
        s->computes.erase(second);

        if (secondStatus != CAD_OK || !secondFinished.hasOutput) {
            // Succeeding once and failing once is nondeterminism of the worst kind, and worth its
            // own message: the first run has already produced a shape that would have been cached.
            return cad::kernel::Error{
                cad::kernel::ErrorCode::Internal,
                std::string("'") + feature->type +
                    "' is not deterministic: the same inputs succeeded once and failed once.",
                "CAD_PLUGIN_DETERMINISM_CHECK is on; see contract 4.1"};
        }

        Session::StoredShape* again = lookupStored(*s, secondFinished.output);
        if (again == nullptr) {
            return cad::kernel::Error{cad::kernel::ErrorCode::Internal,
                                      std::string("'") + feature->type +
                                          "' returned a shape that no longer exists."};
        }

        const cad::kernel::ShapeHash repeat = cad::naming::contentHash(again->shape, again->names);
        if (first.fold64() != repeat.fold64()) {
            return cad::kernel::Error{
                cad::kernel::ErrorCode::Internal,
                std::string("'") + feature->type +
                    "' is not deterministic: the same inputs produced different geometry or "
                    "different element names on two consecutive computes.",
                first.hex() + " then " + repeat.hex()};
        }

        // The FIRST result is returned, not the second. They are equal under the cache's own hash,
        // so the choice does not affect correctness -- but returning the first keeps the check
        // observation-only, and means turning it on cannot change which shape a document gets.
        return cad::recompute::Output{result->shape, result->names};
    };

    s->registry.add(std::move(type));
    return CAD_OK;
}

// ── ribbon ──────────────────────────────────────────────────────────────────────────────
//
// Three levels, all legal for a plugin: a new TAB, a new SECTION in any tab, or a COMMAND in any
// section. Ordered validation is the whole design — a section must name a tab that exists and a
// command must name a section that exists, and naming something unknown is REFUSED rather than
// having it created. Implicit creation means a typo produces an empty tab called "Creat" and the
// author sees a missing button with no error anywhere.

/// Sets the message the HOST vtable's last_error returns, and hands back the status.
///
/// Not `fail()`. That writes the Session API's error channel, which `cad_last_error` reads; the
/// vtable's `last_error` reads `hostError`. A plugin calling through the vtable got a status code
/// with no message, and the test that caught it asserted the refusal EXPLAINED itself rather than
/// only that it happened.
CadStatus hostFail(Session& s, CadStatus status, std::string message) {
    s.hostError = std::move(message);
    return status;
}

CadStatus hostRegisterSettingsPage(void* ctx, const CadSettingsPageDesc* page,
                                   const CadSettingDesc* settings, std::uint32_t count) {
    Session* s = hostSession(ctx);
    if (s == nullptr || page == nullptr) return CAD_ERR_INVALID_INPUT;
    if (page->struct_size < offsetof(CadSettingsPageDesc, label) + sizeof(page->label)) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, "This settings page descriptor is too small.");
    }
    if (page->id == nullptr || *page->id == '\0') {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, "A settings page needs an id.");
    }
    if (count > 0 && settings == nullptr) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT,
                        "A settings page declaring fields must supply them.");
    }

    // MERGED into an existing page, unlike a ribbon tab which is refused. Two plugins adding a
    // group to a shared page is the normal case; two claiming one ribbon tab is not.
    auto at = std::find_if(s->settingsPages.begin(), s->settingsPages.end(),
                          [&](const Session::StoredSettingsPage& p) { return p.id == page->id; });
    if (at == s->settingsPages.end()) {
        Session::StoredSettingsPage fresh;
        fresh.id = page->id;
        fresh.label = page->label != nullptr ? page->label : page->id;
        if (page->struct_size >= offsetof(CadSettingsPageDesc, icon_name) + sizeof(page->icon_name)
            && page->icon_name != nullptr) {
            fresh.iconName = page->icon_name;
        }
        if (page->struct_size >= offsetof(CadSettingsPageDesc, group_label)
                                     + sizeof(page->group_label)
            && page->group_label != nullptr) {
            fresh.groupLabel = page->group_label;
        }
        s->settingsPages.push_back(std::move(fresh));
        at = std::prev(s->settingsPages.end());
    }

    const std::size_t stride = settings != nullptr && settings->struct_size > 0
                                   ? settings->struct_size
                                   : sizeof(CadSettingDesc);
    for (std::uint32_t i = 0; i < count; ++i) {
        // Walked by the CALLER's stride, not ours. A plugin built against an older header has
        // shorter descriptors, and stepping by sizeof(CadSettingDesc) would read the second one
        // from the middle of the first.
        const auto* raw = reinterpret_cast<const CadSettingDesc*>(
            reinterpret_cast<const unsigned char*>(settings) + i * stride);
        if (raw->struct_size < offsetof(CadSettingDesc, id) + sizeof(raw->id)) continue;
        if (raw->id == nullptr || *raw->id == '\0') continue;

        // A duplicate id is dropped rather than shadowing: which value a user edited must not
        // depend on load order.
        bool duplicate = false;
        for (const auto& existingPage : s->settingsPages) {
            for (const auto& existing : existingPage.settings) {
                if (existing.id == raw->id) duplicate = true;
            }
        }
        if (duplicate) continue;

        // EVERY field gated on the caller's own struct_size. Walking by the right stride is only
        // half of it: reading `description` or `kind` from a descriptor that ends after `label` is
        // reading past the end of the plugin's memory, and it segfaulted the moment a test passed a
        // genuinely shorter struct. This is the rule the size prefix exists for, applied to the
        // INPUT side rather than only to the output.
        const std::uint32_t room = raw->struct_size;
        const auto has = [room](std::size_t offset, std::size_t size) {
            return offset + size <= room;
        };

        Session::StoredSetting stored;
        stored.id = raw->id;
        if (has(offsetof(CadSettingDesc, label), sizeof(raw->label)) && raw->label != nullptr) {
            stored.label = raw->label;
        } else {
            stored.label = raw->id;
        }
        if (has(offsetof(CadSettingDesc, description), sizeof(raw->description))
            && raw->description != nullptr) {
            stored.description = raw->description;
        }
        if (has(offsetof(CadSettingDesc, kind), sizeof(raw->kind))) stored.kind = raw->kind;
        if (has(offsetof(CadSettingDesc, default_value), sizeof(raw->default_value))) {
            stored.defaultValue = raw->default_value;
        }
        if (has(offsetof(CadSettingDesc, minimum), sizeof(raw->minimum))) {
            stored.minimum = raw->minimum;
        }
        if (has(offsetof(CadSettingDesc, maximum), sizeof(raw->maximum))) {
            stored.maximum = raw->maximum;
        }
        if (has(offsetof(CadSettingDesc, default_text), sizeof(raw->default_text))
            && raw->default_text != nullptr) {
            stored.defaultText = raw->default_text;
        }
        if (has(offsetof(CadSettingDesc, choice_count), sizeof(raw->choice_count))
            && raw->choices != nullptr) {
            for (std::uint32_t c = 0; c < raw->choice_count; ++c) {
                if (raw->choices[c] != nullptr) stored.choices.emplace_back(raw->choices[c]);
            }
        }
        at->settings.push_back(std::move(stored));
    }
    return CAD_OK;
}

CadStatus hostRegisterTab(void* ctx, const CadTabDesc* desc) {
    Session* s = hostSession(ctx);
    if (s == nullptr || desc == nullptr) return CAD_ERR_INVALID_INPUT;
    if (desc->struct_size < offsetof(CadTabDesc, label) + sizeof(desc->label)) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, "This tab descriptor is too small to be read.");
    }
    if (desc->id == nullptr || *desc->id == '\0') {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, "A ribbon tab needs an id.");
    }
    s->seedBuiltinRibbon();
    if (s->hasTab(desc->id)) {
        // Refused, not merged. Two plugins sharing a tab id would each believe they owned it, and
        // which one's label a user saw would depend on load order.
        return hostFail(*s, CAD_ERR_INVALID_INPUT, std::string("A ribbon tab called '") + desc->id + "' already exists.");
    }
    Session::RibbonTab tab;
    tab.id = desc->id;
    tab.label = desc->label != nullptr ? desc->label : desc->id;
    tab.order = desc->order;
    s->tabs.push_back(std::move(tab));
    return CAD_OK;
}

CadStatus hostRegisterSection(void* ctx, const CadSectionDesc* desc) {
    Session* s = hostSession(ctx);
    if (s == nullptr || desc == nullptr) return CAD_ERR_INVALID_INPUT;
    if (desc->struct_size < offsetof(CadSectionDesc, tab) + sizeof(desc->tab)) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, "This section descriptor is too small to be read.");
    }
    if (desc->id == nullptr || *desc->id == '\0' || desc->tab == nullptr) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, "A ribbon section needs an id and a tab.");
    }
    s->seedBuiltinRibbon();
    if (!s->hasTab(desc->tab)) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, std::string("There is no ribbon tab called '") + desc->tab
                        + "'. Register the tab before its sections.");
    }
    if (s->hasSection(desc->id)) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, std::string("A ribbon section called '") + desc->id + "' already exists.");
    }
    Session::RibbonSection section;
    section.id = desc->id;
    section.label = desc->label != nullptr ? desc->label : desc->id;
    section.tab = desc->tab;
    section.order = desc->order;
    s->sections.push_back(std::move(section));
    return CAD_OK;
}

CadStatus hostRegisterCommand(void* ctx, const CadCommandDesc* desc) {
    Session* s = hostSession(ctx);
    if (s == nullptr || desc == nullptr) return CAD_ERR_INVALID_INPUT;
    if (desc->struct_size < offsetof(CadCommandDesc, invoke) + sizeof(desc->invoke)) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, "This command descriptor has no invoke function.");
    }
    if (desc->id == nullptr || *desc->id == '\0' || desc->invoke == nullptr) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, "A command needs an id and something to invoke.");
    }
    s->seedBuiltinRibbon();

    const bool anyOf = std::any_of(s->ribbonCommands.begin(), s->ribbonCommands.end(),
                                  [&](const Session::RibbonCommand& c) {
                                      return c.id == desc->id;
                                  });
    if (anyOf) {
        return hostFail(*s, CAD_ERR_INVALID_INPUT, std::string("A command called '") + desc->id + "' already exists.");
    }

    Session::RibbonCommand cmd;
    cmd.id = desc->id;
    cmd.label = desc->label != nullptr ? desc->label : desc->id;
    cmd.tooltip = desc->tooltip != nullptr ? desc->tooltip : "";
    cmd.icon = desc->icon_name != nullptr ? desc->icon_name : "";

    // Placement is only read if the descriptor is long enough to carry it — a plugin built against
    // 1.18 has no such field, and reading it would be reading past its end.
    const bool placed = desc->struct_size >= offsetof(CadCommandDesc, order)
                                                + sizeof(desc->order);
    if (placed && desc->section != nullptr && *desc->section != '\0') {
        if (!s->hasSection(desc->section)) {
            return hostFail(*s, CAD_ERR_INVALID_INPUT, std::string("There is no ribbon section called '") + desc->section
                            + "'. Register the section before its commands.");
        }
        cmd.section = desc->section;
        cmd.tab = desc->tab != nullptr ? desc->tab : "";
        cmd.order = desc->order;
        cmd.placement = desc->placement;
    } else {
        // No placement asked for. The host puts it somewhere findable rather than nowhere: a
        // command that registers successfully and appears in no menu is worse than one refused.
        cmd.section = CAD_SECTION_CREATE;
        cmd.tab = CAD_TAB_MODEL;
        cmd.placement = CAD_UI_RIBBON;
    }

    cmd.pluginCtx = desc->plugin_ctx;
    cmd.enabled = desc->enabled;
    cmd.invoke = desc->invoke;
    s->ribbonCommands.push_back(std::move(cmd));
    return CAD_OK;
}

// ── compute context ─────────────────────────────────────────────────────────────────────

CadStatus hostComputeInputCount(void* ctx, CadComputeCtx cc, std::uint32_t* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    Session::ActiveCompute* active = activeCompute(*s, cc);
    if (active == nullptr || active->ctx == nullptr) {
        s->hostError = "That compute is no longer running.";
        return CAD_ERR_BAD_HANDLE;
    }
    *out = static_cast<std::uint32_t>(active->ctx->inputs.size());
    return CAD_OK;
}

CadStatus hostComputeInputShape(void* ctx, CadComputeCtx cc, std::uint32_t index, CadShape* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    Session::ActiveCompute* active = activeCompute(*s, cc);
    if (active == nullptr || active->ctx == nullptr) {
        s->hostError = "That compute is no longer running.";
        return CAD_ERR_BAD_HANDLE;
    }
    if (index >= active->ctx->inputs.size()) {
        s->hostError = "There is no input at that index.";
        return CAD_ERR_INVALID_INPUT;
    }
    const auto* input = active->ctx->inputs[index];
    // Interned WITH its element map, so a plugin can name a face of its input — which is the
    // whole point of a feature that modifies something rather than creating it.
    *out = intern(*s, input->shape, input->map);
    return CAD_OK;
}

/// One property of the object being computed, or a legible refusal.
const cad::document::PropertyValue* computeProperty(Session& s, CadComputeCtx cc,
                                                    const char* name, CadStatus& status) {
    Session::ActiveCompute* active = activeCompute(s, cc);
    if (active == nullptr || active->object == nullptr) {
        s.hostError = "That compute is no longer running.";
        status = CAD_ERR_BAD_HANDLE;
        return nullptr;
    }
    if (name == nullptr) {
        status = CAD_ERR_INVALID_INPUT;
        return nullptr;
    }
    const auto* value = active->object->find(name);
    if (value == nullptr) {
        // Refused, never defaulted. A plugin reading a parameter that is not there has a bug, and
        // handing back 0.0 would bury it inside geometry where it surfaces as a wrong part.
        s.hostError = std::string("This feature has no parameter called '") + name + "'.";
        status = CAD_ERR_INVALID_INPUT;
        return nullptr;
    }
    status = CAD_OK;
    return value;
}

CadStatus hostComputeParamReal(void* ctx, CadComputeCtx cc, const char* name, double* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    CadStatus status = CAD_OK;
    const auto* value = computeProperty(*s, cc, name, status);
    if (value == nullptr) return status;

    // Length and Angle answer in base units, the same convention the C ABI uses everywhere else:
    // millimetres and degrees cross the boundary as plain doubles.
    if (const auto* length = std::get_if<cad::units::Length>(value)) {
        *out = length->base();
        return CAD_OK;
    }
    if (const auto* angle = std::get_if<cad::units::Angle>(value)) {
        *out = angle->base();
        return CAD_OK;
    }
    if (const auto* real = std::get_if<double>(value)) {
        *out = *real;
        return CAD_OK;
    }
    s->hostError = std::string("'") + name + "' is not a number.";
    return CAD_ERR_INVALID_INPUT;
}

CadStatus hostComputeParamInt(void* ctx, CadComputeCtx cc, const char* name, std::int64_t* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    CadStatus status = CAD_OK;
    const auto* value = computeProperty(*s, cc, name, status);
    if (value == nullptr) return status;
    if (const auto* i = std::get_if<std::int64_t>(value)) {
        *out = *i;
        return CAD_OK;
    }
    if (const auto* b = std::get_if<bool>(value)) {
        *out = *b ? 1 : 0;
        return CAD_OK;
    }
    s->hostError = std::string("'") + name + "' is not a whole number.";
    return CAD_ERR_INVALID_INPUT;
}

CadStatus hostComputeParamText(void* ctx, CadComputeCtx cc, const char* name, CadStr* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    CadStatus status = CAD_OK;
    const auto* value = computeProperty(*s, cc, name, status);
    if (value == nullptr) return status;
    const auto* text = std::get_if<std::string>(value);
    if (text == nullptr) {
        s->hostError = std::string("'") + name + "' is not text.";
        return CAD_ERR_INVALID_INPUT;
    }
    // Into the session's scratch, like every other string crossing this boundary: valid until the
    // next call on this session, and the plugin is told to copy it immediately (4.5).
    s->scratch = *text;
    out->data = s->scratch.c_str();
    out->len = s->scratch.size();
    return CAD_OK;
}

/// Reads an element name out of a property, shared by the single and list accessors.
CadStatus elementOut(Session& s, const cad::naming::ElementName& name, CadElementId* out) {
    s.scratch = name.toString();
    out->digest = name.digest();
    out->text = s.scratch.c_str();
    out->text_len = s.scratch.size();
    return CAD_OK;
}

CadStatus hostComputeParamElement(void* ctx, CadComputeCtx cc, const char* name,
                                  CadElementId* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    CadStatus status = CAD_OK;
    const auto* value = computeProperty(*s, cc, name, status);
    if (value == nullptr) return status;
    if (const auto* element = std::get_if<cad::naming::ElementName>(value)) {
        return elementOut(*s, *element, out);
    }
    s->hostError = "That parameter is not an element reference.";
    return CAD_ERR_INVALID_INPUT;
}

CadStatus hostComputeParamCount(void* ctx, CadComputeCtx cc, const char* name,
                                std::uint32_t* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    CadStatus status = CAD_OK;
    const auto* value = computeProperty(*s, cc, name, status);
    if (value == nullptr) return status;
    if (const auto* objects = std::get_if<std::vector<cad::document::ObjectId>>(value)) {
        *out = static_cast<std::uint32_t>(objects->size());
        return CAD_OK;
    }
    if (const auto* elements = std::get_if<std::vector<cad::naming::ElementName>>(value)) {
        *out = static_cast<std::uint32_t>(elements->size());
        return CAD_OK;
    }
    s->hostError = "That parameter is not a list.";
    return CAD_ERR_INVALID_INPUT;
}

CadStatus hostComputeParamElementAt(void* ctx, CadComputeCtx cc, const char* name,
                                    std::uint32_t index, CadElementId* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    CadStatus status = CAD_OK;
    const auto* value = computeProperty(*s, cc, name, status);
    if (value == nullptr) return status;
    const auto* elements = std::get_if<std::vector<cad::naming::ElementName>>(value);
    if (elements == nullptr) {
        s->hostError = "That parameter is not a list of elements.";
        return CAD_ERR_INVALID_INPUT;
    }
    if (index >= elements->size()) {
        // Out of range is BAD_HANDLE rather than INVALID_INPUT: the plugin asked for an item that
        // does not exist, which is the same class of mistake as using a stale handle, and it must
        // never read past the end.
        s->hostError = "That list has no item at that index.";
        return CAD_ERR_BAD_HANDLE;
    }
    return elementOut(*s, (*elements)[index], out);
}

CadStatus hostComputeParamShapeAt(void* ctx, CadComputeCtx cc, const char* name,
                                  std::uint32_t index, CadShape* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    Session::ActiveCompute* active = activeCompute(*s, cc);
    if (active == nullptr || active->ctx == nullptr) {
        // Deliberately stricter than the parameter accessors: resolving an object reference to a
        // SHAPE needs computed inputs, which exist during compute and do not exist at cache-key
        // time. Answering from a parameters-only context would mean inventing geometry.
        s->hostError = "Shapes are only readable while the feature is computing.";
        return CAD_ERR_BAD_HANDLE;
    }
    CadStatus status = CAD_OK;
    const auto* value = computeProperty(*s, cc, name, status);
    if (value == nullptr) return status;
    const auto* objects = std::get_if<std::vector<cad::document::ObjectId>>(value);
    if (objects == nullptr) {
        s->hostError = "That parameter is not a list of objects.";
        return CAD_ERR_INVALID_INPUT;
    }
    if (index >= objects->size()) {
        s->hostError = "That list has no item at that index.";
        return CAD_ERR_BAD_HANDLE;
    }
    if (index >= active->ctx->inputs.size() || active->ctx->inputs[index] == nullptr) {
        s->hostError = "That input produced no shape.";
        return CAD_ERR_BAD_HANDLE;
    }
    *out = intern(*s, active->ctx->inputs[index]->shape, active->ctx->inputs[index]->map);
    return CAD_OK;
}

CadStatus hostComputeFeatureCtx(void* ctx, CadComputeCtx cc, CadFeatureCtx* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    if (activeCompute(*s, cc) == nullptr) {
        s->hostError = "That compute is no longer running.";
        return CAD_ERR_BAD_HANDLE;
    }
    // One handle space, deliberately: a CadFeatureCtx IS a compute handle restricted to its
    // parameters, so the accessors are written once instead of twice.
    *out = cc;
    return CAD_OK;
}

CadStatus hostComputeSetOutput(void* ctx, CadComputeCtx cc, CadShape shape) {
    Session* s = hostSession(ctx);
    if (s == nullptr) return CAD_ERR_INVALID_INPUT;
    Session::ActiveCompute* active = activeCompute(*s, cc);
    if (active == nullptr || active->ctx == nullptr) {
        s->hostError = "That compute is no longer running.";
        return CAD_ERR_BAD_HANDLE;
    }
    if (active->hasOutput) {
        // Refused rather than overwritten: a compute that sets two outputs does not know what it
        // is building, and silently keeping the last would make which one survive an accident of
        // control flow.
        s->hostError = "This compute has already produced its output.";
        return CAD_ERR_INVALID_INPUT;
    }
    if (lookupStored(*s, shape) == nullptr) {
        s->hostError = "That shape no longer exists.";
        return CAD_ERR_BAD_HANDLE;
    }
    active->output = shape;
    active->hasOutput = true;
    return CAD_OK;
}

CadStatus hostComputeFail(void* ctx, CadComputeCtx cc, const char* message, const char* detail) {
    Session* s = hostSession(ctx);
    if (s == nullptr) return CAD_ERR_INVALID_INPUT;
    Session::ActiveCompute* active = activeCompute(*s, cc);
    if (active == nullptr) {
        s->hostError = "That compute is no longer running.";
        return CAD_ERR_BAD_HANDLE;
    }
    active->failMessage = message != nullptr ? message : "";
    active->failDetail = detail != nullptr ? detail : "";
    return CAD_OK;
}

CadStatus hostElementNameOf(void* ctx, CadShape shape, CadShape sub, CadElementId* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    Session::StoredShape* stored = lookupStored(*s, shape);
    cad::kernel::Shape* subShape = lookup(*s, sub);
    if (stored == nullptr || subShape == nullptr) {
        s->hostError = "That shape no longer exists.";
        return CAD_ERR_BAD_HANDLE;
    }
    const auto name = stored->names.nameOf(*subShape);
    if (!name) {
        // Named as lost rather than guessed at. A positional fallback here would be the exact
        // failure this API exists to prevent, and it would be invisible until an edit reordered
        // the faces.
        s->hostError = "That sub-shape has no stable name in this shape.";
        return CAD_ERR_NAMING_LOST;
    }
    s->scratch = name->toString();
    out->digest = name->digest();
    out->text = s->scratch.c_str();
    out->text_len = s->scratch.size();
    return CAD_OK;
}

CadStatus hostElementResolve(void* ctx, CadShape shape, const CadElementId* id, CadShape* out) {
    Session* s = hostSession(ctx);
    if (s == nullptr || id == nullptr || out == nullptr) return CAD_ERR_INVALID_INPUT;
    Session::StoredShape* stored = lookupStored(*s, shape);
    if (stored == nullptr) {
        s->hostError = "That shape no longer exists.";
        return CAD_ERR_BAD_HANDLE;
    }

    // By digest first, by text second. The digest is the fast path; the text is what survives a
    // save/reload and what a plugin may have persisted in its own parameters, so both must work.
    auto found = stored->names.resolve(cad::naming::ElementId{id->digest});
    if (!found && id->text != nullptr && id->text_len > 0) {
        found = stored->names.resolve(
            cad::naming::ElementName::parse(std::string_view(id->text, id->text_len)));
    }
    if (!found) {
        s->hostError = "That face or edge no longer exists in this shape.";
        return CAD_ERR_NAMING_LOST;
    }
    *out = intern(*s, *found);
    return CAD_OK;
}

void hostReleaseShape(void* ctx, CadShape handle) {
    Session* s = hostSession(ctx);
    if (s == nullptr) return;
    // Idempotent, and accepts 0 -- the uniform rule for every release in this ABI. A plugin
    // shutting down after a failure releases things it may already have released.
    s->shapes.erase(handle);
}

}  // namespace
}  // extern "C++"

const CadHost* cad_plugin_host(CadSession handle) {
    const CadHost* result = nullptr;
    withSession(handle, [&](Session& s) {
        if (!s.host) {
            s.host = std::make_unique<CadHost>();
            CadHost& h = *s.host;
            h = CadHost{};
            h.struct_size = static_cast<std::uint32_t>(sizeof(CadHost));
            h.struct_version = 1;
            h.abi_major = CAD_ABI_VERSION_MAJOR;
            h.abi_minor = CAD_ABI_VERSION_MINOR;
            h.host_ctx = &s;

            h.log = &hostLog;
            h.last_error = &hostLastError;
            h.make_box = &hostMakeBox;
            h.boolean_fuse = &hostFuse;
            h.boolean_cut = &hostCut;
            h.shape_validate = &hostValidate;
            h.shape_release = &hostReleaseShape;
            h.element_resolve = &hostElementResolve;
            h.element_name_of = &hostElementNameOf;
            h.shape_sub_count = &hostShapeSubCount;
            h.shape_sub_at = &hostShapeSubAt;
            h.register_feature = &hostRegisterFeature;
            h.register_command = &hostRegisterCommand;
            h.register_tab = &hostRegisterTab;
            h.register_settings_page = &hostRegisterSettingsPage;
            h.register_section = &hostRegisterSection;
            h.compute_input_count = &hostComputeInputCount;
            h.compute_input_shape = &hostComputeInputShape;
            h.compute_param_real = &hostComputeParamReal;
            h.compute_param_int = &hostComputeParamInt;
            h.compute_param_text = &hostComputeParamText;
            h.compute_set_output = &hostComputeSetOutput;
            h.compute_fail = &hostComputeFail;
            h.compute_param_element = &hostComputeParamElement;
            h.compute_param_count = &hostComputeParamCount;
            h.compute_param_element_at = &hostComputeParamElementAt;
            h.compute_param_shape_at = &hostComputeParamShapeAt;
            h.compute_feature_ctx = &hostComputeFeatureCtx;
            // fillet_edges, txn_*, register_command and register_format stay NULL. See the note
            // above: plugins are required to check, and this makes them do it from the start.
        }
        result = s.host.get();
        return CAD_OK;
    });
    return result;
}

CadStatus cad_plugins_load(CadSession handle, const char* directory, std::uint32_t* outLoaded,
                           std::uint32_t* outFailed) {
    if (outLoaded != nullptr) *outLoaded = 0;
    if (outFailed != nullptr) *outFailed = 0;

    // Outside withSession, because cad_plugin_host takes the same session lock this would hold.
    const CadHost* host = cad_plugin_host(handle);
    if (host == nullptr) return CAD_ERR_INVALID_INPUT;

    std::vector<std::filesystem::path> directories;
    std::uint32_t loaded = 0;
    std::uint32_t failed = 0;
    std::string lastFailure;

    const CadStatus scanned = withSession(handle, [&](Session& s) {
        if (directory == nullptr || *directory == '\0') {
            return fail(s, CAD_ERR_INVALID_INPUT, "No plugin directory given.");
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            // Not an error. A machine with no plugins installed is the common case, and a host
            // that reported failure for it would make "no plugins" indistinguishable from "the
            // plugin system is broken".
            return CAD_OK;
        }
        directories = cad::abi::discoverPluginDirectories(directory);
        return CAD_OK;
    });
    if (scanned != CAD_OK) return scanned;

    for (const auto& dir : directories) {
        // The plugin's initialize() runs inside this call and calls BACK into the host through the
        // vtable -- so the session must not be locked here. Loading under the lock deadlocks on the
        // plugin's first registration, which is every plugin's first action.
        auto result = cad::abi::loadPluginFrom(dir, host);
        if (result) {
            withSession(handle, [&](Session& s) {
                s.loadedPlugins.push_back(std::move(result).value());
                return CAD_OK;
            });
            ++loaded;
        } else {
            // Counted and remembered, never fatal: one bad plugin must not stop the others, and a
            // silent skip would leave a user wondering why their plugin does nothing.
            lastFailure = result.error().message;
            ++failed;
        }
    }

    // Written LAST, and only here. withSession clears lastError on entry, so a message stored as
    // each failure happened would be wiped by the next plugin that loaded successfully -- leaving
    // the shell a failure count with no reason, which is the state this whole channel exists to
    // avoid. Recorded once, after the loop, where nothing follows to clear it.
    if (!lastFailure.empty()) {
        withSession(handle, [&](Session& s) {
            s.lastError = lastFailure;
            return CAD_OK;
        });
    }

    if (outLoaded != nullptr) *outLoaded = loaded;
    if (outFailed != nullptr) *outFailed = failed;
    return CAD_OK;
}

CadStatus cad_settings_page_count(CadSession handle, std::uint32_t* out) {
    return withSession(handle, [&](Session& s) {
        if (out != nullptr) *out = static_cast<std::uint32_t>(s.settingsPages.size());
        return CAD_OK;
    });
}

CadStatus cad_settings_page_at(CadSession handle, std::uint32_t index, CadSettingsPageDesc* out,
                               std::uint32_t* outSettingCount) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr || index >= s.settingsPages.size()) {
            return fail(s, CAD_ERR_INVALID_INPUT, "No settings page at that index.");
        }
        const Session::StoredSettingsPage& page = s.settingsPages[index];

        // Written only as far as the CALLER's struct_size. A shell built against an older header has
        // a shorter struct, and filling our whole one would write past its end -- the mirror image
        // of the rule that stops us READING past a plugin's.
        const std::uint32_t room = out->struct_size;
        const auto fits = [room](std::size_t offset, std::size_t size) {
            return offset + size <= room;
        };
        if (fits(offsetof(CadSettingsPageDesc, id), sizeof(out->id))) out->id = page.id.c_str();
        if (fits(offsetof(CadSettingsPageDesc, label), sizeof(out->label))) {
            out->label = page.label.c_str();
        }
        if (fits(offsetof(CadSettingsPageDesc, icon_name), sizeof(out->icon_name))) {
            out->icon_name = page.iconName.c_str();
        }
        if (fits(offsetof(CadSettingsPageDesc, group_label), sizeof(out->group_label))) {
            out->group_label = page.groupLabel.c_str();
        }
        if (outSettingCount != nullptr) {
            *outSettingCount = static_cast<std::uint32_t>(page.settings.size());
        }
        return CAD_OK;
    });
}

CadStatus cad_settings_at(CadSession handle, std::uint32_t pageIndex, std::uint32_t settingIndex,
                          CadSettingDesc* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr || pageIndex >= s.settingsPages.size()) {
            return fail(s, CAD_ERR_INVALID_INPUT, "No settings page at that index.");
        }
        const Session::StoredSettingsPage& page = s.settingsPages[pageIndex];
        if (settingIndex >= page.settings.size()) {
            return fail(s, CAD_ERR_INVALID_INPUT, "No setting at that index.");
        }
        const Session::StoredSetting& setting = page.settings[settingIndex];

        const std::uint32_t room = out->struct_size;
        const auto fits = [room](std::size_t offset, std::size_t size) {
            return offset + size <= room;
        };
        if (fits(offsetof(CadSettingDesc, id), sizeof(out->id))) out->id = setting.id.c_str();
        if (fits(offsetof(CadSettingDesc, label), sizeof(out->label))) {
            out->label = setting.label.c_str();
        }
        if (fits(offsetof(CadSettingDesc, description), sizeof(out->description))) {
            out->description = setting.description.c_str();
        }
        if (fits(offsetof(CadSettingDesc, kind), sizeof(out->kind))) out->kind = setting.kind;
        if (fits(offsetof(CadSettingDesc, default_value), sizeof(out->default_value))) {
            out->default_value = setting.defaultValue;
        }
        if (fits(offsetof(CadSettingDesc, minimum), sizeof(out->minimum))) {
            out->minimum = setting.minimum;
        }
        if (fits(offsetof(CadSettingDesc, maximum), sizeof(out->maximum))) {
            out->maximum = setting.maximum;
        }
        if (fits(offsetof(CadSettingDesc, default_text), sizeof(out->default_text))) {
            out->default_text = setting.defaultText.c_str();
        }
        if (fits(offsetof(CadSettingDesc, choice_count), sizeof(out->choice_count))) {
            // Pointers into a session-owned scratch array, so they outlive this call by exactly as
            // long as every other string this ABI hands back: until the next one.
            s.choicePointers.clear();
            for (const std::string& choice : setting.choices) {
                s.choicePointers.push_back(choice.c_str());
            }
            out->choices = s.choicePointers.empty() ? nullptr : s.choicePointers.data();
            out->choice_count = static_cast<std::uint32_t>(s.choicePointers.size());
        }
        return CAD_OK;
    });
}

CadStatus cad_ribbon_counts(CadSession handle, std::uint32_t* outTabs, std::uint32_t* outSections,
                            std::uint32_t* outCommands) {
    return withSession(handle, [&](Session& s) {
        // Seeded on read as well as on write, so a session nobody registered into still reports the
        // built-in ribbon rather than an empty one.
        s.seedBuiltinRibbon();
        if (outTabs != nullptr) *outTabs = static_cast<std::uint32_t>(s.tabs.size());
        if (outSections != nullptr) *outSections = static_cast<std::uint32_t>(s.sections.size());
        if (outCommands != nullptr) {
            *outCommands = static_cast<std::uint32_t>(s.ribbonCommands.size());
        }
        return CAD_OK;
    });
}

int32_t cad_abi_accepts(std::uint32_t pluginAbiMajor, std::uint32_t pluginMinHostMinor,
                        const char** outReason) {
    const auto reason = [outReason](const char* text) {
        if (outReason != nullptr) *outReason = text;
    };

    // Backward. Every major this host has ever served, it still serves -- ADR 0011 -- so the
    // check is "is this one of ours", not "is this the current one". Today there is exactly one.
    // When ABI 2 exists, 1 stays acceptable here and is served through a shim; that is the whole
    // meaning of "majors are served, not replaced".
    if (pluginAbiMajor == 0 || pluginAbiMajor > CAD_ABI_VERSION_MAJOR) {
        reason("This plugin was built for a newer generation of vCAD's plugin interface.");
        return 0;
    }

    // Forward. Refusal, not compatibility: a plugin needing calls this host never populated would
    // dereference a null function pointer inside third-party code.
    if (pluginMinHostMinor > CAD_ABI_VERSION_MINOR) {
        reason("This plugin needs a newer version of vCAD than the one you are running.");
        return 0;
    }

    reason("");
    return 1;
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
    const auto s = lookup(handle);
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
            out->needs_plugin = r.needsPlugin;
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

CadStatus cad_sketch_create_on_face(CadSession handle, const CadElementId* face, CadSketch* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        if (face == nullptr || face->text == nullptr || face->text_len == 0) {
            // The TEXT is required, not the digest. A digest is only meaningful inside the process
            // that produced it; the text is what survives a save and reload, and a placement that
            // stopped working after reopening the file would be worse than one that never worked.
            return fail(s, CAD_ERR_INVALID_INPUT,
                        "A sketch on a face needs that face's name.");
        }

        cad::sketch::SketchPlane placement;
        placement.kind = cad::sketch::SketchPlane::Kind::Face;
        placement.face.assign(face->text, face->text_len);
        *out = registerSketch(s, cad::sketch::Sketch(std::move(placement)));
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
            // Refused: a non-finite coordinate. Reported as an error rather than passed back as
            // an id, because an id that indexes no geometry is the kind of value that gets stored
            // and dereferenced three operations later.
            if (id == cad::sketch::kInvalidGeo) {
                return fail(s, CAD_ERR_INVALID_INPUT,
                            "Sketch geometry needs finite coordinates.");
            }
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
            // Refused: a non-finite coordinate. Reported as an error rather than passed back as
            // an id, because an id that indexes no geometry is the kind of value that gets stored
            // and dereferenced three operations later.
            if (id == cad::sketch::kInvalidGeo) {
                return fail(s, CAD_ERR_INVALID_INPUT,
                            "Sketch geometry needs finite coordinates.");
            }
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
            // Refused: a non-finite coordinate. See the note on addLine above.
            if (id == cad::sketch::kInvalidGeo) {
                return fail(s, CAD_ERR_INVALID_INPUT,
                            "Sketch geometry needs finite coordinates.");
            }
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
                case CAD_CON_TANGENT:       index = sketch.tangent(a, ap, b, bp); break;
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

CadStatus cad_sketch_export_dxf(CadSession handle, CadSketch sk, const char* path, double scale) {
    return withSession(handle, [&](Session& s) {
        return withSketch(s, sk, [&](cad::sketch::Sketch& sketch) {
            if (path == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing path.");
            cad::io::DxfExportOptions options;
            if (scale > 0.0) options.scale = scale;
            auto r = cad::io::exportDxf(sketch, path, options);
            if (!r) {
                s.lastError = r.error().message;
                return toStatus(r.error().code);
            }
            return CAD_OK;
        });
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

CadStatus cad_rollback_set(CadSession handle, CadObject id) {
    return withSession(handle, [&](Session& s) {
        std::optional<ObjectId> marker;
        if (id != 0) {
            if (!s.doc().contains(ObjectId{id})) {
                return fail(s, CAD_ERR_BAD_HANDLE, "No such object to roll back to.");
            }
            marker = ObjectId{id};
        }
        s.history.replaceCurrent(s.doc().withRollbackAfter(marker));
        return CAD_OK;
    });
}

CadStatus cad_rollback_get(CadSession handle, CadObject* out) {
    return withSession(handle, [&](Session& s) {
        if (out == nullptr) return fail(s, CAD_ERR_INVALID_INPUT, "Missing output pointer.");
        const auto marker = s.doc().rollbackAfter();
        *out = marker.has_value() ? marker->value : 0;
        return CAD_OK;
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
    const auto s = lookup(handle);
    // Deliberately does NOT clear the scratch: this reads what cad_import_probe just left.
    return s ? s->scratch.c_str() : "";
}

static const char* joinExtensions(CadSession handle, bool writable) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto s = lookup(handle);
    if (!s) return "";
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
