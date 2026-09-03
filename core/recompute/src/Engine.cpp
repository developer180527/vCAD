#include "cad/recompute/Engine.h"
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <filesystem>

#include <algorithm>
#include <deque>
#include <set>

namespace cad::recompute {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= kFnvPrime;
    }
}

/// The reason a blocked feature carries, naming the upstream feature responsible.
///
/// Blocked objects used to get `withState(Blocked)` and nothing else, so `error()` stayed empty
/// and the shell showed a red feature with no tooltip. To a user that is indistinguishable from a
/// crash — the torture suite calls it failing illegibly, and it is the difference between "go fix
/// Sketch3" and "something is wrong somewhere".
///
/// The NAME matters more than the category. A deleted input has no object left to name, so it
/// falls back to the id, which is still an anchor a developer can search for.
kernel::Error blockedBy(const Document& doc, ObjectId input, const char* what) {
    const auto source = doc.find(input);
    const std::string who = source && !source->label().empty()
                                ? source->label()
                                : "feature #" + std::to_string(input.value);
    return kernel::Error{kernel::ErrorCode::InvalidInput,
                         "Blocked: " + who + " " + what + ".",
                         "upstream object " + std::to_string(input.value)};
}

/// Content digest of a file, or 0 if it cannot be read.
///
/// A missing or unreadable file digests to 0 DELIBERATELY, and every unreadable file shares that
/// value. The alternative — refusing to build a key — would mean a broken path could not even be
/// recomputed to produce its error, so the feature would be stuck rather than failing legibly.
///
/// Memoised on (size, mtime), because this runs on every key computation and a STEP file can be
/// hundreds of megabytes. Note carefully: mtime is used ONLY to decide whether to re-read the
/// file. It is never mixed into the key. That distinction is the whole design — mtime differs
/// between two machines holding identical bytes, so a key containing it would defeat the shared
/// DDC tier for every imported file.
std::uint64_t fileDigest(const std::string& path) {
    namespace fs = std::filesystem;

    struct Memo {
        std::uintmax_t size = 0;
        std::int64_t mtime = 0;
        std::uint64_t digest = 0;
    };
    static std::mutex mutex;
    static std::unordered_map<std::string, Memo> memo;

    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return 0;
    const auto written = fs::last_write_time(path, ec);
    if (ec) return 0;
    const auto mtime = written.time_since_epoch().count();

    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = memo.find(path);
        if (it != memo.end() && it->second.size == size && it->second.mtime == mtime) {
            return it->second.digest;
        }
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::uint64_t h = kFnvOffset;
    char buffer[64 * 1024];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
        const auto got = static_cast<std::size_t>(in.gcount());
        for (std::size_t i = 0; i < got; ++i) {
            mix(h, static_cast<std::uint64_t>(static_cast<unsigned char>(buffer[i])));
        }
    }
    // Length too: two files where one is a prefix of the other must not collide.
    mix(h, static_cast<std::uint64_t>(size));

    {
        const std::lock_guard<std::mutex> lock(mutex);
        memo[path] = Memo{size, static_cast<std::int64_t>(mtime), h};
    }
    return h;
}

}  // namespace

// --- FeatureRegistry ---------------------------------------------------------------------

void FeatureRegistry::add(FeatureType t) {
    const std::string name = t.name;
    types_[name] = std::move(t);
}

const FeatureType* FeatureRegistry::find(std::string_view name) const {
    const auto it = types_.find(std::string(name));
    return it == types_.end() ? nullptr : &it->second;
}

std::vector<std::string> FeatureRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(types_.size());
    for (const auto& [n, _] : types_) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

// --- MemoryCache -------------------------------------------------------------------------

MemoryCache::MemoryCache(std::size_t maxEntries) : maxEntries_(maxEntries) {}

std::optional<Output> MemoryCache::get(std::uint64_t key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++misses_;
        return std::nullopt;
    }
    ++hits_;
    return it->second;
}

void MemoryCache::put(std::uint64_t key, const Output& value) {
    if (entries_.count(key) != 0) return;
    if (entries_.size() >= maxEntries_ && !order_.empty()) {
        // Insertion-order eviction. assetlib's disk tier evicts LRU by mtime touched on
        // fetch; the L0 tier will need the same, plus liveness pinning against the open
        // document and iOS memory-pressure notifications (ADR 0004). Not yet.
        entries_.erase(order_.front());
        order_.erase(order_.begin());
    }
    entries_.emplace(key, value);
    order_.push_back(key);
}

// --- Engine ------------------------------------------------------------------------------

Engine::Engine(const FeatureRegistry& registry, Cache& cache)
    : registry_(registry), cache_(cache) {}

