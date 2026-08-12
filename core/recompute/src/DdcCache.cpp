#include "cad/recompute/DdcCache.h"

#include "cad/io/Serialize.h"

#include <assetlib/ddc.h>

#include <cstdio>

namespace cad::recompute {
namespace {

/// The DDC is keyed by string, and the string must encode everything that could change the
/// bytes — including the encoding itself.
///
/// The serialisation version is in here rather than only in the blob header on purpose: a
/// format change makes every old key unreachable, so we never even read a blob we would
/// have to reject. Without it, every client would decode-and-discard on the first access
/// after a format bump.
std::string keyString(std::uint64_t contentKey) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "cadfeat.v%u.%016llx",
                  cad::io::kSerializationVersion,
                  static_cast<unsigned long long>(contentKey));
    return buf;
}

}  // namespace

struct DdcCache::Impl {
    assetlib::DdcStore store;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t unreadable = 0;

    Impl(std::filesystem::path local, std::filesystem::path shared)
        : store(std::move(local), std::move(shared)) {}
};

DdcCache::DdcCache(std::filesystem::path localRoot, std::filesystem::path sharedRoot)
    : impl_(std::make_unique<Impl>(std::move(localRoot), std::move(sharedRoot))) {}

DdcCache::~DdcCache() = default;

std::optional<Output> DdcCache::get(std::uint64_t key) {
    std::string bytes;
    if (!impl_->store.fetchBytes(keyString(key), bytes)) {
        ++impl_->misses;
        return std::nullopt;
    }

    auto decoded = cad::io::deserialize(bytes);
    if (!decoded) {
        // Found but unusable. Count it as a miss so the caller recomputes, and evict the
        // local copy so we do not pay for the same bad blob on every access. The shared
        // tier is deliberately left alone — other machines may be serving from it, and
        // deleting there is not ours to do.
        ++impl_->unreadable;
        ++impl_->misses;
        impl_->store.evictLocal(keyString(key));
        return std::nullopt;
    }

    ++impl_->hits;
    return std::move(decoded).value();
}

void DdcCache::put(std::uint64_t key, const Output& value) {
    auto bytes = cad::io::serialize(value);
    if (!bytes) return;   // not cacheable; recompute next time rather than fail now
    impl_->store.storeBytes(keyString(key), bytes.value());
}

bool DdcCache::get(std::uint64_t key, std::string& out) {
    // Distinct key namespace from cooked output: the same 64-bit key must never name both a
    // shape blob and a mesh blob.
    if (!impl_->store.fetchBytes(keyString(key ^ 0x6d657368'00000000ULL), out)) {
        ++impl_->misses;
        return false;
    }
    ++impl_->hits;
    return true;
}

void DdcCache::put(std::uint64_t key, const std::string& bytes) {
    impl_->store.storeBytes(keyString(key ^ 0x6d657368'00000000ULL), bytes);
}

bool MemoryBlobStore::get(std::uint64_t key, std::string& out) {
    const auto it = blobs_.find(key);
    if (it == blobs_.end()) {
        ++misses_;
        return false;
    }
    ++hits_;
    out = it->second;
    return true;
}

void MemoryBlobStore::put(std::uint64_t key, const std::string& bytes) {
    blobs_.emplace(key, bytes);
}

std::size_t DdcCache::hits() const { return impl_->hits; }
std::size_t DdcCache::misses() const { return impl_->misses; }
std::size_t DdcCache::unreadable() const { return impl_->unreadable; }

void DdcCache::resetStats() {
    impl_->hits = 0;
    impl_->misses = 0;
    impl_->unreadable = 0;
}

// --- TieredCache -----------------------------------------------------------------------

TieredCache::TieredCache(std::unique_ptr<Cache> l0, std::unique_ptr<Cache> l1)
    : l0_(std::move(l0)), l1_(std::move(l1)) {}

TieredCache::~TieredCache() = default;

std::optional<Output> TieredCache::get(std::uint64_t key) {
    if (auto fast = l0_->get(key)) {
        ++l0Hits_;
        return fast;
    }
    if (!l1_) {
        ++misses_;
        return std::nullopt;
    }
    if (auto slow = l1_->get(key)) {
        // Promote, so the second drag of the same dimension does not pay for
        // deserialisation again. This is what makes scrubbing a parameter back and forth
        // feel instant after the first pass.
        l0_->put(key, *slow);
        ++promotions_;
        return slow;
    }
    ++misses_;
    return std::nullopt;
}

void TieredCache::put(std::uint64_t key, const Output& value) {
    l0_->put(key, value);
    // Synchronous for now. ADR 0004 calls for this to move to a background thread once the
    // interactive path is measured — serialising a large shape on the drag thread is
    // exactly the kind of stall the L0 tier exists to avoid. Measure before moving it:
    // a background writer needs its own lifetime and ordering rules, and that complexity
    // should be bought with evidence.
    if (l1_) l1_->put(key, value);
}

std::size_t TieredCache::hits() const {
    return l0Hits_ + (l1_ ? l1_->hits() : 0);
}

std::size_t TieredCache::misses() const { return misses_; }

void TieredCache::resetStats() {
    l0Hits_ = 0;
    promotions_ = 0;
    misses_ = 0;
    l0_->resetStats();
    if (l1_) l1_->resetStats();
}

}  // namespace cad::recompute
