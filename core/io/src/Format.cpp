#include "cad/io/Format.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace cad::io {

std::vector<std::shared_ptr<IFormatProvider>> occtProviders();

namespace {

std::string lowerExt(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}  // namespace

std::string ImportReport::summary() const {
    std::ostringstream os;
    os << "Imported " << format << ": " << solids << " solid" << (solids == 1 ? "" : "s")
       << ", " << faces << " face" << (faces == 1 ? "" : "s") << ".";
    if (!declaredUnits.empty()) {
        os << " Units: " << declaredUnits << ".";
    } else if (unitsWereAssumed) {
        os << " Units were assumed.";
    }
    if (healing.changed) os << " " << healing.summary();
    for (const auto& w : warnings) os << " " << w;
    for (const auto& u : unsupported) os << " " << u;
    return os.str();
}

struct FormatRegistry::Impl {
    std::vector<std::shared_ptr<IFormatProvider>> providers;
    std::unordered_map<std::string, IFormatProvider*> byExtension;
};

FormatRegistry::FormatRegistry() : impl_(std::make_unique<Impl>()) {}
FormatRegistry::~FormatRegistry() = default;
FormatRegistry::FormatRegistry(FormatRegistry&&) noexcept = default;
FormatRegistry& FormatRegistry::operator=(FormatRegistry&&) noexcept = default;

void FormatRegistry::add(std::shared_ptr<IFormatProvider> provider) {
    if (!provider) return;
    // Last registration wins for an extension, so a plugin can deliberately supersede a
    // built-in — a licensed Parasolid provider replacing nothing, or a better STEP reader
    // replacing ours.
    for (const auto& ext : provider->extensions()) {
        impl_->byExtension[ext] = provider.get();
    }
    impl_->providers.push_back(std::move(provider));
}

IFormatProvider* FormatRegistry::forPath(const std::string& path) const {
    const auto it = impl_->byExtension.find(lowerExt(path));
    return it == impl_->byExtension.end() ? nullptr : it->second;
}

IFormatProvider* FormatRegistry::byId(const std::string& id) const {
    for (const auto& p : impl_->providers) {
        if (p->id() == id) return p.get();
    }
    return nullptr;
}

std::vector<IFormatProvider*> FormatRegistry::all() const {
    std::vector<IFormatProvider*> out;
    out.reserve(impl_->providers.size());
    for (const auto& p : impl_->providers) out.push_back(p.get());
    return out;
}

std::vector<std::string> FormatRegistry::readableExtensions() const {
    std::vector<std::string> out;
    for (const auto& p : impl_->providers) {
        if (!p->capabilities().read) continue;
        for (const auto& e : p->extensions()) out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> FormatRegistry::writableExtensions() const {
    std::vector<std::string> out;
    for (const auto& p : impl_->providers) {
        if (!p->capabilities().write) continue;
        for (const auto& e : p->extensions()) out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

FormatRegistry FormatRegistry::builtins() {
    FormatRegistry r;
    for (auto& p : occtProviders()) r.add(std::move(p));
    // 3MF (lib3mf) and the licensed OCCT add-ons — Parasolid, JT, DXF, IFC — register here
    // when they are compiled in. See docs/FORMATS.md for which need a licence.
    return r;
}

kernel::Result<ImportResult> importFile(const FormatRegistry& registry,
                                        const std::string& path,
                                        const ImportOptions& options) {
    IFormatProvider* provider = registry.forPath(path);
    if (provider == nullptr) {
        return kernel::Error{kernel::ErrorCode::Unsupported,
                             "I don't know how to open this kind of file.",
                             "no provider registered for " + path};
    }
    if (!provider->capabilities().read) {
        return kernel::Error{kernel::ErrorCode::Unsupported,
                             provider->displayName() + " files can be written but not read."};
    }
    return provider->read(path, options);
}

kernel::Result<void> exportFile(const FormatRegistry& registry, const std::string& path,
                                const kernel::Shape& shape) {
    IFormatProvider* provider = registry.forPath(path);
    if (provider == nullptr) {
        return kernel::Error{kernel::ErrorCode::Unsupported,
                             "I don't know how to save this kind of file.",
                             "no provider registered for " + path};
    }
    if (!provider->capabilities().write) {
        return kernel::Error{kernel::ErrorCode::Unsupported,
                             provider->displayName() + " files can be read but not written."};
    }
    return provider->write(path, shape);
}

}  // namespace cad::io