kernel::Result<std::uint64_t> Engine::cacheKeyOf(const Document& doc,
                                                 const document::ObjectData& object,
                                                 const FeatureRegistry& registry) {
    const FeatureType* type = registry.find(object.type());
    if (type == nullptr) {
        return kernel::Error{kernel::ErrorCode::Unsupported,
                             "Unknown feature type '" + object.type() + "'.",
                             "no such type in the registry; a plugin may be missing"};
    }

    std::uint64_t h = kFnvOffset;
    for (char c : object.type()) mix(h, static_cast<std::uint64_t>(c));

    // The feature VERSION. Forgetting this is the classic cooking-pipeline bug: change the
    // implementation, keep serving results computed by the old one.
    mix(h, type->version);

    // The object's own id, because it is `ComputeContext::namingSerial` and therefore an INPUT to
    // the output -- it is stamped into every element name the compute produces. A key that omits an
    // input is the same bug as the version above, arriving through a quieter door.
    //
    // What it cost while it was missing: two Box features with the same dx/dy/dz hashed alike, so
    // the second was served the FIRST one's Output -- its shape and its ElementMap. Measured: all
    // 26 element names identical, both carrying serial 4 while the object ids were 4 and 5. Every
    // boolean between two identical bodies then failed with "Two pieces of this shape ended up
    // with the same identity", which reads as a naming limitation and is not one. Worse than the
    // refusal is what the refusal was hiding: a reference to one body's face genuinely resolved in
    // the other, and only ElementMap::collisions made that visible instead of wrong.
    //
    // This does give up sharing one cache entry between two identical parts, which the note below
    // rightly prizes for DEPENDENCIES. The difference is that a dependency is consumed by content
    // -- what it looks like is all that matters -- while the serial is consumed by identity, and
    // two objects that must be distinguishable cannot be allowed to produce the same names.
    mix(h, object.id().value);

    for (const auto& p : object.properties()) {
        if (p.cosmetic) continue;   // colour and labels must not invalidate geometry
        for (char c : p.name) mix(h, static_cast<std::uint64_t>(c));

        // Property::expression is deliberately NOT mixed in. This key answers "would recomputing
        // produce the same geometry", and geometry is built from the VALUE -- 80 is 80 whether a
        // person typed it or `width * 2` produced it. Including the text would give two identical
        // parts different cache entries and re-cut every feature the moment a user tidied a
        // formula. Document::digest() does include it, because "has this file changed" is a
        // different question with a different answer.

        // An ObjectId property contributes its target's CACHE KEY, not the id. This is the
        // assetlib rule that a dependency is recorded by content rather than identity: two
        // different upstream objects that produce identical geometry share downstream cache
        // entries, and renumbering an object does not invalidate anything.
        if (const auto* ref = std::get_if<document::ObjectId>(&p.value)) {
            const auto target = doc.find(*ref);
            if (!target) {
                return kernel::Error{kernel::ErrorCode::InvalidInput,
                                     "This feature refers to something that no longer exists."};
            }
            mix(h, ref->value);
            mix(h, target->cacheKey());
        } else if (const auto* list =
                       std::get_if<std::vector<document::ObjectId>>(&p.value)) {
            for (const auto& ref2 : *list) {
                const auto target = doc.find(ref2);
                if (!target) {
                    return kernel::Error{kernel::ErrorCode::InvalidInput,
                                         "This feature refers to something that no longer exists."};
                }
                mix(h, ref2.value);
                mix(h, target->cacheKey());
            }
        } else {
            mix(h, document::digestOf(p.value));
        }
    }

    // External inputs, by CONTENT. Without this the key covers the path string and nothing else,
    // so editing an imported file and recomputing serves the shape cached from the old contents.
    if (type->externalInputs) {
        for (const std::string& path : type->externalInputs(object)) {
            for (char c : path) mix(h, static_cast<std::uint64_t>(c));
            mix(h, fileDigest(path));
        }
    }
    return h;
}

Document Engine::invalidate(const Document& doc, ObjectId id) {
    // Breadth-first over dependents. Marking only the object itself is the bug that makes a
    // model look stale until you touch something else.
    Document out = doc;
    std::deque<ObjectId> queue{id};
    std::set<ObjectId> seen{id};

    while (!queue.empty()) {
        const ObjectId current = queue.front();
        queue.pop_front();
        if (const auto obj = out.find(current)) {
            out = out.replace(std::make_shared<const document::ObjectData>(
                obj->withState(document::ObjectState::Dirty)));
        }
        for (const ObjectId dep : out.dependents(current)) {
            if (seen.insert(dep).second) queue.push_back(dep);
        }
    }
    return out;
}

