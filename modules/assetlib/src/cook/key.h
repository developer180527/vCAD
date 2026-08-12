#pragma once
#include "assetlib/asset_registry.h"
#include "assetlib/cooker.h"
#include <filesystem>
#include <string>
#include <vector>

// Internal: cook IDENTITY and STALENESS — the "is this asset's cooked output
// already correct?" policy, isolated from orchestration so it can be reasoned
// about (and tested) on its own. Not a public header.
namespace assetlib {

// Lowercased extension of a project-relative source path (cooker lookup key).
std::string lowerExtOf(const std::string& sourcePath);

// The record's BLAKE3 source hash: rec.sourceHash when it's a valid 64-hex
// BLAKE3 (scan() keeps it fresh and upgrades legacy FNV), else hashed from
// disk. "" when the source is unreadable.
std::string cookSourceHash(const AssetRecord& rec,
                           const std::filesystem::path& projectRoot);

// The DDC key for this record's CURRENT inputs: source hash ⊕ cooker id ⊕
// cooker version ⊕ settings fingerprint ⊕ per-asset import settings ⊕ the
// source hashes of everything this asset DEPENDS ON.
// "" when the source is unreadable (no identity → nothing to cache).
//
// `depHashes` is why declaring a dependency (`ctx.addDependency`) now means
// something. It is THE invalidation mechanism, because staleness is decided by
// this key alone — see cookIsStale, and note it never consults `rec.state`, so a
// registry-side "mark dependents stale" cascade would be a no-op. Putting the
// dependency in the key also makes invalidation correct across a SHARED DDC: a
// cascade is local to one machine's registry, while a key is content-derived and
// therefore means the same thing on every machine.
//
// KNOWN LIMIT, by construction: dependencies are discovered DURING a cook and
// recorded afterwards, so they influence the key from the SECOND cook onward. A
// cooker that can determine its extra inputs from the source alone should also
// fold them into settingsFingerprint, which is checked before the first cook —
// MaterialCooker does exactly that with the .shader manifest. depHashes is the
// safety net for what only the cook itself can discover; settingsFingerprint is
// the first line.
std::string computeCookKey(const AssetRecord& rec, ICooker& cooker,
                           const std::filesystem::path& projectRoot,
                           const std::vector<std::string>& depHashes = {});

// Content-addressed staleness, given the record and its current key:
//   • key differs from the last attempt's → stale (inputs changed)
//   • same key + Failed → NOT stale (these exact inputs already failed;
//     retry only when something changes, or via forceRecook)
//   • same key + empty cookedPath → NOT stale (deliberately skipped)
//   • same key + materialized output missing → stale (a .cache wipe; the
//     DDC restores it without recooking)
// No mtime comparison, no global cook version.
bool cookIsStale(const AssetRecord& rec, const std::string& currentKey,
                 const std::filesystem::path& cacheRoot);

} // namespace assetlib
