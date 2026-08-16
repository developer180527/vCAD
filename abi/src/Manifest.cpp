#include "Manifest.h"

#include "cad/abi/cad_plugin_abi.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace cad::abi {
namespace {

using base::Error;
using base::ErrorCode;

std::string trimmed(std::string_view s) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto begin = std::find_if(s.begin(), s.end(), notSpace);
    const auto end = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    return begin < end ? std::string(begin, end) : std::string{};
}

/// A key's value as an unsigned integer, or nothing.
///
/// `from_chars` rather than `stoul`: it does not throw, does not consult the locale, and reports
/// trailing junk. `abi_major = 1x` is a malformed manifest and must be refused rather than read as
/// 1 — a version field that silently accepts a prefix is how a plugin ends up loaded under the
/// wrong generation.
bool wholeNumber(const std::string& text, std::uint32_t& out) {
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) return false;
    out = value;
    return true;
}

std::uint32_t capabilityBit(std::string_view name) {
    if (name == "filesystem") return CAD_CAP_FILESYSTEM;
    if (name == "network") return CAD_CAP_NETWORK;
    if (name == "subprocess") return CAD_CAP_SUBPROCESS;
    if (name == "ui") return CAD_CAP_UI;
    return 0;
}

}  // namespace

const char* sharedLibrarySuffix() noexcept {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

base::Result<PluginManifest> parsePluginManifest(const std::string& text,
                                                 std::filesystem::path directory) {
    PluginManifest manifest;
    manifest.directory = std::move(directory);

    bool sawAbiMajor = false;
    std::istringstream in(text);
    std::string line;
    int number = 0;

    while (std::getline(in, line)) {
        ++number;
        const std::string clean = trimmed(line);
        if (clean.empty() || clean[0] == '#') continue;

        const auto eq = clean.find('=');
        if (eq == std::string::npos) {
            return Error{ErrorCode::InvalidInput,
                         "That plugin's manifest is malformed and it was not loaded.",
                         "line " + std::to_string(number) + ": expected key = value"};
        }
        const std::string key = trimmed(std::string_view(clean).substr(0, eq));
        const std::string value = trimmed(std::string_view(clean).substr(eq + 1));

        if (key == "id") {
            manifest.id = value;
        } else if (key == "name") {
            manifest.displayName = value;
        } else if (key == "semver") {
            manifest.semver = value;
        } else if (key == "library") {
            manifest.library = value;
        } else if (key == "abi_major") {
            if (!wholeNumber(value, manifest.abiMajor)) {
                return Error{ErrorCode::InvalidInput,
                             "That plugin's manifest is malformed and it was not loaded.",
                             "line " + std::to_string(number) + ": abi_major is not a number"};
            }
            sawAbiMajor = true;
        } else if (key == "min_host_minor") {
            if (!wholeNumber(value, manifest.minHostMinor)) {
                return Error{ErrorCode::InvalidInput,
                             "That plugin's manifest is malformed and it was not loaded.",
                             "line " + std::to_string(number) + ": min_host_minor is not a number"};
            }
        } else if (key == "capabilities") {
            // Comma-separated. An unknown name is REFUSED rather than ignored: a manifest asking
            // for a capability this host has never heard of is either from the future or a typo,
            // and silently dropping it would show the user a permission list that is not what the
            // plugin asked for.
            std::size_t at = 0;
            while (at <= value.size()) {
                const auto comma = value.find(',', at);
                const auto piece = trimmed(std::string_view(value).substr(
                    at, comma == std::string::npos ? std::string::npos : comma - at));
                if (!piece.empty()) {
                    const std::uint32_t bit = capabilityBit(piece);
                    if (bit == 0) {
                        return Error{ErrorCode::InvalidInput,
                                     "That plugin asks for a capability this version of vCAD does "
                                     "not know about, so it was not loaded.",
                                     "line " + std::to_string(number) + ": unknown capability '" +
                                         piece + "'"};
                    }
                    manifest.requiredCaps |= bit;
                }
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
        // Unknown KEYS are ignored, unlike unknown capabilities. A newer plugin may carry fields
        // this host predates, and refusing those would make the manifest non-additive -- the exact
        // property the ABI itself is built to have.
    }

    if (manifest.id.empty()) {
        return Error{ErrorCode::InvalidInput,
                     "That plugin's manifest does not say what plugin it is, so it was not loaded.",
                     "missing `id`"};
    }
    if (!sawAbiMajor) {
        return Error{ErrorCode::InvalidInput,
                     "That plugin's manifest does not say which plugin interface it was built "
                     "for, so it was not loaded.",
                     "missing `abi_major`"};
    }
    if (manifest.library.empty()) {
        return Error{ErrorCode::InvalidInput,
                     "That plugin's manifest does not name a library, so it was not loaded.",
                     "missing `library`"};
    }

    // The `library` value must be a bare FILENAME. See the note on PluginManifest::library: a
    // manifest is written by whoever shipped the plugin, so a path here would let installing one
    // plugin load a library from anywhere on the machine.
    const std::filesystem::path named(manifest.library);
    if (named.is_absolute() || named.has_root_path() || named.has_parent_path() ||
        manifest.library.find("..") != std::string::npos) {
        return Error{ErrorCode::InvalidInput,
                     "That plugin's manifest names a library outside its own folder, so it was "
                     "not loaded.",
                     "library must be a file name, not a path: '" + manifest.library + "'"};
    }

    return manifest;
}

std::filesystem::path libraryPath(const PluginManifest& manifest) {
    std::filesystem::path name(manifest.library);
    // A manifest may name `sheetmetal` and get `sheetmetal.dylib`, `.so` or `.dll` — one manifest
    // for five platforms. An explicit suffix is honoured as written, so a plugin that really does
    // ship differently-named binaries can still say so.
    if (!name.has_extension()) name += sharedLibrarySuffix();
    return manifest.directory / name;
}

}  // namespace cad::abi
