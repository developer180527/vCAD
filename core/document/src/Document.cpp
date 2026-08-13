#include "cad/document/Document.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <unordered_set>

namespace cad::document {
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

const char* toString(ObjectState s) noexcept {
    switch (s) {
        case ObjectState::Clean:   return "Clean";
        case ObjectState::Dirty:   return "Dirty";
        case ObjectState::Failed:  return "Failed";
        case ObjectState::Blocked: return "Blocked";
    }
    return "Unknown";
}

// --- ObjectData ------------------------------------------------------------------------

ObjectData::ObjectData(ObjectId id, std::string type)
    : id_(id), type_(std::move(type)), label_(type_) {}

const PropertyValue* ObjectData::find(std::string_view name) const {
    for (const auto& p : properties_) {
        if (p.name == name) return &p.value;
    }
    return nullptr;
}

std::vector<ObjectId> ObjectData::inputs() const {
    // Derived from the properties rather than stored separately. A separately-stored edge
    // list is one more thing that can disagree with the parameters, and when it disagrees
    // the recompute order is wrong in a way that is very hard to see.
    std::vector<ObjectId> out;
    for (const auto& p : properties_) {
        if (const auto* id = std::get_if<ObjectId>(&p.value)) {
            if (!id->isNull()) out.push_back(*id);
        } else if (const auto* list = std::get_if<std::vector<ObjectId>>(&p.value)) {
            for (const auto& i : *list) {
                if (!i.isNull()) out.push_back(i);
            }
        }
    }
    return out;
}

ObjectData ObjectData::withProperty(std::string name, PropertyValue value,
                                    bool cosmetic) const {
    ObjectData copy = *this;
    for (auto& p : copy.properties_) {
        if (p.name == name) {
            p.value = std::move(value);
            p.cosmetic = cosmetic;
            return copy;
        }
    }
    copy.properties_.push_back(Property{std::move(name), std::move(value), cosmetic});
    // Keep properties sorted by name so the cache key does not depend on insertion order.
    std::sort(copy.properties_.begin(), copy.properties_.end(),
              [](const Property& a, const Property& b) { return a.name < b.name; });
    return copy;
}

ObjectData ObjectData::withLabel(std::string l) const {
    ObjectData copy = *this;
    copy.label_ = std::move(l);
    return copy;
}

ObjectData ObjectData::withState(ObjectState s) const {
    ObjectData copy = *this;
    copy.state_ = s;
    if (s != ObjectState::Failed && s != ObjectState::Blocked) copy.error_ = kernel::Error{};
    return copy;
}

ObjectData ObjectData::withError(kernel::Error e) const {
    ObjectData copy = *this;
    copy.error_ = std::move(e);
    copy.state_ = ObjectState::Failed;
    return copy;
}

ObjectData ObjectData::withOutput(Output o, std::uint64_t key) const {
    ObjectData copy = *this;
    copy.output_ = std::move(o);
    copy.hasOutput_ = true;
    copy.cacheKey_ = key;
    copy.state_ = ObjectState::Clean;
    copy.error_ = kernel::Error{};
    return copy;
}

ObjectData ObjectData::withoutOutput() const {
    ObjectData copy = *this;
    copy.output_ = Output{};
    copy.hasOutput_ = false;
    copy.cacheKey_ = 0;
    return copy;
}

// --- Document --------------------------------------------------------------------------

struct Document::Impl {
    // std::map, not unordered_map: iteration order is the id order, so every traversal is
    // deterministic without an explicit sort. Determinism here feeds straight into the
    // cache keys.
    std::map<ObjectId, ObjectPtr> objects;
    std::uint64_t nextId = 1;
};

Document::Document() : impl_(std::make_shared<Impl>()) {}
Document::Document(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

std::size_t Document::size() const noexcept { return impl_->objects.size(); }

std::vector<ObjectId> Document::ids() const {
    std::vector<ObjectId> out;
    out.reserve(impl_->objects.size());
    for (const auto& [id, _] : impl_->objects) out.push_back(id);
    return out;
}

ObjectPtr Document::find(ObjectId id) const {
    const auto it = impl_->objects.find(id);
    return it == impl_->objects.end() ? nullptr : it->second;
}

bool Document::contains(ObjectId id) const { return impl_->objects.count(id) != 0; }

std::pair<Document, ObjectId> Document::add(std::string type) const {
    auto next = std::make_shared<Impl>(*impl_);   // copies the pointer map, not the objects
    const ObjectId id{next->nextId++};
    next->objects[id] = std::make_shared<const ObjectData>(id, std::move(type));
    return {Document(next), id};
}

Document Document::replace(ObjectPtr object) const {
    if (!object) return *this;
    auto next = std::make_shared<Impl>(*impl_);
    next->objects[object->id()] = std::move(object);
    return Document(next);
}

Document Document::insert(ObjectPtr object) const {
    if (!object) return *this;
    auto next = std::make_shared<Impl>(*impl_);
    const ObjectId id = object->id();
    next->objects[id] = std::move(object);
    next->nextId = std::max(next->nextId, id.value + 1);
    return Document(next);
}

std::uint64_t Document::nextId() const noexcept { return impl_->nextId; }

Document Document::withNextId(std::uint64_t value) const {
    auto next = std::make_shared<Impl>(*impl_);
    // Only ever forward. A loader that passed a stale value must not be able to walk the
    // allocator backwards into ids that are already in use.
    next->nextId = std::max(next->nextId, value);
    return Document(next);
}

Document Document::remove(ObjectId id) const {
    auto next = std::make_shared<Impl>(*impl_);
    next->objects.erase(id);
    return Document(next);
}

std::vector<ObjectId> Document::dependents(ObjectId id) const {
    std::vector<ObjectId> out;
    for (const auto& [other, obj] : impl_->objects) {
        const auto in = obj->inputs();
        if (std::find(in.begin(), in.end(), id) != in.end()) out.push_back(other);
    }
    return out;
}

kernel::Result<std::vector<ObjectId>> Document::topologicalOrder() const {
    enum class Mark : std::uint8_t { None, InProgress, Done };
    std::map<ObjectId, Mark> marks;
    std::vector<ObjectId> order;
    std::vector<ObjectId> stack;   // for a legible cycle message

    // Explicit stack rather than recursion: a deep feature tree is normal, and a stack
    // overflow on a large customer document is not an acceptable failure mode.
    struct Frame {
        ObjectId id;
        std::size_t nextInput = 0;
        std::vector<ObjectId> inputs;
    };

    for (const auto& [rootId, _] : impl_->objects) {
        if (marks[rootId] == Mark::Done) continue;

        std::vector<Frame> frames;
        frames.push_back({rootId, 0, find(rootId)->inputs()});
        marks[rootId] = Mark::InProgress;
        stack.push_back(rootId);

        while (!frames.empty()) {
            Frame& f = frames.back();
            if (f.nextInput < f.inputs.size()) {
                const ObjectId child = f.inputs[f.nextInput++];
                if (!contains(child)) continue;   // dangling; the engine reports it
                const Mark m = marks[child];
                if (m == Mark::Done) continue;
                if (m == Mark::InProgress) {
                    std::ostringstream os;
                    os << "These features depend on each other in a loop: ";
                    bool first = true;
                    for (auto it = std::find(stack.begin(), stack.end(), child);
                         it != stack.end(); ++it) {
                        if (!first) os << " -> ";
                        first = false;
                        const auto obj = find(*it);
                        os << (obj ? obj->label() : "?");
                    }
                    return kernel::Error{kernel::ErrorCode::InvalidInput, os.str()};
                }
                marks[child] = Mark::InProgress;
                stack.push_back(child);
                frames.push_back({child, 0, find(child)->inputs()});
                continue;
            }
            marks[f.id] = Mark::Done;
            order.push_back(f.id);
            stack.pop_back();
            frames.pop_back();
        }
    }
    return order;
}

std::uint64_t Document::digest() const {
    std::uint64_t h = kFnvOffset;
    for (const auto& [id, obj] : impl_->objects) {
        mix(h, id.value);
        for (char c : obj->type()) mix(h, static_cast<std::uint64_t>(c));
        for (const auto& p : obj->properties()) {
            for (char c : p.name) mix(h, static_cast<std::uint64_t>(c));
            mix(h, digestOf(p.value));
        }
    }
    return h;
}

// --- History ---------------------------------------------------------------------------

History::History(Document initial, std::size_t depth)
    : current_(std::move(initial)), currentLabel_("Initial"), depth_(depth) {}

void History::commit(Document next, std::string label) {
    past_.push_back({current_, currentLabel_});
    if (past_.size() > depth_) past_.erase(past_.begin());
    future_.clear();          // a new edit discards the redo branch, as everywhere else
    current_ = std::move(next);
    currentLabel_ = std::move(label);
}

void History::replaceCurrent(Document next) { current_ = std::move(next); }

bool History::undo() {
    if (past_.empty()) return false;
    future_.push_back({current_, currentLabel_});
    current_ = past_.back().doc;
    currentLabel_ = past_.back().label;
    past_.pop_back();
    return true;
}

bool History::redo() {
    if (future_.empty()) return false;
    past_.push_back({current_, currentLabel_});
    current_ = future_.back().doc;
    currentLabel_ = future_.back().label;
    future_.pop_back();
    return true;
}

std::string History::undoLabel() const {
    return past_.empty() ? std::string{} : currentLabel_;
}

std::string History::redoLabel() const {
    return future_.empty() ? std::string{} : future_.back().label;
}

}  // namespace cad::document
