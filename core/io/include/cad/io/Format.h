#pragma once

#include "cad/kernel/Healing.h"
#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"
#include "cad/units/Units.h"

#include <memory>
#include <string>
#include <vector>

namespace cad::io {

/// What a provider can do. Reported so the UI can tell the user what a conversion will cost
/// BEFORE they run it, rather than after — docs/FORMATS.md rule 1.
struct Capabilities {
    bool read = false;
    bool write = false;
    bool solids = false;     ///< true B-rep; false means the format is mesh-only
    bool assemblies = false;
    bool colours = false;
    bool units = false;      ///< carries a unit declaration we can trust
    bool pmi = false;        ///< semantic GD&T (only AP242 ed3 and QIF, today: neither)
};

/// Everything an import lost, changed, or had to guess. Never discarded silently.
struct ImportReport {
    std::string format;
    std::string sourcePath;

    std::size_t solids = 0;
    std::size_t shells = 0;
    std::size_t faces = 0;

    /// Unit the file declared, and the factor applied. Empty when the format carries no
    /// unit information (STL, DXF), in which case `assumedUnits` was used instead.
    std::string declaredUnits;
    double scaleToMillimetres = 1.0;
    bool unitsWereAssumed = false;

    kernel::HealingReport healing;

    /// Entities we could not represent. Each is a human sentence, not a class name.
    std::vector<std::string> unsupported;
    std::vector<std::string> warnings;

    [[nodiscard]] bool lossless() const noexcept { return unsupported.empty(); }
    [[nodiscard]] std::string summary() const;
};

struct ImportOptions {
    /// Used only when the format declares no units. There is no default on purpose: STL and
    /// DXF carry nothing reliable, and guessing is how a 25.4x scaling error reaches a
    /// customer (docs/FORMATS.md rule 2).
    units::UnitSystem assumedUnits = units::UnitSystem::Millimetre;
    bool heal = true;
    kernel::HealingOptions healing;
};

struct ImportResult {
    kernel::Shape shape;
    ImportReport report;
};

/// One file format. Plugins register these; the core knows nothing about STEP specifically.
///
/// Mirrors OCCT's own DE_Wrapper provider registry (7.8+) deliberately, so that OCCT-native
/// formats are a thin delegation and a third-party format looks identical to the app.
class IFormatProvider {
public:
    virtual ~IFormatProvider() = default;

    [[nodiscard]] virtual std::string id() const = 0;          ///< "step", "3mf"
    [[nodiscard]] virtual std::string displayName() const = 0; ///< "STEP (AP203/214/242)"
    [[nodiscard]] virtual std::vector<std::string> extensions() const = 0;  ///< ".step"
    [[nodiscard]] virtual Capabilities capabilities() const = 0;

    virtual kernel::Result<ImportResult> read(const std::string& path,
                                              const ImportOptions&) = 0;
    virtual kernel::Result<void> write(const std::string& path, const kernel::Shape&) = 0;
};

/// The registry the app and plugins share.
class FormatRegistry {
public:
    FormatRegistry();
    ~FormatRegistry();
    FormatRegistry(FormatRegistry&&) noexcept;
    FormatRegistry& operator=(FormatRegistry&&) noexcept;

    void add(std::shared_ptr<IFormatProvider>);

    /// Provider for a path, chosen by extension. Null if nothing handles it.
    [[nodiscard]] IFormatProvider* forPath(const std::string& path) const;
    [[nodiscard]] IFormatProvider* byId(const std::string& id) const;
    [[nodiscard]] std::vector<IFormatProvider*> all() const;

    /// Everything we can read/write, for a file dialog's filter list.
    [[nodiscard]] std::vector<std::string> readableExtensions() const;
    [[nodiscard]] std::vector<std::string> writableExtensions() const;

    /// The built-ins: everything OCCT gives us without a paid add-on, plus 3MF when it is
    /// compiled in. See docs/FORMATS.md for the tier-2 formats that need a licence.
    static FormatRegistry builtins();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Convenience: pick a provider by extension and read. Runs healing BEFORE any naming, per
/// kernel/Healing.h — healing changes topology, so a shape must never be named first.
kernel::Result<ImportResult> importFile(const FormatRegistry&, const std::string& path,
                                        const ImportOptions& = {});

kernel::Result<void> exportFile(const FormatRegistry&, const std::string& path,
                                const kernel::Shape&);

}  // namespace cad::io
