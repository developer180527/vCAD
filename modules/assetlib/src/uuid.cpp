#include "assetlib/uuid.h"
#include <random>
#include <cctype>

namespace assetlib {

static std::mt19937_64& rng() {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    return gen;
}

UUID UUID::generate() {
    UUID u;
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t hi = dist(rng());
    uint64_t lo = dist(rng());
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL; // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL; // variant bits
    for (int i = 0; i < 8; ++i) u.bytes[i]   = (hi >> (56 - i*8)) & 0xFF;
    for (int i = 0; i < 8; ++i) u.bytes[8+i] = (lo >> (56 - i*8)) & 0xFF;
    return u;
}

UUID UUID::null()          { UUID u; u.bytes.fill(0); return u; }
bool UUID::isNull() const  { for (auto b : bytes) if (b) return false; return true; }

std::string UUID::toString() const {
    // Lookup table — avoids snprintf format parsing overhead
    static constexpr char kHex[] = "0123456789abcdef";
    // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx  (36 chars + null)
    char buf[37];
    const uint8_t* b = bytes.data();
    int o = 0;
    for (int i : {0,1,2,3})    { buf[o++]=kHex[b[i]>>4]; buf[o++]=kHex[b[i]&0xF]; }
    buf[o++] = '-';
    for (int i : {4,5})        { buf[o++]=kHex[b[i]>>4]; buf[o++]=kHex[b[i]&0xF]; }
    buf[o++] = '-';
    for (int i : {6,7})        { buf[o++]=kHex[b[i]>>4]; buf[o++]=kHex[b[i]&0xF]; }
    buf[o++] = '-';
    for (int i : {8,9})        { buf[o++]=kHex[b[i]>>4]; buf[o++]=kHex[b[i]&0xF]; }
    buf[o++] = '-';
    for (int i : {10,11,12,13,14,15}) { buf[o++]=kHex[b[i]>>4]; buf[o++]=kHex[b[i]&0xF]; }
    buf[o] = '\0';
    return buf;
}

bool UUID::tryParse(const std::string& s, UUID& out) {
    // Strip dashes, validate length and hex chars
    std::string clean;
    clean.reserve(32);
    for (char c : s) {
        if (c == '-') continue;
        if (!std::isxdigit((unsigned char)c)) return false; // non-hex char
        clean += (char)std::tolower((unsigned char)c);
    }
    if (clean.size() != 32) return false; // wrong length

    UUID tmp;
    for (int i = 0; i < 16; ++i) {
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int hi = hexVal(clean[i*2]);
        int lo = hexVal(clean[i*2+1]);
        if (hi < 0 || lo < 0) return false;
        tmp.bytes[i] = (uint8_t)(hi << 4 | lo);
    }
    out = tmp;
    return true;
}

UUID UUID::fromString(const std::string& s) {
    UUID out;
    tryParse(s, out); // leaves out as null UUID on failure
    return out;
}

} // namespace assetlib

// FNV-1a — better bit distribution than polynomial for byte sequences
size_t std::hash<assetlib::UUID>::operator()(const assetlib::UUID& u) const noexcept {
    size_t h = sizeof(size_t) == 8
        ? 0xcbf29ce484222325ULL  // FNV offset basis 64-bit
        : 0x811c9dc5U;           // FNV offset basis 32-bit
    constexpr size_t kPrime = sizeof(size_t) == 8
        ? 0x00000100000001B3ULL  // FNV prime 64-bit
        : 0x01000193U;           // FNV prime 32-bit
    for (auto b : u.bytes) { h ^= b; h *= kPrime; }
    return h;
}
