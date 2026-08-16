#include "cad/abi/PluginCatalog.h"

#include "Loader.h"
#include "Manifest.h"
#include "cad/abi/cad_plugin_abi.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cad::abi {

const char* toString(PluginStatus status) noexcept {
    switch (status) {
        case PluginStatus::Ready:          return "Ready";
        case PluginStatus::Incompatible:   return "Incompatible";
        case PluginStatus::Invalid:        return "Invalid";
        case PluginStatus::LibraryMissing: return "Library missing";
    }
    return "Unknown";
}

std::string describeCapabilities(std::uint32_t caps) {
    std::string text;
    const auto add = [&text](const char* name) {
        if (!text.empty()) text += ", ";
        text += name;
    };
    if ((caps & CAD_CAP_FILESYSTEM) != 0) add("filesystem");
    if ((caps & CAD_CAP_NETWORK) != 0) add("network");
    if ((caps & CAD_CAP_SUBPROCESS) != 0) add("subprocess");
    if ((caps & CAD_CAP_UI) != 0) add("ui");
    return text;
}

std::filesystem::path userPluginDirectory() {
    // The override exists for tests, and for anyone running two builds side by side. Read from the
    // environment rather than a setting because it has to work before any settings are loaded.
    if (const char* override = std::getenv("VCAD_PLUGIN_DIR");
        override != nullptr && override[0] != '\0') {
        return {override};
    }

#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && local[0] != '\0') {
        return std::filesystem::path(local) / "vCAD" / "plugins";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / "Library" / "Application Support" / "vCAD" /
               "plugins";
    }
#else
    // XDG first, then its documented default. Honouring XDG_DATA_HOME matters on Linux because a
    // packaged application that ignores it writes into a directory the distribution does not
    // expect and the user cannot relocate.
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "vCAD" / "plugins";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "vCAD" / "plugins";
    }
#endif
    return std::filesystem::current_path() / "plugins";
}

std::vector<InstalledPlugin> scanInstalledPlugins(const std::filesystem::path& root) {
    std::vector<InstalledPlugin> found;

    for (const auto& directory : discoverPluginDirectories(root)) {
        InstalledPlugin entry;
        entry.directory = directory;
        // The directory name is the fallback identity, for BOTH fields. A plugin whose manifest
        // will not parse still has to be nameable in a list, or the only way to remove it is to
        // guess -- and a blank cell reads as missing data rather than as "its manifest could not
        // be read", which is the thing actually being reported.
        entry.id = directory.filename().string();
        entry.displayName = entry.id;

        std::ifstream in(directory / "plugin.manifest", std::ios::binary);
        if (!in) {
            entry.status = PluginStatus::Invalid;
            entry.statusDetail = "Its manifest could not be read.";
            found.push_back(std::move(entry));
            continue;
        }
        std::ostringstream text;
        text << in.rdbuf();

        auto parsed = parsePluginManifest(text.str(), directory);
        if (!parsed) {
            entry.status = PluginStatus::Invalid;
            entry.statusDetail = parsed.error().message;
            found.push_back(std::move(entry));
            continue;
        }

        const PluginManifest& manifest = parsed.value();
        entry.id = manifest.id;
        entry.displayName = manifest.displayName.empty() ? manifest.id : manifest.displayName;
        entry.semver = manifest.semver;
        entry.requiredCaps = manifest.requiredCaps;

        // Decided from the manifest, with nothing opened. This is the same call the loader makes
        // before dlopen, so what a manager shows and what a load would do cannot disagree.
        const char* reason = nullptr;
        if (cad_abi_accepts(manifest.abiMajor, manifest.minHostMinor, &reason) == 0) {
            entry.status = PluginStatus::Incompatible;
            entry.statusDetail = reason != nullptr ? reason : "It is not compatible.";
            found.push_back(std::move(entry));
            continue;
        }

        std::error_code ec;
        if (!std::filesystem::exists(libraryPath(manifest), ec) || ec) {
            entry.status = PluginStatus::LibraryMissing;
            entry.statusDetail = "Its library is missing from the plugin folder.";
            found.push_back(std::move(entry));
            continue;
        }

        entry.status = PluginStatus::Ready;
        found.push_back(std::move(entry));
    }

    return found;
}

}  // namespace cad::abi
