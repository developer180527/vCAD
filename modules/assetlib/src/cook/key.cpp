#include "cook/key.h"
#include "assetlib/ddc.h"

#include <cctype>

namespace assetlib {

std::string lowerExtOf(const std::string& sourcePath) {
    std::string ext = std::filesystem::path(sourcePath).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
    return ext;
}

std::string cookSourceHash(const AssetRecord& rec,
                           const std::filesystem::path& projectRoot) {
    if (rec.sourceHash.size() == 64) return rec.sourceHash;
    return blake3File(projectRoot / rec.sourcePath);
}

std::string computeCookKey(const AssetRecord& rec, ICooker& cooker,
                           const std::filesystem::path& projectRoot,
                           const std::vector<std::string>& depHashes) {
    const std::string srcHash = cookSourceHash(rec, projectRoot);
    if (srcHash.empty()) return {};        // unreadable source — no identity

    CookContext ctx;                        // fingerprint may inspect the path
    ctx.uuid       = rec.uuid;
    ctx.sourcePath = projectRoot / rec.sourcePath;

    DdcKeyInputs in;
    in.cookerId      = cooker.id();
    in.cookerVersion = cooker.version();
    in.settings      = cooker.settingsFingerprint(ctx);
    if (!rec.importSettings.empty()) {
        in.settings += '\n';
        in.settings += rec.importSettings.json;
    }
    in.sourceHash = srcHash;
    // computeDdcKey sorts these, so neither the order a cooker declares its
    // inputs in nor the row order of the dependency table can change the key.
    in.depHashes = depHashes;

    // Extra FILES the cooker says it reads. Hashed here rather than by each
    // cooker, so declaring an input is all a cooker has to do — and so a cooker
    // that declares nothing is visibly declaring nothing, instead of being
    // indistinguishable from one that hashed its inputs by hand.
    //
    // The path is folded in alongside the content hash: two different declared
    // files with identical bytes are the same input for keying purposes, but a
    // cook that starts reading a DIFFERENT file with the same content is a real
    // change of inputs. A path that does not exist contributes a distinct
    // "missing" marker rather than being skipped, so an input appearing later
    // moves the key.
    for (const auto& p : cooker.declaredInputs(ctx)) {
        const std::string h = blake3File(p);
        in.depHashes.push_back(p.generic_string() + "@"
                               + (h.empty() ? "missing" : h));
    }
    return computeDdcKey(in);
}

bool cookIsStale(const AssetRecord& rec, const std::string& currentKey,
                 const std::filesystem::path& cacheRoot) {
    if (currentKey.empty()) return false;   // unreadable source — cooking
                                            // would fail identically
    // Inputs changed since the last attempt (or never attempted).
    if (rec.ddcKey != currentKey) return true;

    // Same inputs as last time. Failed stays failed (retry only when the
    // source/cooker/settings change — or via forceRecook); a Ready record
    // with no cookedPath was deliberately skipped.
    if (rec.state == AssetState::Failed) return false;
    if (rec.cookedPath.empty())          return false;

    // Materialized output vanished (user wiped .cache/) — a DDC hit restores
    // it without recooking.
    std::error_code ec;
    return !std::filesystem::exists(cacheRoot / rec.cookedPath, ec);
}

} // namespace assetlib
