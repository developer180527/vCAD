#include "cad/recompute/Engine.h"

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

    for (const auto& p : object.properties()) {
        if (p.cosmetic) continue;   // colour and labels must not invalidate geometry
        for (char c : p.name) mix(h, static_cast<std::uint64_t>(c));

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
            mix(h, target->cacheKey());
        } else if (const auto* list =
                       std::get_if<std::vector<document::ObjectId>>(&p.value)) {
            for (const auto& ref2 : *list) {
                const auto target = doc.find(ref2);
                if (!target) {
                    return kernel::Error{kernel::ErrorCode::InvalidInput,
                                         "This feature refers to something that no longer exists."};
                }
                mix(h, target->cacheKey());
            }
        } else {
            mix(h, document::digestOf(p.value));
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
        bool blocked = false;
        for (const ObjectId in : object->inputs()) {
            if (poisoned.count(in) != 0) { blocked = true; break; }
            if (!doc.contains(in)) { blocked = true; break; }
        }
        if (blocked) {
            poisoned.insert(id);
            ++report.blocked;
            doc = doc.replace(std::make_shared<const document::ObjectData>(
                object->withState(document::ObjectState::Blocked).withoutOutput()));
            continue;
        }

        // 2. Already clean with output: nothing to do.
        if (object->state() == document::ObjectState::Clean && object->output() != nullptr) {
            ++report.skipped;
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
        bool inputsOk = true;
        for (const ObjectId in : object->inputs()) {
            const auto source = doc.find(in);
            if (!source || source->output() == nullptr) { inputsOk = false; break; }
            ctx.inputs.push_back(source->output());
        }
        if (!inputsOk) {
            poisoned.insert(id);
            ++report.blocked;
            doc = doc.replace(std::make_shared<const document::ObjectData>(
                object->withState(document::ObjectState::Blocked).withoutOutput()));
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
