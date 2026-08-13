#pragma once

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

/// Logging for the whole application.
///
/// In `core/` and dependency-free on purpose: the kernel, the solver and `abi/` all need it, and it
/// has to compile for the iPad target where there is no Qt. Nothing here includes a toolkit.
///
/// Filtering is by CATEGORY as much as by level, because the useful question in this application is
/// which subsystem, not how bad. A fillet failure and a file-parse failure are both warnings and are
/// never interesting at the same time.
namespace cad::log {

enum class Level : std::uint8_t { Trace = 0, Debug, Info, Warning, Error, Off };

/// Subsystems. `ThirdParty` exists so OCCT, dime and planegcs diagnostics land here rather than on
/// a terminal nobody is watching — today they go to stderr unfiltered, or are suppressed entirely.
enum class Category : std::uint8_t {
    Kernel, Naming, Document, Recompute, Io, Sketch, Render, App, Shell, Plugin, ThirdParty,
    Count
};

[[nodiscard]] const char* toString(Level) noexcept;
[[nodiscard]] const char* toString(Category) noexcept;

struct Record {
    Level level = Level::Info;
    Category category = Category::App;
    /// Microseconds since the epoch. Not a formatted string: a sink writing to a file wants a
    /// different format from one feeding a future crash report's breadcrumb ring.
    std::int64_t timestampUs = 0;
    std::uint64_t thread = 0;
    const char* file = "";     ///< literal from __FILE__, never owned
    int line = 0;
    std::string message;
};

class ISink {
public:
    virtual ~ISink() = default;
    /// Called with the global lock held, so a sink need not be thread-safe itself. It must not call
    /// back into the log — that deadlocks, and a sink that logs its own failure is the usual way in.
    virtual void write(const Record&) = 0;
};

/// Writes to stderr. Added by default so a fresh process is never silent.
[[nodiscard]] std::shared_ptr<ISink> stderrSink();

/// Rotating file sink. Rotates to `path.1` at `maxBytes` and keeps one generation.
///
/// One generation, not ten: this file's job is to be attachable to a bug report, and a user asked
/// for "the log" will send the newest. Deep history belongs in a crash report's breadcrumb ring.
[[nodiscard]] std::shared_ptr<ISink> fileSink(const std::string& path,
                                             std::size_t maxBytes = 8u * 1024u * 1024u);

void addSink(std::shared_ptr<ISink>);
void clearSinks();

/// Per-category threshold. Below it, a call site costs one relaxed load and a predicted branch —
/// which is what lets `recompute` log per feature and `sketch` per solve without being a cost.
void setLevel(Category, Level);
void setLevel(Level);                        ///< all categories
[[nodiscard]] bool enabled(Category, Level) noexcept;

void write(Record);

/// Built by the macros below. Formats into a local buffer and emits once on destruction, so a
/// half-written line can never interleave with another thread's.
class Entry {
public:
    Entry(Level level, Category category, const char* file, int line);
    ~Entry();
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;

    template <class T>
    Entry& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }

private:
    std::ostringstream stream_;
    Record record_;
};

}  // namespace cad::log

/// The `if/else` shape is deliberate: it short-circuits BEFORE constructing the Entry, so a
/// disabled category never builds a stream. The dangling-else guard matters because these appear
/// inside unbraced ifs in real code.
#define CAD_LOG(category, level)                                            \
    if (!::cad::log::enabled(category, level)) {                            \
    } else                                                                  \
        ::cad::log::Entry(level, category, __FILE__, __LINE__)

#define CAD_TRACE(cat) CAD_LOG(cat, ::cad::log::Level::Trace)
#define CAD_DEBUG(cat) CAD_LOG(cat, ::cad::log::Level::Debug)
#define CAD_INFO(cat)  CAD_LOG(cat, ::cad::log::Level::Info)
#define CAD_WARN(cat)  CAD_LOG(cat, ::cad::log::Level::Warning)
#define CAD_ERROR(cat) CAD_LOG(cat, ::cad::log::Level::Error)
