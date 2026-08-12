#include "assetlib/mesh_asset.h"
#include <fstream>
#include <cstring>
#include <cstdio>
#include <cstdint>   // SIZE_MAX

namespace assetlib {

static const struct { uint32_t flag; uint32_t size; } kAttribs[] = {
    {VF_POSITION,12},{VF_NORMAL,12},{VF_TANGENT,16},
    {VF_UV0,8},{VF_UV1,8},{VF_COLOR,4},{VF_JOINTS,4},{VF_WEIGHTS,16},
};

uint32_t vertexStride(uint32_t flags) {
    uint32_t s = 0;
    for (auto& a : kAttribs) if (flags & a.flag) s += a.size;
    return s;
}

uint32_t vertexAttributeOffset(uint32_t flags, VertexFlags attr) {
    uint32_t off = 0;
    for (auto& a : kAttribs) {
        if (a.flag == static_cast<uint32_t>(attr)) break;
        if (flags & a.flag) off += a.size;
    }
    return off;
}

bool saveMesh(const MeshAsset& mesh, const std::filesystem::path& outPath) {
    std::filesystem::create_directories(outPath.parent_path());
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "[MeshAsset] Cannot write: %s\n",
                     outPath.string().c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&mesh.header),      sizeof(MeshHeader));
    f.write(reinterpret_cast<const char*>(mesh.vertexData.data()),
            static_cast<std::streamsize>(mesh.vertexData.size()));
    f.write(reinterpret_cast<const char*>(mesh.indexData.data()),
            static_cast<std::streamsize>(mesh.indexData.size()));
    f.write(reinterpret_cast<const char*>(mesh.submeshes.data()),
            static_cast<std::streamsize>(mesh.submeshes.size() * sizeof(MeshSubmesh)));
    // Material section — always written; materialCount in header says how many
    f.write(reinterpret_cast<const char*>(mesh.materials.data()),
            static_cast<std::streamsize>(mesh.materials.size() * sizeof(CookedMaterial)));

    // v3 skinned payload
    if (mesh.header.boneCount > 0) {
        f.write(reinterpret_cast<const char*>(mesh.bones.data()),
                (std::streamsize)(mesh.bones.size() * sizeof(CookedBone)));
        const uint32_t skelSize = (uint32_t)mesh.skeletonBlob.size();
        f.write(reinterpret_cast<const char*>(&skelSize), 4);
        f.write(reinterpret_cast<const char*>(mesh.skeletonBlob.data()), skelSize);
        const uint32_t clipCount = (uint32_t)mesh.clips.size();
        f.write(reinterpret_cast<const char*>(&clipCount), 4);
        for (const auto& c : mesh.clips) {
            const uint32_t nameLen = (uint32_t)c.name.size();
            f.write(reinterpret_cast<const char*>(&nameLen), 4);
            f.write(c.name.data(), nameLen);
            f.write(reinterpret_cast<const char*>(&c.mappedTracks), 4);
            f.write(reinterpret_cast<const char*>(&c.totalTracks), 4);
            const uint32_t blobSize = (uint32_t)c.blob.size();
            f.write(reinterpret_cast<const char*>(&blobSize), 4);
            f.write(reinterpret_cast<const char*>(c.blob.data()), blobSize);
        }
    }

    // v4: coarser LOD levels, each self-contained. The count leads the section
    // rather than living in the fixed-size header — see the note there.
    if (mesh.header.version >= 4) {
        const uint32_t lodCount = (uint32_t)mesh.lods.size();
        f.write(reinterpret_cast<const char*>(&lodCount), 4);
    }
    for (const auto& l : mesh.lods) {
        const uint32_t vcount = l.vertexCount;
        const uint32_t vbytes = (uint32_t)l.vertexData.size();
        const uint32_t icount = l.indexCount;
        const uint32_t ibytes = (uint32_t)l.indexData.size();
        const uint32_t rcount = (uint32_t)l.submeshes.size();
        f.write(reinterpret_cast<const char*>(&vcount), 4);
        f.write(reinterpret_cast<const char*>(&vbytes), 4);
        f.write(reinterpret_cast<const char*>(&icount), 4);
        f.write(reinterpret_cast<const char*>(&ibytes), 4);
        // v5 only, and the reader keys off the version rather than sniffing:
        // a v4 file that grew a fifth field would be indistinguishable from a
        // corrupt one.
        if (mesh.header.version >= 5)
            f.write(reinterpret_cast<const char*>(&rcount), 4);
        if (vbytes) f.write(reinterpret_cast<const char*>(l.vertexData.data()), vbytes);
        if (ibytes) f.write(reinterpret_cast<const char*>(l.indexData.data()), ibytes);
        if (mesh.header.version >= 5 && rcount)
            f.write(reinterpret_cast<const char*>(l.submeshes.data()),
                    (std::streamsize)(rcount * sizeof(MeshSubmesh)));
    }

    if (!f) {
        std::fprintf(stderr, "[MeshAsset] Write error: %s\n",
                     outPath.string().c_str());
        return false;
    }
    std::printf("[MeshAsset] Saved %s  verts=%u idx=%u submeshes=%u mats=%u lods=%u\n",
        outPath.filename().string().c_str(),
        mesh.header.vertexCount, mesh.header.indexCount,
        mesh.header.submeshCount, mesh.header.materialCount,
        (uint32_t)mesh.lods.size());
    return true;
}

