#include "Loader.h"

#include "cad/log/Log.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace cad::abi {
namespace {

using base::Error;
using base::ErrorCode;

std::string lastLoadError() {
#if defined(_WIN32)
    const DWORD code = ::GetLastError();
    char* text = nullptr;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = length != 0 && text != nullptr ? std::string(text, length)
                                                         : "error " + std::to_string(code);
    if (text != nullptr) ::LocalFree(text);
    return message;
#else
    const char* text = ::dlerror();
    return text != nullptr ? std::string(text) : std::string("unknown error");
#endif
}

}  // namespace

base::Result<SharedLibrary> SharedLibrary::open(const std::filesystem::path& path) {
#if defined(_WIN32)
    // The DLL's own directory is searched for its dependencies, and nothing else is added to the
    // process search path. A plugin that ships its own libraries finds them; one that does not
    // cannot accidentally pick up a library from wherever the application happened to be launched.
    HMODULE handle = ::LoadLibraryExW(path.wstring().c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (handle == nullptr) {
        return Error{ErrorCode::InvalidInput, "That plugin's library could not be loaded.",
                     path.string() + ": " + lastLoadError()};
    }
    return SharedLibrary(reinterpret_cast<void*>(handle));
#else
    // RTLD_LOCAL is the important half and it is PLUGIN_CONTRACT.md §4.7 made real: symbols from
    // one plugin are not visible to another. Without it, two plugins that each bundle a different
    // version of the same library resolve to whichever loaded first -- the classic plugin-ecosystem
    // failure, and one that presents as a crash inside the innocent plugin.
    //
    // RTLD_NOW rather than lazy: every undefined symbol is resolved here, where the failure is a
    // legible refusal at load time, rather than at the first call from inside a recompute.
    ::dlerror();   // clear any stale error so lastLoadError() reports ours
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return Error{ErrorCode::InvalidInput, "That plugin's library could not be loaded.",
                     path.string() + ": " + lastLoadError()};
    }
    return SharedLibrary(handle);
#endif
}

void* SharedLibrary::symbol(const char* name) const {
    if (handle_ == nullptr) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
#else
    return ::dlsym(handle_, name);
#endif
}

std::vector<std::filesystem::path> discoverPluginDirectories(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return found;

    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec) || ec) continue;
        std::error_code exists;
        if (std::filesystem::exists(entry.path() / "plugin.manifest", exists) && !exists) {
            found.push_back(entry.path());
        }
    }
    // Deterministic order. Plugins must not depend on load order, but a host that varies it turns
    // an order-dependent bug into one that only reproduces on someone else's machine.
    std::sort(found.begin(), found.end());
    return found;
}

