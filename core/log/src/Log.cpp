#include "cad/log/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace cad::log {
namespace {

constexpr std::size_t kCategories = static_cast<std::size_t>(Category::Count);

/// Thresholds are atomics read on every call site, so setLevel from another thread is safe and
/// cheap. Relaxed ordering: a log line appearing one instruction late is not worth a fence.
std::atomic<std::uint8_t> g_levels[kCategories] = {};

std::mutex g_mutex;
std::vector<std::shared_ptr<ISink>> g_sinks;
bool g_initialised = false;

std::int64_t nowMicroseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Trailing component only. Full paths are build-machine specific, differ between developers, and
/// make every line long enough to wrap in a terminal.
const char* shortFile(const char* path) {
    if (path == nullptr) return "";
    const char* slash = std::strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

class StderrSink final : public ISink {
public:
    void write(const Record& r) override {
        std::fprintf(stderr, "%-5s %-10s %s:%d  %s\n", toString(r.level), toString(r.category),
                     shortFile(r.file), r.line, r.message.c_str());
    }
};

class FileSink final : public ISink {
public:
    FileSink(std::string path, std::size_t maxBytes)
        : path_(std::move(path)), maxBytes_(maxBytes) {
        open();
    }
    ~FileSink() override {
        if (file_ != nullptr) std::fclose(file_);
    }

    void write(const Record& r) override {
        if (file_ == nullptr) return;
        const std::int64_t seconds = r.timestampUs / 1000000;
        const int millis = static_cast<int>((r.timestampUs % 1000000) / 1000);
        char stamp[32];
        std::tm tm{};
        const std::time_t t = static_cast<std::time_t>(seconds);
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

        const int written =
            std::fprintf(file_, "%s.%03d %-5s %-10s [%llu] %s:%d  %s\n", stamp, millis,
                         toString(r.level), toString(r.category),
                         static_cast<unsigned long long>(r.thread), shortFile(r.file), r.line,
                         r.message.c_str());
        // Flushed every line. A log that loses its last buffer is worthless precisely when it
        // matters -- the crash is what stopped the flush.
        std::fflush(file_);
        if (written > 0) {
            bytes_ += static_cast<std::size_t>(written);
            if (bytes_ >= maxBytes_) rotate();
        }
    }

private:
    void open() {
        file_ = std::fopen(path_.c_str(), "ab");
        if (file_ != nullptr) {
            std::error_code ec;
            bytes_ = static_cast<std::size_t>(std::filesystem::file_size(path_, ec));
            if (ec) bytes_ = 0;
        }
    }
    void rotate() {
        if (file_ != nullptr) std::fclose(file_);
        std::error_code ec;
        std::filesystem::rename(path_, path_ + ".1", ec);   // replaces any previous generation
        bytes_ = 0;
        open();
    }

    std::string path_;
    std::size_t maxBytes_ = 0;
    std::size_t bytes_ = 0;
    std::FILE* file_ = nullptr;
};

/// Adds the stderr sink on first use, so a process that never configures logging still shows
/// errors rather than swallowing them. Called with g_mutex held.
void ensureInitialised() {
    if (g_initialised) return;
    g_initialised = true;
    g_sinks.push_back(std::make_shared<StderrSink>());
}

}  // namespace

const char* toString(Level l) noexcept {
    switch (l) {
        case Level::Trace:   return "TRACE";
        case Level::Debug:   return "DEBUG";
        case Level::Info:    return "INFO";
        case Level::Warning: return "WARN";
        case Level::Error:   return "ERROR";
        case Level::Off:     return "OFF";
    }
    return "?";
}

const char* toString(Category c) noexcept {
    switch (c) {
        case Category::Kernel:     return "kernel";
        case Category::Naming:     return "naming";
        case Category::Document:   return "document";
        case Category::Recompute:  return "recompute";
        case Category::Io:         return "io";
        case Category::Sketch:     return "sketch";
        case Category::Render:     return "render";
        case Category::App:        return "app";
        case Category::Shell:      return "shell";
        case Category::Plugin:     return "plugin";
        case Category::ThirdParty: return "3rdparty";
        case Category::Count:      break;
    }
    return "?";
}

std::shared_ptr<ISink> stderrSink() { return std::make_shared<StderrSink>(); }

std::shared_ptr<ISink> fileSink(const std::string& path, std::size_t maxBytes) {
    return std::make_shared<FileSink>(path, maxBytes);
}

void addSink(std::shared_ptr<ISink> sink) {
    if (!sink) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureInitialised();
    g_sinks.push_back(std::move(sink));
}

void clearSinks() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sinks.clear();
    g_initialised = true;   // stays cleared: silence was asked for explicitly
}

void setLevel(Category c, Level l) {
    if (c == Category::Count) return;
    g_levels[static_cast<std::size_t>(c)].store(static_cast<std::uint8_t>(l),
                                                std::memory_order_relaxed);
}

void setLevel(Level l) {
    for (std::size_t i = 0; i < kCategories; ++i) {
        g_levels[i].store(static_cast<std::uint8_t>(l), std::memory_order_relaxed);
    }
}

bool enabled(Category c, Level l) noexcept {
    if (c == Category::Count) return false;
    return static_cast<std::uint8_t>(l)
           >= g_levels[static_cast<std::size_t>(c)].load(std::memory_order_relaxed);
}

void write(Record record) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureInitialised();
    for (const auto& sink : g_sinks) sink->write(record);
}

Entry::Entry(Level level, Category category, const char* file, int line) {
    record_.level = level;
    record_.category = category;
    record_.file = file;
    record_.line = line;
    record_.timestampUs = nowMicroseconds();
    record_.thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
}

Entry::~Entry() {
    record_.message = stream_.str();
    write(std::move(record_));
}

}  // namespace cad::log
