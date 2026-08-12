#include "assetlib/ddc_manifest.h"

#include <cstdio>

namespace assetlib {

namespace {
constexpr const char* kMagic = "ddc-manifest-v1";
constexpr const char* kPrimaryName = "@";

// True only for a bare filename that stays inside its directory: no
// separators of either flavour, no drive/ADS colon, no "." / ".." traversal,
// no leading dash-free weirdness beyond that. Anything else is rejected
// rather than sanitized — a manifest we don't fully understand is a miss.
bool isPlainFilename(const std::string& name) {
    if (name.empty() || name.size() > 255)                return false;
    if (name == "." || name == "..")                      return false;
    if (name.find_first_of("/\\:") != std::string::npos)  return false;
    // Reject control characters (a name is a filesystem path, not a payload).
    for (unsigned char c : name) if (c < 0x20 || c == 0x7f) return false;
    // Belt and braces: the path itself must agree it is just a filename.
    const std::filesystem::path p(name);
    return p.filename() == p && !p.has_root_directory() && !p.has_root_name();
}
} // namespace

bool ddcStoreRecord(DdcStore& ddc, const std::string& key,
                    const std::filesystem::path& primary,
                    const std::vector<std::filesystem::path>& extras) {
    std::string manifest = std::string(kMagic) + "\n";
    auto addMember = [&](const std::filesystem::path& file,
                         const std::string& name) -> bool {
        const std::string mk = blake3File(file);
        if (mk.empty() || !ddc.store(mk, file)) return false;
        manifest += mk; manifest += '\t'; manifest += name; manifest += '\n';
        return true;
    };
    if (!addMember(primary, kPrimaryName)) return false;
    for (const auto& e : extras)
        if (!addMember(e, e.filename().string())) return false;
    return ddc.storeBytes(key, manifest);
}

bool ddcFetchRecord(DdcStore& ddc, const std::string& key,
                    const std::filesystem::path& outPath) {
    std::string manifest;
    if (!ddc.fetchBytes(key, manifest)) return false;

    size_t pos = manifest.find('\n');
    if (pos == std::string::npos ||
        manifest.compare(0, pos, kMagic) != 0) return false;
    ++pos;
    // A record is only a hit if it actually yields the PRIMARY output. A
    // manifest carrying no "@" member (a truncated one from a shared tier
    // produces exactly that) used to return true having written nothing, so
    // the caller committed Ready with a cookedPath that did not exist.
    // Found by tests/fuzz_ddc_manifest_test.cpp on its first run.
    bool sawPrimary = false;
    while (pos < manifest.size()) {
        size_t eol = manifest.find('\n', pos);
        if (eol == std::string::npos) eol = manifest.size();
        const std::string line = manifest.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) return false;
        const std::string mk   = line.substr(0, tab);
        const std::string name = line.substr(tab + 1);
        // A manifest from a SHARED store is REMOTE INPUT written by another
        // machine — treat member names as hostile. Allowlist (a plain
        // filename, nothing else) rather than blocklist: rejecting only
        // "/ \ .." still lets "C:evil" through on Windows, where a
        // drive-relative path escapes the cache directory entirely.
        if (name != kPrimaryName && !isPlainFilename(name)) {
            std::fprintf(stderr, "[DDC] rejected manifest member name '%s' "
                         "for key %s\n", name.c_str(), key.c_str());
            return false;
        }
        const bool isPrimary = (name == kPrimaryName);
        if (isPrimary) {
            if (sawPrimary) return false;      // two primaries: malformed
            sawPrimary = true;
        }
        const std::filesystem::path dst =
            isPrimary ? outPath : outPath.parent_path() / name;
        if (!ddc.fetch(mk, dst)) return false;
    }
    return sawPrimary;
}

} // namespace assetlib
