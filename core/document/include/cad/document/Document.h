#pragma once

#include "cad/document/PropertyValue.h"
#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"
#include "cad/naming/ElementMap.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cad::document {

/// Where an object stands after the last recompute.
enum class ObjectState : std::uint8_t {
    Clean,      ///< output is current
    Dirty,      ///< inputs or properties changed; needs recompute
    Failed,     ///< its own compute failed; `error` says why
    Blocked,    ///< an upstream object failed, so this one could not even be attempted
};

const char* toString(ObjectState) noexcept;

/// What a feature produced. Empty until the first successful recompute.
struct Output {
    kernel::Shape shape;
    naming::ElementMap map;
};

/// One node of the document DAG.
///
/// Immutable once constructed. Every edit produces a NEW ObjectData and a new Document
/// version that shares the untouched nodes by pointer — see Document below for why.
class ObjectData {
public:
    ObjectData(ObjectId id, std::string type);

    [[nodiscard]] ObjectId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& type() const noexcept { return type_; }
    [[nodiscard]] const std::string& label() const noexcept { return label_; }

    [[nodiscard]] const std::vector<Property>& properties() const noexcept { return properties_; }
    [[nodiscard]] const PropertyValue* find(std::string_view name) const;

    /// Objects this one consumes. Derived from ObjectId/ObjectList properties, so the DAG
    /// can never drift out of sync with the parameters that define it.
    [[nodiscard]] std::vector<ObjectId> inputs() const;

    [[nodiscard]] ObjectState state() const noexcept { return state_; }
    [[nodiscard]] const kernel::Error& error() const noexcept { return error_; }
    [[nodiscard]] const Output* output() const noexcept {
        return hasOutput_ ? &output_ : nullptr;
    }

    /// Cache key of the last successful compute. Zero if never computed.
    [[nodiscard]] std::uint64_t cacheKey() const noexcept { return cacheKey_; }

    // --- builders: each returns a new value, leaving this one untouched ------------------
    [[nodiscard]] ObjectData withProperty(std::string name, PropertyValue value,
                                          bool cosmetic = false) const;
    [[nodiscard]] ObjectData withLabel(std::string) const;
    [[nodiscard]] ObjectData withState(ObjectState) const;
    [[nodiscard]] ObjectData withError(kernel::Error) const;
    [[nodiscard]] ObjectData withOutput(Output, std::uint64_t cacheKey) const;
    [[nodiscard]] ObjectData withoutOutput() const;

private:
    ObjectId id_;
    std::string type_;
    std::string label_;
    std::vector<Property> properties_;
    ObjectState state_ = ObjectState::Dirty;
    kernel::Error error_;
    Output output_;
    bool hasOutput_ = false;
    std::uint64_t cacheKey_ = 0;
};

using ObjectPtr = std::shared_ptr<const ObjectData>;

/// A persistent (immutable) document.
///
/// ADR 0003, decided here: every edit yields a NEW Document that shares every untouched node
/// with its predecessor by `shared_ptr`. Undo is therefore "keep the previous value" — no
/// inverse operations to write, no way for an undo to be subtly wrong, and no chokepoint to
/// forget to route a mutation through.
///
/// The cost is memory for the versions we retain, which is bounded by the undo depth and is
/// small because sharing is structural: editing one node in a 5000-object assembly copies
/// one node and 5000 pointers, not 5000 objects.
///
/// The payoff beyond undo: a Document value can be handed to a render thread or a worker
/// with no locking, because nobody can mutate it. That is what makes the threading model in
/// docs/decisions/0006 possible.
class Document {
public:
    Document();

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::vector<ObjectId> ids() const;          ///< sorted, deterministic
    [[nodiscard]] ObjectPtr find(ObjectId) const;
    [[nodiscard]] bool contains(ObjectId) const;

    /// Adds a new object of `type`. Returns the new document and the assigned id.
    [[nodiscard]] std::pair<Document, ObjectId> add(std::string type) const;

    /// Replaces an object wholesale. The caller builds the new ObjectData from the old one
    /// via the with*() builders.
    [[nodiscard]] Document replace(ObjectPtr) const;

