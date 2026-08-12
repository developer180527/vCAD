#pragma once
#include <cstdlib>

// Internal: environment knobs shared by the cook TUs (pipeline, scheduler,
// worker dispatch). Not a public header.
namespace assetlib {

// Reads a positive integer environment override; returns fallback when
// unset, empty, non-numeric, or <= 0.
inline long envLong(const char* name, long fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    return (end && *end == '\0' && n > 0) ? n : fallback;
}

} // namespace assetlib
