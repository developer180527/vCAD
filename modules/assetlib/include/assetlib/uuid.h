#pragma once
#include <array>
#include <string>
#include <cstdint>
#include <functional>

namespace assetlib {

// 128-bit UUID — stable across runs, stored as hex string in the registry.
// Format: "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" (UUID v4 layout)
struct UUID {
    std::array<uint8_t, 16> bytes{};

    bool operator==(const UUID& o) const { return bytes == o.bytes; }
    bool operator!=(const UUID& o) const { return bytes != o.bytes; }
    bool operator< (const UUID& o) const { return bytes <  o.bytes; }

    bool        isNull()   const;
    std::string toString() const; // "a3f7c2d1-e5b8-4xxx-yxxx-xxxxxxxxxxxx"

    // Silent parse — returns null UUID on malformed input.
    // Prefer tryParse() when you need to distinguish failure from null UUID.
    static UUID fromString(const std::string& s);

    // Explicit parse — returns false and leaves out unchanged on any error.
    // Use this when loading UUIDs from files, scene data, or user input.
    static bool tryParse(const std::string& s, UUID& out);

    static UUID generate(); // UUID v4, statistically unique
    static UUID null();     // all-zeros sentinel
};

} // namespace assetlib

template<> struct std::hash<assetlib::UUID> {
    size_t operator()(const assetlib::UUID& u) const noexcept;
};