kernel::Result<std::pair<Document, RecomputeReport>> Engine::recompute(
    const Document& input) const {

    auto ordered = input.topologicalOrder();
    if (!ordered) return ordered.error();

    Document doc = input;
    RecomputeReport report;
    std::set<ObjectId> poisoned;   // failed, or downstream of a failure

    for (const ObjectId id : ordered.value()) {
        const auto object = doc.find(id);
        if (!object) continue;

        // 1. Blocked by an upstream failure? Mark and continue — do NOT abort the pass.
        //    A user with one broken feature in a fifty-feature part must still see the
        // 0. Suspended by the rollback marker.
        //
        // Checked FIRST, before blocking and before the cache. A suspended feature is not failed and
        // not blocked -- it is deliberately not part of the model right now -- so it gets Dirty with
        // its output dropped. Dirty, because that is exactly true: it will need computing the moment
        // the marker moves past it.
        //
        // Its output MUST be dropped rather than left stale. A suspended extrude that kept its solid
        // would still be tessellated and drawn, so rolling back would suspend a feature and change
        // nothing on screen.
        //
        // Not counted as failed or blocked in the report: nothing is wrong, and a status bar saying
        // "3 features could not be built" after a deliberate rollback would be alarming nonsense.
        if (doc.isRolledBack(id)) {
            ++report.skipped;
            poisoned.insert(id);   // so dependents below are Blocked, not attempted
            doc = doc.replace(std::make_shared<const document::ObjectData>(
                object->withState(document::ObjectState::Dirty).withoutOutput()));
            continue;
        }

        //    other forty-nine.
        ObjectId blocker{};
        const char* reason = nullptr;
        for (const ObjectId in : object->inputs()) {
            if (poisoned.count(in) != 0) {
                blocker = in;
                reason = "could not be computed";
                break;
            }
            if (!doc.contains(in)) {
                blocker = in;
                reason = "was deleted";
                break;
            }
        }
        if (reason != nullptr) {
            poisoned.insert(id);
            ++report.blocked;
            doc = doc.replace(std::make_shared<const document::ObjectData>(
                object->withBlocked(blockedBy(doc, blocker, reason)).withoutOutput()));
            continue;
        }

        // 2. Already clean with output: nothing to do.
        if (object->state() == document::ObjectState::Clean && object->output() != nullptr) {
            ++report.skipped;
            continue;
        }

        // 2A. The feature's TYPE is not registered: its plugin is not installed.
        //
        // Distinguished from every other failure, and PLUGIN_CONTRACT.md 4A is the reason. The
        // document is not broken — it is complete and correct, and this machine is missing
        // software. Reporting it as Failed makes a user conclude a colleague's file is corrupt,
        // and the next thing they do is save over it.
        //
        // The object is left otherwise UNTOUCHED: its type, its parameters and its last output
        // all stay, so saving from this session writes every byte back. That is the whole
        // guarantee — a document must outlive the plugin that made it.
        if (registry_.find(object->type()) == nullptr) {
            poisoned.insert(id);   // dependents block, with a reason naming this feature
            ++report.needsPlugin;
            doc = doc.replace(std::make_shared<const document::ObjectData>(
                object->withNeedsPlugin(kernel::Error{
                    kernel::ErrorCode::Unsupported,
                    "'" + object->label() + "' needs a plugin that is not installed (" +
                        object->type() + "). Its settings are unchanged and will work again "
                        "once the plugin is available.",
                    "feature type not in the registry"})));
            continue;
        }

        auto key = cacheKeyOf(doc, *object, registry_);
        if (!key) {
            poisoned.insert(id);
            ++report.failed;
            report.failedObjects.push_back(id);
            doc = doc.replace(std::make_shared<const document::ObjectData>(
                object->withError(key.error()).withoutOutput()));
            continue;
        }

        // 3. Cache.
        if (auto cached = cache_.get(key.value())) {
            ++report.cached;
            doc = doc.replace(std::make_shared<const document::ObjectData>(
                object->withOutput(std::move(*cached), key.value())));
            continue;
        }

        // 4. Compute.
        const FeatureType* type = registry_.find(object->type());
        ComputeContext ctx{{}, *object, static_cast<std::uint32_t>(id.value)};
        ObjectId missing{};
        bool inputsOk = true;
        for (const ObjectId in : object->inputs()) {
            const auto source = doc.find(in);
            if (!source || source->output() == nullptr) {
                missing = in;
                inputsOk = false;
                break;
            }
            ctx.inputs.push_back(source->output());
        }
        if (!inputsOk) {
            poisoned.insert(id);
            ++report.blocked;
            doc = doc.replace(std::make_shared<const document::ObjectData>(
                object->withBlocked(blockedBy(doc, missing, "produced no shape"))
                    .withoutOutput()));
            continue;
        }

        auto result = type->compute(ctx);
        if (!result) {
            poisoned.insert(id);
            ++report.failed;
            report.failedObjects.push_back(id);
            doc = doc.replace(std::make_shared<const document::ObjectData>(
                object->withError(result.error()).withoutOutput()));
            continue;
        }

        ++report.computed;
        cache_.put(key.value(), result.value());
        doc = doc.replace(std::make_shared<const document::ObjectData>(
            object->withOutput(std::move(result).value(), key.value())));
    }

    return std::pair{doc, report};
}

}  // namespace cad::recompute