    /// Inserts an object keeping the id it already carries. For LOADING a saved document.
    ///
    /// Object ids cannot be reassigned on load, and this is not a preference. The recompute
    /// engine uses the object's id as the naming serial (`Engine.cpp`, ComputeContext), so every
    /// ElementName produced by a feature has that feature's id baked into it. A saved fillet
    /// stores the element names of the edges it rounds; renumbering its base feature on load
    /// makes every one of those names resolve to nothing, and the fillet fails with
    /// ErrorCode::NamingLost. Renumbering is silent, plausible, and destroys the file.
    ///
    /// Raises the id allocator past the inserted id, so a later add() cannot collide with it.
    [[nodiscard]] Document insert(ObjectPtr) const;

    /// The id the next add() will assign.
    [[nodiscard]] std::uint64_t nextId() const noexcept;

    /// Restores the id allocator, which a loader must do explicitly.
    ///
    /// insert() only ever raises the mark past the ids present, which is not enough: if the
    /// highest-numbered object was DELETED before saving, reloading would hand its id to the next
    /// new object. Element names recorded against the old object would then resolve against the
    /// new one's geometry — a wrong reference rather than a lost one, which is worse. Ids are
    /// never reused within a session, and must not be reused across one either.
    [[nodiscard]] Document withNextId(std::uint64_t) const;

    /// Removes an object. Objects still referencing it are marked Failed on the next
    /// recompute rather than silently losing the reference.
    [[nodiscard]] Document remove(ObjectId) const;

    /// Objects that reference `id` directly.
    [[nodiscard]] std::vector<ObjectId> dependents(ObjectId) const;

    /// Dependency order. Error if the graph contains a cycle, naming the objects involved —
    /// a cycle is a user-visible modelling mistake, not an internal fault.
    [[nodiscard]] kernel::Result<std::vector<ObjectId>> topologicalOrder() const;

    // ── rollback ──────────────────────────────────────────────────────────────────────────
    //
    // The marker every history-based CAD application has: features after it are SUSPENDED, so you
    // can move it back up the tree, insert a feature in the middle, and move it down again.
    //
    // Identified by ObjectId, not by an index. Ids are handed out monotonically and never reused,
    // so id order IS creation order and the marker survives a feature above it being deleted --
    // where an index would silently start suspending a different feature.
    //
    // Deliberately NOT part of digest(): where the marker sits does not change what any feature IS,
    // so moving it must not invalidate a single cache key. Rolling forward then re-serves every
    // suspended feature from the cache instead of recomputing it, which is what makes dragging the
    // marker feel instant on a heavy model.

    /// Last feature that computes. Null means the whole tree computes.
    [[nodiscard]] std::optional<ObjectId> rollbackAfter() const noexcept;
    [[nodiscard]] Document withRollbackAfter(std::optional<ObjectId>) const;

    /// Whether `id` is suspended by the marker.
    [[nodiscard]] bool isRolledBack(ObjectId) const noexcept;

    /// Content digest of the whole document. Deterministic across processes.
    [[nodiscard]] std::uint64_t digest() const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;   ///< shared: Document copies are O(1)
    explicit Document(std::shared_ptr<const Impl>);
};

/// Undo/redo as a stack of whole document versions.
///
/// This is only sound because Document is persistent; with a mutable document it would be
/// ruinous. See the class comment above.
class History {
public:
    explicit History(Document initial, std::size_t depth = 200);

    [[nodiscard]] const Document& current() const noexcept { return current_; }

    /// Records `next` as a new version labelled for the UI's undo menu.
    void commit(Document next, std::string label);

    /// Updates the current version WITHOUT creating an undo step.
    ///
    /// For recompute results. A recompute is not something the user did, so it must not
    /// appear in the undo menu — otherwise undo walks backwards through recomputes instead
    /// of through edits, which is baffling to use.
    void replaceCurrent(Document next);

    bool undo();
    bool redo();

    [[nodiscard]] bool canUndo() const noexcept { return !past_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !future_.empty(); }
    [[nodiscard]] std::string undoLabel() const;
    [[nodiscard]] std::string redoLabel() const;

private:
    struct Entry {
        Document doc;
        std::string label;
    };
    Document current_;
    std::string currentLabel_;
    std::vector<Entry> past_;
    std::vector<Entry> future_;
    std::size_t depth_;
};

}  // namespace cad::document