base::Result<LoadedPlugin> loadPluginFrom(const std::filesystem::path& directory,
                                          const CadHost* host) {
    if (host == nullptr) {
        return Error{ErrorCode::Internal, "No plugin host is available.", directory.string()};
    }

    const auto manifestPath = directory / "plugin.manifest";
    std::ifstream in(manifestPath, std::ios::binary);
    if (!in) {
        return Error{ErrorCode::InvalidInput, "That plugin has no manifest, so it was not loaded.",
                     manifestPath.string()};
    }
    std::ostringstream text;
    text << in.rdbuf();

    auto parsed = parsePluginManifest(text.str(), directory);
    if (!parsed) return parsed.error();
    const PluginManifest manifest = std::move(parsed.value());

    // ── the refusal that costs nothing ──────────────────────────────────────────────────
    //
    // Version checked from the MANIFEST, before the library is opened. This is the entire reason
    // a manifest exists: dlopen runs static initialisers, so a plugin refused after loading has
    // already executed. Refused here, it has not run at all.
    const char* reason = nullptr;
    if (cad_abi_accepts(manifest.abiMajor, manifest.minHostMinor, &reason) == 0) {
        return Error{ErrorCode::Unsupported,
                     reason != nullptr ? reason : "That plugin is not compatible with this version.",
                     manifest.id + ": manifest declares abi_major=" +
                         std::to_string(manifest.abiMajor) + " min_host_minor=" +
                         std::to_string(manifest.minHostMinor)};
    }

    const auto library = libraryPath(manifest);
    std::error_code ec;
    if (!std::filesystem::exists(library, ec) || ec) {
        return Error{ErrorCode::InvalidInput,
                     "That plugin's library is missing, so it was not loaded.", library.string()};
    }

    auto opened = SharedLibrary::open(library);
    if (!opened) return opened.error();
    SharedLibrary image = std::move(opened.value());

    using EntryPoint = const CadPluginDesc* (*)(void);
    // NOLINTNEXTLINE(*-reinterpret-cast) — the one unavoidable cast at any C plugin boundary.
    auto* entry = reinterpret_cast<EntryPoint>(image.symbol("cad_plugin_main"));
    if (entry == nullptr) {
        return Error{ErrorCode::InvalidInput,
                     "That file is not a vCAD plugin, so it was not loaded.",
                     library.string() + ": no cad_plugin_main"};
    }

    const CadPluginDesc* desc = entry();
    if (desc == nullptr) {
        return Error{ErrorCode::InvalidInput, "That plugin did not describe itself.",
                     library.string() + ": cad_plugin_main returned null"};
    }

    // struct_size before any field past the header, exactly as the host demands of descriptors
    // elsewhere. A plugin built against a newer header is LONGER than ours and must not be read
    // past our own end; one built against an older header is shorter and its trailing members are
    // simply absent.
    if (desc->struct_size < offsetof(CadPluginDesc, initialize) + sizeof(desc->initialize)) {
        return Error{ErrorCode::InvalidInput,
                     "That plugin was built against an incomplete version of the interface.",
                     manifest.id + ": struct_size " + std::to_string(desc->struct_size)};
    }

    // ── the manifest is a claim; the binary is the truth ────────────────────────────────
    //
    // Checked against each other rather than trusted separately. A manifest that could disagree
    // with its own library is a lie surface: the user is shown one plugin's identity and version
    // while a different plugin's code runs. Refusing the disagreement is the only way the consent
    // the user gave at install time means anything at load time.
    if (desc->id == nullptr || manifest.id != desc->id) {
        return Error{ErrorCode::InvalidInput,
                     "That plugin's manifest does not match its library, so it was not loaded.",
                     "manifest id '" + manifest.id + "' but library says '" +
                         (desc->id != nullptr ? desc->id : "(null)") + "'"};
    }
    if (desc->abi_major != manifest.abiMajor || desc->min_host_minor != manifest.minHostMinor) {
        return Error{ErrorCode::InvalidInput,
                     "That plugin's manifest does not match its library, so it was not loaded.",
                     manifest.id + ": manifest says abi_major=" +
                         std::to_string(manifest.abiMajor) + "/min_host_minor=" +
                         std::to_string(manifest.minHostMinor) + ", library says " +
                         std::to_string(desc->abi_major) + "/" +
                         std::to_string(desc->min_host_minor)};
    }
    if (desc->required_caps != manifest.requiredCaps) {
        return Error{ErrorCode::InvalidInput,
                     "That plugin asks for more than its manifest declared, so it was not loaded.",
                     manifest.id + ": manifest declared caps " +
                         std::to_string(manifest.requiredCaps) + ", library requests " +
                         std::to_string(desc->required_caps)};
    }

    if (desc->initialize == nullptr) {
        return Error{ErrorCode::InvalidInput, "That plugin has no initialisation entry point.",
                     manifest.id};
    }

    const CadStatus status = desc->initialize(host);
    if (status != CAD_OK) {
        // Not unloaded, and that is deliberate: initialize may have registered something before
        // failing, and unmapping an image the registry now points into is worse than the leak of
        // keeping it. The registration window closes either way.
        return Error{ErrorCode::Internal, "That plugin failed to start, so it was not loaded.",
                     manifest.id + ": initialize returned " + std::to_string(status)};
    }

    CAD_INFO(log::Category::Plugin)
        << "loaded " << manifest.id << " " << manifest.semver << " from " << library.string();

    return LoadedPlugin{manifest, desc, std::move(image)};
}

}  // namespace cad::abi
