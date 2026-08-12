#pragma once
#include "ddc.h"
#include <filesystem>
#include <string>
#include <vector>

namespace assetlib {

// ── DDC record = manifest of member blobs ────────────────────────────────────
// A cook can produce several files (cooked mesh + sibling .ctex embedded
// textures). Each member is stored under ITS OWN content hash; a small
// manifest under the cook key names them. Fetching materializes every member
// or reports a miss — a hit can never yield a mesh missing its textures.
// Format: "ddc-manifest-v1\n" then "<blobKey>\t<name>\n" per member, where
// name "@" is the primary output and anything else is a sibling filename.
//
// Splitting members by content hash also dedups for free: two meshes carrying
// byte-identical textures share one blob.

// Store `primary` (+ any `extras`) as the record for `key`. All-or-nothing.
bool ddcStoreRecord(DdcStore& ddc, const std::string& key,
                    const std::filesystem::path& primary,
                    const std::vector<std::filesystem::path>& extras);

// Materialize the record for `key`: the primary at `outPath`, every extra
// beside it. False on miss OR any missing member (never a partial result).
bool ddcFetchRecord(DdcStore& ddc, const std::string& key,
                    const std::filesystem::path& outPath);

} // namespace assetlib
