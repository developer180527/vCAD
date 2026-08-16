#pragma once

/// What plugins are installed, and what the host would do with each of them.
///
/// This is the surface a plugin MANAGER is built on, and it deliberately answers the question
/// without loading anything. Everything below is read from manifests — no library is opened, no
/// `cad_plugin_main` is called, no static initialiser runs. A user looking at a list of installed
/// plugins has not thereby executed them, which is the same reasoning that makes the manifest exist
/// at all (see Manifest.h in this module).
///
/// C++ rather than C, unlike the rest of this directory. The consumers are in-process hosts: the
/// Qt shell today, the SwiftUI shell later. A plugin never calls this — it describes plugins to
/// the application, not the application to a plugin — so the C-ABI constraints that shape
/// `cad_plugin_abi.h` do not apply and would only make the caller's life harder.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cad::abi {

/// What the host would do with this plugin if it were asked to load it.
enum class PluginStatus {
    /// The manifest is valid and this host serves its ABI generation.
    Ready,
    /// The manifest asks for a newer host, or a generation this host does not serve. Refusable
    /// without opening the library, which is the point.
    Incompatible,
    /// The manifest could not be read: malformed, missing a required field, or naming a library
    /// outside its own folder.
    Invalid,
    /// The manifest is fine and the library it names is not there.
    LibraryMissing,
};

[[nodiscard]] const char* toString(PluginStatus) noexcept;

/// One installed plugin, as its manifest describes it.
///
/// Everything here is a CLAIM. The manifest is what the plugin says about itself, and it is only
/// checked against the library at load time — so a manager showing this list is showing what
/// plugins assert, and should say so rather than implying the host has verified it.
struct InstalledPlugin {
    std::string id;
    std::string displayName;
    std::string semver;
    std::uint32_t requiredCaps = 0;

    PluginStatus status = PluginStatus::Invalid;
    /// One sentence a user can act on, for anything other than Ready.
    std::string statusDetail;

    std::filesystem::path directory;
};

/// Every plugin directory under `root`, in a stable order, with its status decided.
///
/// Never throws and never loads. A directory that is not a plugin at all is skipped; one that is a
/// broken plugin is REPORTED, because "I installed it and it is not in the list" is the least
/// actionable thing a plugin system can tell someone.
[[nodiscard]] std::vector<InstalledPlugin> scanInstalledPlugins(const std::filesystem::path& root);

/// Where plugins live for the current user.
///
/// Per-user rather than system-wide, deliberately: installing a plugin downloaded from the internet
/// must never require administrator rights. An installer that asks for elevation is a
/// privilege-escalation step wearing a convenience hat, and the thing being elevated is arbitrary
/// third-party code.
///
/// `VCAD_PLUGIN_DIR` overrides it, which is how tests point at a staging directory without writing
/// into a developer's real profile.
[[nodiscard]] std::filesystem::path userPluginDirectory();

/// A human-readable list of the capabilities in a `CAD_CAP_*` bitmask, e.g. "filesystem, network".
///
/// Empty for no capabilities, which a manager should render as something explicit rather than as a
/// blank cell — "none" is information and an empty cell is an absence of it.
[[nodiscard]] std::string describeCapabilities(std::uint32_t caps);

}  // namespace cad::abi
