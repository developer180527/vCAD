#pragma once

/// Discovering, checking and loading plugin shared libraries.
///
/// # The order of operations is the design
///
/// ```
/// discover → read manifest → REFUSE HERE IF POSSIBLE → dlopen → cad_plugin_main → verify → initialize
/// ```
///
/// Everything to the left of `dlopen` happens with none of the plugin's code having run, and that
/// is the point of having a manifest at all (see Manifest.h). A version mismatch, a malformed
/// manifest, or a capability this host does not know about are all decided there.
///
/// Everything to the right treats the descriptor as the truth and the manifest as a claim. They
/// are checked against each other, because a manifest that could disagree with its binary would be
/// a lie surface: the user would be shown one plugin's identity while another's code ran.
///
/// # What this deliberately does not do
///
/// **It never unloads.** PLUGIN_CONTRACT.md §2 decided plugins are not hot-reloadable, and this is
/// where that decision becomes code. A document may hold features whose compute is a function
/// pointer into a plugin's image; unmapping that image means the next recompute calls into
/// unmapped memory, and the window in which that is safe cannot be proven. Libraries stay resident
/// for the life of the process. The cost is that updating a plugin needs a restart.

#include "Manifest.h"
#include "cad/abi/cad_plugin_abi.h"
#include "cad/base/Result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cad::abi {

/// An open shared library. Closes nothing — see the note above on why nothing is ever unloaded.
class SharedLibrary {
public:
    /// Opens `path`, or returns why it could not be opened.
    [[nodiscard]] static base::Result<SharedLibrary> open(const std::filesystem::path&);

    /// A symbol, or null. The caller checks; a missing `cad_plugin_main` is a legible refusal
    /// rather than a crash.
    [[nodiscard]] void* symbol(const char* name) const;

private:
    explicit SharedLibrary(void* handle) : handle_(handle) {}
    void* handle_ = nullptr;
};

/// One plugin that loaded, initialised and is now running.
struct LoadedPlugin {
    PluginManifest manifest;
    const CadPluginDesc* desc = nullptr;
    SharedLibrary library;
};

/// What a scan of a directory found. Failures are DATA rather than exceptions: one bad plugin must
/// not stop the others loading, and the user needs to be told which one and why.
struct PluginLoadReport {
    struct Rejection {
        std::string id;        ///< from the manifest when it parsed, otherwise the directory name
        std::string message;   ///< user-facing
        std::string detail;    ///< developer-facing
    };
    std::vector<std::string> loaded;      ///< ids, in load order
    std::vector<Rejection> rejected;
};

/// Reads one plugin directory and loads it, or explains why not.
///
/// `initialize` is called with `host`; registration happens inside it and nowhere else.
[[nodiscard]] base::Result<LoadedPlugin> loadPluginFrom(const std::filesystem::path& directory,
                                                        const CadHost* host);

/// Every immediate subdirectory of `root` containing a `plugin.manifest`.
///
/// Sorted by directory name, so load order is the same on every machine. Plugins must not depend
/// on each other's load order — but a host that varies it makes an order-dependent bug appear
/// only on someone else's computer.
[[nodiscard]] std::vector<std::filesystem::path> discoverPluginDirectories(
    const std::filesystem::path& root);

}  // namespace cad::abi
