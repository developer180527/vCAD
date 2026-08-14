#pragma once

#include "cad/document/Document.h"
#include "cad/kernel/Result.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cad::recompute {

using document::Document;
using document::ObjectId;
using document::ObjectPtr;
using document::Output;

/// What a feature's compute function is handed.
struct ComputeContext {
    /// Inputs already computed, in the order the object's ObjectId properties declare them.
    /// Guaranteed non-null and successful — a failed input means we never got here.
    std::vector<const Output*> inputs;
    const document::ObjectData& object;

    /// Feature serial handed to NamingContext. Derived from the object id so it is stable
    /// across rebuilds and across processes — using a running counter here would make
    /// element names depend on evaluation order, which is exactly the class of bug that
    /// destroys cache hit rates.
    std::uint32_t namingSerial;
};

using ComputeFn = std::function<kernel::Result<Output>(const ComputeContext&)>;

/// Paths this feature will READ that are not part of the document.
///
/// The cache key is built from the document, so anything a feature reads from outside it is
/// invisible to the key — and the cached result is then served after the external thing has
/// changed. Import had exactly that bug: its key covered the `path` STRING, so editing the
/// referenced STEP file on disk and recomputing returned the previously cached shape. Silent
/// wrong geometry, which is the worst kind.
///
/// Declaring the paths here lets the engine fold their CONTENT digests into the key, so a
/// changed file invalidates exactly the features that read it. This mirrors the plugin contract's
/// declare_external_input (docs/design/PLUGIN_CONTRACT.md §4B) deliberately: built-ins and
/// plugins are held to the same rule, because the cache cannot tell them apart.
using ExternalInputsFn = std::function<std::vector<std::string>(const document::ObjectData&)>;

/// A feature type: the unit a plugin registers and the unit the cache keys on.
struct FeatureType {
    std::string name;
    /// Bumped when the implementation changes in a way that alters output for identical
    /// inputs. This is what makes a cached result from an older build invalid, and it is
    /// the single most commonly forgotten thing in a cooking pipeline — assetlib calls the
    /// same field the cooker version.
    std::uint32_t version = 1;
    ComputeFn compute;

    /// Optional. Null means "this feature reads nothing outside the document", which is the
    /// correct answer for every purely parametric feature.
    ExternalInputsFn externalInputs;
};

/// Registry of feature types. Plugins add to this; nothing else knows what a "Pad" is.
class FeatureRegistry {
public:
    void add(FeatureType);
    [[nodiscard]] const FeatureType* find(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> names() const;

    /// The built-in set, so tests and the app share exactly one definition of "Box".
    static FeatureRegistry builtins();

private:
    std::unordered_map<std::string, FeatureType> types_;
};

/// Cache of computed outputs, keyed by content.
///
/// The interface is deliberately assetlib-shaped (see ADR 0004): a pure content key in, an
/// optional value out. `MemoryCache` below is the L0 tier; the assetlib DDC becomes the L1
/// disk tier and the shared team tier behind the same interface.
class Cache {
public:
    virtual ~Cache() = default;
    virtual std::optional<Output> get(std::uint64_t key) = 0;
    virtual void put(std::uint64_t key, const Output&) = 0;

    /// Reported so tests can assert that a change recomputed exactly the affected subgraph
    /// and nothing more. Hit rate is a correctness property here, not a performance nicety:
    /// a spurious miss means we recomputed something whose inputs did not change, which
    /// means the key is wrong.
    virtual std::size_t hits() const = 0;
    virtual std::size_t misses() const = 0;
    virtual void resetStats() = 0;
};

/// In-memory L0 tier: hash -> live Shape, no serialisation at all. This is the tier that
/// has to answer within the interactive budget while the user drags a dimension.
class MemoryCache : public Cache {
public:
    explicit MemoryCache(std::size_t maxEntries = 4096);
    std::optional<Output> get(std::uint64_t key) override;
    void put(std::uint64_t key, const Output&) override;
    std::size_t hits() const override { return hits_; }
    std::size_t misses() const override { return misses_; }
    void resetStats() override { hits_ = misses_ = 0; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    std::unordered_map<std::uint64_t, Output> entries_;
    std::vector<std::uint64_t> order_;   ///< insertion order, for eviction
    std::size_t maxEntries_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

/// Outcome of one recompute pass.
struct RecomputeReport {
    std::size_t computed = 0;   ///< actually ran the feature
    std::size_t cached = 0;     ///< served from cache
    std::size_t skipped = 0;    ///< already clean
    std::size_t failed = 0;
    std::size_t blocked = 0;
    std::vector<ObjectId> failedObjects;

    [[nodiscard]] bool allSucceeded() const noexcept { return failed == 0 && blocked == 0; }
};

/// Evaluates the document DAG.
///
/// Behaviour that matters and is easy to get wrong:
///
///   * A failed object does NOT abort the pass. It is marked Failed, everything downstream
///     is marked Blocked, and every independent branch still computes. A user with one
///     broken feature in a fifty-feature part must still see the other forty-nine.
///   * The cache key covers feature type, feature VERSION, every non-cosmetic property, and
///     the cache keys of the inputs. Two documents that would produce identical geometry
///     therefore share cache entries even if they were built by different routes.
///   * Recompute is deterministic: evaluation follows topological order broken by ObjectId,
///     never by container iteration order.
class Engine {
public:
    Engine(const FeatureRegistry& registry, Cache& cache);

    /// Recomputes everything not already clean. Returns the updated document.
    kernel::Result<std::pair<Document, RecomputeReport>> recompute(const Document&) const;

    /// Marks an object and everything downstream of it dirty. This is what an edit calls.
    [[nodiscard]] static Document invalidate(const Document&, ObjectId);

    /// The content key an object would have. Exposed for tests and diagnostics.
    [[nodiscard]] static kernel::Result<std::uint64_t> cacheKeyOf(
        const Document&, const document::ObjectData&, const FeatureRegistry&);

private:
    const FeatureRegistry& registry_;
    Cache& cache_;
};

}  // namespace cad::recompute
