#pragma once

/// A plugin's manifest: what it claims about itself, read before any of its code runs.
///
/// # Why a manifest exists at all
///
/// Not for dependency management. `dlopen` runs static initialisers, and static initialisers are
/// code execution — so without a manifest the host must EXECUTE a plugin in order to learn whether
/// it should execute it. With one, the host can show a user what a plugin claims and what it wants,
/// and can refuse a version mismatch, with nothing of the plugin's having run.
///
/// That is the entire justification, and it decides the shape of everything here: this file parses
/// data from a stranger, reaches a decision, and only then does the loader touch the binary.
///
/// PLUGIN_CONTRACT.md §2 also rules out two things a manifest might be expected to carry.
/// It must NOT list library dependencies for conflict checking — §4.7 makes conflicts impossible
/// by construction, so such a list would have no consumer, could not be verified against the
/// binary, and would be a permanent surface that starts lying the moment someone edits it. And a
/// manifest is never read from a document: a `.vpart` that could name a library to load would make
/// opening a file arbitrary code execution.
///
/// # The format
///
/// `key = value`, one per line, `#` for comments. Deliberately not JSON: this is the first thing
/// that touches untrusted bytes on the plugin path, and a format whose whole grammar fits in the
/// function below is one where "what could a hostile file do here" has a short answer. It also
/// costs no dependency.

#include "cad/base/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace cad::abi {

struct PluginManifest {
    std::string id;             ///< reverse-DNS, e.g. "com.acme.sheetmetal"
    std::string displayName;
    std::string semver;

    std::uint32_t abiMajor = 0;
    std::uint32_t minHostMinor = 0;
    std::uint32_t requiredCaps = 0;

    /// The shared library's FILE NAME, resolved against the manifest's own directory.
    ///
    /// A bare filename, enforced: no directory separators, no `..`, not absolute. The manifest is
    /// written by whoever shipped the plugin, so a path here would let an installed manifest point
    /// the loader at any library on the machine — which turns "install this plugin" into "load
    /// that". `library = ../../../../usr/lib/something.so` is the attack, and it is refused by
    /// `parsePluginManifest` rather than by the caller remembering to check.
    std::string library;

    /// Where this manifest was read from. The library is resolved beside it.
    std::filesystem::path directory;
};

/// Parses manifest text. Never throws, never reads the filesystem.
///
/// `directory` is recorded on the result so the loader can resolve `library` against it; it is not
/// read here.
[[nodiscard]] base::Result<PluginManifest> parsePluginManifest(const std::string& text,
                                                              std::filesystem::path directory);

/// The library path this manifest names, already constrained to `directory`.
[[nodiscard]] std::filesystem::path libraryPath(const PluginManifest&);

/// The platform's shared-library suffix — `.dll`, `.dylib`, `.so`.
///
/// Exposed so a manifest can name `sheetmetal` and get the right file on each platform, which is
/// what lets one plugin ship one manifest for five targets.
[[nodiscard]] const char* sharedLibrarySuffix() noexcept;

}  // namespace cad::abi
