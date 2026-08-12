#include "assetlib/scene_asset.h"
#include <fstream>
#include <cstdio>

namespace assetlib {

bool saveScene(const SceneAsset& scene, const std::filesystem::path& outPath) {
    std::filesystem::create_directories(outPath.parent_path());
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "[SceneAsset] Cannot write: %s\n",
                     outPath.string().c_str());
        return false;
    }

    // Write header
    SceneHeader hdr = scene.header;
    hdr.magic           = kSceneMagic;
    hdr.version         = kSceneVersion;
    hdr.entityCount     = static_cast<uint32_t>(scene.entities.size());
    hdr.stringTableSize = static_cast<uint32_t>(scene.stringTable.size());
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(SceneHeader));

    // Write entity records (contiguous fixed-size block)
    if (!scene.entities.empty()) {
        f.write(reinterpret_cast<const char*>(scene.entities.data()),
                static_cast<std::streamsize>(
                    scene.entities.size() * sizeof(SceneEntity)));
    }

    // Write string table
    if (!scene.stringTable.empty()) {
        f.write(scene.stringTable.data(),
                static_cast<std::streamsize>(scene.stringTable.size()));
    }

    if (!f) {
        std::fprintf(stderr, "[SceneAsset] Write error: %s\n",
                     outPath.string().c_str());
        return false;
    }

    std::printf("[SceneAsset] Saved %s  entities=%u strings=%u bytes\n",
                outPath.filename().string().c_str(),
                hdr.entityCount, hdr.stringTableSize);
    return true;
}

// A cooked scene is UNTRUSTED input, and it is the most exposed deserializer in
// the engine: a shipped dist opens one with no registry and no source assets to
// fall back on. It can arrive truncated (an interrupted cook, a partial
// download), corrupt (bad disk), or from a shared DDC written by another
// machine. Every count in the 32-byte header is therefore validated against the
// bytes that actually exist BEFORE it sizes an allocation or a read.
//
// Two bugs this guards, both found by tests/fuzz_scene_loader_test.cpp — and
// both already found and fixed in the MESH deserializer by its own fuzz target.
// This loader carried them verbatim, which is the real lesson: a hardening that
// lands in one deserializer and not its sibling is a coincidence, not a policy.
//   1. Unbounded allocation — entityCount and stringTableSize were resize()d
//      straight from the header. The fuzzer asked for 2.7 GB from a 3 482-byte
//      file; entityCount=0xFFFFFFFF asks for 1 TB (256 B per record), and a
//      20-iteration explore run was SIGKILLed by the OOM killer.
//   2. Truncation accepted — `return f.good() || f.eof()` treated a short read
//      as success, handing back full-size zero-filled records while the header
//      still claimed the original counts. A half-written scene loaded clean.
bool loadScene(SceneAsset& out, const std::filesystem::path& inPath) {
    std::error_code ec;
    const auto fileSizeRaw = std::filesystem::file_size(inPath, ec);
    if (ec) {
        std::fprintf(stderr, "[SceneAsset] Cannot stat: %s\n",
                     inPath.string().c_str());
        return false;
    }
    const uint64_t fileSize = static_cast<uint64_t>(fileSizeRaw);

    std::ifstream f(inPath, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[SceneAsset] Cannot open: %s\n",
                     inPath.string().c_str());
        return false;
    }
    if (fileSize < sizeof(SceneHeader)) {
        std::fprintf(stderr, "[SceneAsset] Truncated (no header): %s\n",
                     inPath.string().c_str());
        return false;
    }

    // Read header
    f.read(reinterpret_cast<char*>(&out.header), sizeof(SceneHeader));
    if (!f || out.header.magic != kSceneMagic) {
        std::fprintf(stderr, "[SceneAsset] Bad magic: %s\n",
                     inPath.string().c_str());
        return false;
    }
    if (out.header.version != kSceneVersion) {
        std::fprintf(stderr, "[SceneAsset] Unsupported version %u: %s\n",
                     out.header.version, inPath.string().c_str());
        return false;
    }

    // ── The declared layout must fit in the bytes that exist ────────────────
    // 64-bit arithmetic throughout: both counts are uint32_t and
    // entityCount * sizeof(SceneEntity) overflows 32 bits at 16.7M entities,
    // so a 32-bit check would pass for a file demanding gigabytes.
    const uint64_t needEntities = static_cast<uint64_t>(out.header.entityCount)
                                * sizeof(SceneEntity);
    const uint64_t declared = sizeof(SceneHeader) + needEntities
                            + static_cast<uint64_t>(out.header.stringTableSize);
    if (declared > fileSize) {
        std::fprintf(stderr,
                     "[SceneAsset] Truncated or corrupt: header declares %llu B "
                     "(%u entities, %u B strings) but the file is %llu B: %s\n",
                     (unsigned long long)declared, out.header.entityCount,
                     out.header.stringTableSize, (unsigned long long)fileSize,
                     inPath.string().c_str());
        return false;
    }

    // Only now is a header count safe to allocate from.
    out.entities.resize(out.header.entityCount);
    if (out.header.entityCount > 0) {
        f.read(reinterpret_cast<char*>(out.entities.data()),
               static_cast<std::streamsize>(needEntities));
    }

    out.stringTable.resize(out.header.stringTableSize);
    if (out.header.stringTableSize > 0) {
        f.read(out.stringTable.data(),
               static_cast<std::streamsize>(out.header.stringTableSize));
    }

    // `f.eof()` is deliberately NOT accepted: that is precisely what let
    // truncated files through. The size check above already proved the bytes
    // are there, so a short read here means the file changed underneath us.
    if (!f.good()) {
        std::fprintf(stderr, "[SceneAsset] Short read: %s\n",
                     inPath.string().c_str());
        return false;
    }
    return true;
}

} // namespace assetlib
