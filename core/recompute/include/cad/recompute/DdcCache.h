#pragma once

#include "cad/recompute/Engine.h"

#include <filesystem>
#include <memory>

namespace cad::recompute {

/// L1/L2 tier backed by assetlib's `DdcStore` (ADR 0004).
///
/// What we get for free by reusing it rather than writing another one:
///   * BLAKE3-256 content addressing. Cryptographic strength is not paranoia on a SHARED
///     tier — an FNV collision would silently serve the wrong geometry to every machine in
///     the team.
///   * Two tiers: a local per-machine store plus an optional shared mount. A colleague or
///     CI that already rebuilt this assembly means we fetch instead of recompute.
///   * Atomic ingest, immutable read-only blobs, LRU eviction by mtime touched on fetch,
///     and link-count pinning.
///
/// What is CAD-specific and had to be added here:
///   * `Output` is a live OCCT handle graph, so everything goes through cad::io::serialize.
///   * The serialisation version is folded into the key, so a format change makes old blobs
///     unreachable rather than misread.
///   * A blob that fails to read is a MISS, never an error. A stale or corrupt cache entry
///     must not be able to fail a build.
class DdcCache : public Cache {
public:
    /// Empty `localRoot` uses assetlib's default (~/.engine/ddc, or $ENGINE_DDC).
    /// Empty `sharedRoot` disables the shared tier.
    explicit DdcCache(std::filesystem::path localRoot = {},
                      std::filesystem::path sharedRoot = {});
    ~DdcCache() override;

    std::optional<Output> get(std::uint64_t key) override;
    void put(std::uint64_t key, const Output&) override;

    std::size_t hits() const override;
    std::size_t misses() const override;
    void resetStats() override;

    /// Blobs that were found but could not be decoded. Non-zero means a format change went
    /// out without a version bump, or the store is damaged — worth surfacing rather than
    /// absorbing silently into the miss count.
    [[nodiscard]] std::size_t unreadable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// L0 in front of L1: memory, then disk/shared.
///
/// The tiering is the whole point of ADR 0004's first modification. Dragging a dimension
/// has to answer inside ~16-50 ms, and that budget does not survive serialisation plus a
/// filesystem round trip. L0 holds live shapes with no encoding at all; a hit there costs a
/// hash lookup. L1 is consulted only on an L0 miss, and a value found in L1 is promoted
/// into L0 so the second drag of the same dimension is instant.
///
/// Writes go to both, but the L1 write is where an assetlib-style background thread will
/// go once the interactive path is measured (currently synchronous — see the note in put()).
class TieredCache : public Cache {
public:
    TieredCache(std::unique_ptr<Cache> l0, std::unique_ptr<Cache> l1);
    ~TieredCache() override;

    std::optional<Output> get(std::uint64_t key) override;
    void put(std::uint64_t key, const Output&) override;

    std::size_t hits() const override;
    std::size_t misses() const override;
    void resetStats() override;

    [[nodiscard]] Cache& l0() noexcept { return *l0_; }
    [[nodiscard]] Cache& l1() noexcept { return *l1_; }

    /// Hits served by the fast tier. The ratio against total hits is the number that says
    /// whether interaction will feel immediate.
    [[nodiscard]] std::size_t l0Hits() const noexcept { return l0Hits_; }
    [[nodiscard]] std::size_t promotions() const noexcept { return promotions_; }

private:
    std::unique_ptr<Cache> l0_;
    std::unique_ptr<Cache> l1_;
    std::size_t l0Hits_ = 0;
    std::size_t promotions_ = 0;
    std::size_t misses_ = 0;
};

}  // namespace cad::recompute