// A cooked mesh is UNTRUSTED input: it can arrive truncated (an interrupted
// cook, a partial download), corrupt (bad disk), or from a shared DDC written
// by another machine. Every count and stride in the header therefore has to be
// validated against the bytes that actually exist BEFORE it is used to size an
// allocation or a read.
//
// Two bugs this guards, both found by tests/fuzz_mesh_loader_test.cpp:
//   1. Unbounded allocation — every section was resize()d straight from a
//      header field. materialCount=0xFFFFFFFF asks for 4.5 TB (1052 B each),
//      vertexCount*vertexStride can ask for exabytes; the process thrashes
//      itself to death on a single malformed file.
//   2. Truncation accepted — the old `return f.good() || f.eof()` treated a
//      short read as success, handing back full-size zero-filled buffers while
//      the header still claimed the original counts. `src/assets/issues.md`
//      recorded this class and it was fixed in mesh_loader.cpp (the bgfx
//      path), but never here — and this is the deserializer the runtime
//      actually streams through.
bool loadMesh(MeshAsset& out, const std::filesystem::path& inPath) {
    std::error_code ec;
    const auto fileSizeRaw = std::filesystem::file_size(inPath, ec);
    if (ec) {
        std::fprintf(stderr, "[MeshAsset] Cannot stat: %s\n",
                     inPath.string().c_str());
        return false;
    }
    const size_t fileSize = static_cast<size_t>(fileSizeRaw);

    std::ifstream f(inPath, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[MeshAsset] Cannot open: %s\n",
                     inPath.string().c_str());
        return false;
    }
    if (fileSize < sizeof(MeshHeader)) {
        std::fprintf(stderr, "[MeshAsset] Truncated (no header): %s\n",
                     inPath.string().c_str());
        return false;
    }
    f.read(reinterpret_cast<char*>(&out.header), sizeof(MeshHeader));
    if (!f || out.header.magic != 0x4D455348) {
        std::fprintf(stderr, "[MeshAsset] Bad magic: %s\n",
                     inPath.string().c_str());
        return false;
    }
    if (out.header.version < 2 || out.header.version > 5) {
        std::fprintf(stderr, "[MeshAsset] Unsupported version %u: %s\n",
                     out.header.version, inPath.string().c_str());
        return false;
    }

    // Reserves `count * elemSize` bytes of the remaining file, or fails. This
    // single check is what makes every resize() below safe: allocation is
    // bounded by the file that exists, not by what the header wishes for.
    size_t offset = sizeof(MeshHeader);
    auto claim = [&](size_t count, size_t elemSize, const char* what) -> bool {
        if (elemSize != 0 && count > (SIZE_MAX / elemSize)) {
            std::fprintf(stderr, "[MeshAsset] %s size overflows: %s\n",
                         what, inPath.string().c_str());
            return false;
        }
        const size_t bytes = count * elemSize;
        if (bytes > fileSize - offset) {
            std::fprintf(stderr, "[MeshAsset] %s declares %zu B but only %zu B "
                         "remain: %s\n", what, bytes, fileSize - offset,
                         inPath.string().c_str());
            return false;
        }
        offset += bytes;
        return true;
    };

    // Strides are format constants, not free-form numbers. An indexStride of
    // 0/1/3 was previously reinterpreted as 16-bit, silently scrambling
    // geometry rather than failing.
    if (out.header.indexCount > 0 &&
        out.header.indexStride != 2 && out.header.indexStride != 4) {
        std::fprintf(stderr, "[MeshAsset] Invalid indexStride %u (must be 2 or "
                     "4): %s\n", out.header.indexStride, inPath.string().c_str());
        return false;
    }
    if (out.header.vertexCount > 0 && out.header.vertexStride == 0) {
        std::fprintf(stderr, "[MeshAsset] vertexStride is 0 with %u vertices: "
                     "%s\n", out.header.vertexCount, inPath.string().c_str());
        return false;
    }

    if (!claim(out.header.vertexCount, out.header.vertexStride, "vertex data"))
        return false;
    out.vertexData.resize((size_t)out.header.vertexCount * out.header.vertexStride);
    f.read(reinterpret_cast<char*>(out.vertexData.data()),
           static_cast<std::streamsize>(out.vertexData.size()));

    if (!claim(out.header.indexCount, out.header.indexStride, "index data"))
        return false;
    out.indexData.resize((size_t)out.header.indexCount * out.header.indexStride);
    f.read(reinterpret_cast<char*>(out.indexData.data()),
           static_cast<std::streamsize>(out.indexData.size()));

    if (!claim(out.header.submeshCount, sizeof(MeshSubmesh), "submeshes"))
        return false;
    out.submeshes.resize(out.header.submeshCount);
    f.read(reinterpret_cast<char*>(out.submeshes.data()),
           static_cast<std::streamsize>(out.submeshes.size() * sizeof(MeshSubmesh)));

    // Every submesh draw range must lie inside the index buffer. The renderer
    // issues a draw straight from these numbers, so a corrupt record (a single
    // flipped bit is enough) otherwise becomes an out-of-bounds GPU read.
    // 64-bit sum so offset+count cannot wrap.
    for (const auto& s : out.submeshes) {
        if ((uint64_t)s.indexOffset + s.indexCount > out.header.indexCount) {
            std::fprintf(stderr, "[MeshAsset] Submesh range [%u,%llu) exceeds "
                         "indexCount %u: %s\n", s.indexOffset,
                         (unsigned long long)s.indexOffset + s.indexCount,
                         out.header.indexCount, inPath.string().c_str());
            return false;
        }
    }

    // Material section (version 2+)
    if (out.header.materialCount > 0) {
        if (!claim(out.header.materialCount, sizeof(CookedMaterial), "materials"))
            return false;
        out.materials.resize(out.header.materialCount);
        f.read(reinterpret_cast<char*>(out.materials.data()),
               static_cast<std::streamsize>(
                   out.materials.size() * sizeof(CookedMaterial)));
    }

    // v3 skinned payload (older files / static meshes: boneCount == 0)
    if (out.header.version >= 3 && out.header.boneCount > 0) {
        if (!claim(out.header.boneCount, sizeof(CookedBone), "bones")) return false;
        out.bones.resize(out.header.boneCount);
        f.read(reinterpret_cast<char*>(out.bones.data()),
               static_cast<std::streamsize>(out.bones.size() * sizeof(CookedBone)));

        uint32_t skelSize = 0;
        if (!claim(1, 4, "skeleton size")) return false;
        f.read(reinterpret_cast<char*>(&skelSize), 4);
        if (!claim(skelSize, 1, "skeleton blob")) return false;
        out.skeletonBlob.resize(skelSize);
        f.read(reinterpret_cast<char*>(out.skeletonBlob.data()), skelSize);

        uint32_t clipCount = 0;
        if (!claim(1, 4, "clip count")) return false;
        f.read(reinterpret_cast<char*>(&clipCount), 4);
        // Each clip costs at least its four length fields, so the remaining
        // bytes bound how many can exist — this stops a huge clipCount from
        // allocating before a single clip has been read.
        if (!claim(clipCount, 16, "clip headers")) return false;
        offset -= (size_t)clipCount * 16;        // re-counted precisely below
        out.clips.resize(clipCount);
        for (auto& c : out.clips) {
            uint32_t nameLen = 0;
            if (!claim(1, 4, "clip name length")) return false;
            f.read(reinterpret_cast<char*>(&nameLen), 4);
            if (!claim(nameLen, 1, "clip name")) return false;
            c.name.resize(nameLen);
            f.read(c.name.data(), nameLen);

            if (!claim(2, 4, "clip track counts")) return false;
            f.read(reinterpret_cast<char*>(&c.mappedTracks), 4);
            f.read(reinterpret_cast<char*>(&c.totalTracks), 4);

            uint32_t blobSize = 0;
            if (!claim(1, 4, "clip blob size")) return false;
            f.read(reinterpret_cast<char*>(&blobSize), 4);
            if (!claim(blobSize, 1, "clip blob")) return false;
            c.blob.resize(blobSize);
            f.read(reinterpret_cast<char*>(c.blob.data()), blobSize);
        }
    }

    // ── v4: coarser LOD levels ──────────────────────────────────────────────
    // Same claim() budget as everything else: a corrupt lodCount must not
    // allocate before a single byte has been read. Cooked meshes travel through
    // a shared DDC, so these bytes are another machine's.
    if (out.header.version >= 4) {
        // v5 added a per-level submesh range table. v4 files are still out
        // there in project caches and in the DDC, and they are perfectly good
        // geometry — they simply have no ranges, which reads as "one range, the
        // whole buffer", exactly what they meant. Rejecting them would break
        // every project until a full re-cook for no gain.
        const bool hasRanges = out.header.version >= 5;
        const size_t levelHeaderBytes = hasRanges ? 20 : 16;

        uint32_t lodCount = 0;
        if (!claim(1, 4, "lod count")) return false;
        f.read(reinterpret_cast<char*>(&lodCount), 4);
        if (!claim(lodCount, levelHeaderBytes, "lod headers")) return false;
        offset -= (size_t)lodCount * levelHeaderBytes;   // re-counted precisely below
        out.lods.resize(lodCount);
        for (auto& l : out.lods) {
            if (!claim(hasRanges ? 5 : 4, 4, "lod header")) return false;
            uint32_t vcount = 0, vbytes = 0, icount = 0, ibytes = 0, rcount = 0;
            f.read(reinterpret_cast<char*>(&vcount), 4);
            f.read(reinterpret_cast<char*>(&vbytes), 4);
            f.read(reinterpret_cast<char*>(&icount), 4);
            f.read(reinterpret_cast<char*>(&ibytes), 4);
            if (hasRanges) f.read(reinterpret_cast<char*>(&rcount), 4);

            // ── The COUNT must agree with the BYTES ─────────────────────────
            // For every other section in this file the two cannot disagree,
            // because the byte size is DERIVED from the count
            // (vertexData.resize(vertexCount * vertexStride)). The LOD section
            // stores both independently, so it needs the cross-check the others
            // get for free — and it is not a nicety: `Mesh lm(lvb, lib,
            // lvl.indexCount)` in AssetService hands indexCount straight to
            // bgfx as a draw range, so `icount = 0x40000000` with twelve bytes
            // of index data is a GPU read off the end of the buffer.
            const uint32_t vs = out.header.vertexStride;
            const uint32_t is = out.header.indexStride;
            auto exactly = [](uint32_t count, uint32_t stride, uint32_t bytes) {
                if (count == 0) return bytes == 0;
                if (stride == 0) return false;               // no valid width
                if (count > UINT32_MAX / stride) return false;   // would overflow
                return count * stride == bytes;
            };
            if (!exactly(vcount, vs, vbytes) || !exactly(icount, is, ibytes)) {
                std::fprintf(stderr, "[MeshAsset] LOD level declares %u verts /"
                             " %u B (stride %u) and %u indices / %u B (stride %u)"
                             " — counts do not match the payload: %s\n",
                             vcount, vbytes, vs, icount, ibytes, is,
                             inPath.string().c_str());
                return false;
            }

            if (!claim(vbytes, 1, "lod vertices")) return false;
            l.vertexData.resize(vbytes);
            f.read(reinterpret_cast<char*>(l.vertexData.data()), vbytes);
            if (!claim(ibytes, 1, "lod indices")) return false;
            l.indexData.resize(ibytes);
            f.read(reinterpret_cast<char*>(l.indexData.data()), ibytes);

            // Submesh ranges, so a level keeps its material groups (see the
            // LodLevel note in mesh_asset.h). Every range must address the
            // level's OWN index buffer — a range reaching past it is the same
            // out-of-bounds draw as a bad indexCount, one indirection along.
            if (!claim(rcount, sizeof(MeshSubmesh), "lod submeshes")) return false;
            l.submeshes.resize(rcount);
            if (rcount) {
                f.read(reinterpret_cast<char*>(l.submeshes.data()),
                       (std::streamsize)(rcount * sizeof(MeshSubmesh)));
                for (const auto& s : l.submeshes) {
                    if (s.indexCount > icount ||
                        s.indexOffset > icount - s.indexCount) {
                        std::fprintf(stderr, "[MeshAsset] LOD submesh range "
                                     "[%u,+%u) is outside the level's %u indices:"
                                     " %s\n", s.indexOffset, s.indexCount, icount,
                                     inPath.string().c_str());
                        return false;
                    }
                }
            }

            l.vertexCount = vcount;
            l.indexCount  = icount;
        }
    }

    // Every read above was pre-validated to fit, so a stream failure here means
    // the file changed under us or the disk errored — either way, not usable.
    // (`f.eof()` is deliberately NOT accepted: that is what let truncated files
    // through as success.)
    if (!f) {
        std::fprintf(stderr, "[MeshAsset] Short read: %s\n",
                     inPath.string().c_str());
        return false;
    }
    return true;
}

} // namespace assetlib
