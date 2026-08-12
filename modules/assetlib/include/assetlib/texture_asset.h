#pragma once
#include <cstdint>
#include <vector>
#include <filesystem>

namespace assetlib {

// GPU-ready pixel formats. Cooked offline into hardware block-compressed
// data + a full mip chain, so the runtime does ZERO CPU work: read the
// header, hand the byte blocks straight to the GPU. Raw RGBA32 (v1) was a
// VRAM/bandwidth trap — 64MB per 4k texture, no mips, cold texture caches.
enum TextureFormatId : uint32_t {
    kTexRGBA8 = 0,   // v1 legacy — raw, no mips
    kTexBC7   = 1,   // color HQ: near-lossless 4:1 — SLOW encode (final bake)
    kTexBC5   = 2,   // normal maps: two-channel XY, Z reconstructed in-shader
    kTexBC1   = 3,   // color, opaque: 8:1, fast squish (iteration default)
    kTexBC3   = 4,   // color, alpha: 4:1, fast squish (iteration default)
};

// Bytes per 4x4 block. BC1 is 8; BC3/BC5/BC7 are 16.
inline uint32_t bcBytesPerBlock(uint32_t fmt) {
    return fmt == kTexBC1 ? 8u : 16u;
}

struct TextureHeader {
    uint32_t magic    = 0x54455820; // 'TEX '
    uint32_t version  = 2;          // v2: format + mipCount (v1 pads were 0)
    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t channels = 4;
    uint32_t format   = kTexRGBA8;  // TextureFormatId
    uint32_t mipCount = 1;          // 0 (v1 pad) reads as 1
    uint8_t  _pad[4]  = {};
};
static_assert(sizeof(TextureHeader) == 32, "TextureHeader size changed");

struct TextureAsset {
    TextureHeader        header;
    // kTexRGBA8: raw width*height*4.
    // BC formats: every mip packed contiguous (mip0..mipN), each mip
    // ceil(w/4)*ceil(h/4)*bcBytesPerBlock(format) bytes at that mip's
    // dimensions (BC1 = 8, BC3/BC5/BC7 = 16) — exactly the layout bgfx
    // expects for a pre-mipped texture upload.
    std::vector<uint8_t> pixels;
};

bool saveTexture(const TextureAsset& tex, const std::filesystem::path& outPath);
bool loadTexture(TextureAsset& out,       const std::filesystem::path& inPath);

} // namespace assetlib
