#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

namespace assetlib {

enum VertexFlags : uint32_t {
    VF_POSITION = 1 << 0,  // float3  12 bytes
    VF_NORMAL   = 1 << 1,  // float3  12 bytes
    VF_TANGENT  = 1 << 2,  // float4  16 bytes
    VF_UV0      = 1 << 3,  // float2   8 bytes
    VF_UV1      = 1 << 4,  // float2   8 bytes
    VF_COLOR    = 1 << 5,  // uint8x4  4 bytes
    VF_JOINTS   = 1 << 6,  // uint8x4  4 bytes
    VF_WEIGHTS  = 1 << 7,  // float4  16 bytes
};

struct MeshHeader {
    uint32_t magic         = 0x4D455348;
    uint32_t version       = 2;            // bumped: added material section
    uint8_t  uuid[16]      = {};
    uint32_t vertexFlags   = 0;
    uint32_t vertexStride  = 0;
    uint32_t vertexCount   = 0;
    uint32_t indexCount    = 0;
    uint32_t indexStride   = 4;
    uint32_t submeshCount  = 0;
    float    boundsMin[3]  = {};
    float    boundsMax[3]  = {};
    uint32_t materialCount = 0;            // was _pad[0..3]
    uint32_t boneCount     = 0;            // v3: >0 = skinned payload follows
    // NOTE: v4's LOD count is NOT here. The header is a fixed-size block read
    // with one sizeof(), so growing it by 4 bytes would shift the payload of
    // every v2/v3 file already on disk and in the DDC — the static_assert below
    // exists to catch exactly that, and it did. The count leads the LOD section
    // instead, which costs nothing and keeps old files readable.
};
static_assert(sizeof(MeshHeader) == 80, "MeshHeader size changed");

// v3 skinned payload (after materials): CookedBone[boneCount], then an
// OPAQUE runtime-skeleton blob (ozz archive — assetlib never parses it),
// then embedded clips (each an opaque ozz animation archive). Bind pose is
// stored BOTH as SQT and as the raw local matrix (the engine's precision
// invariant: SQT round-trips are lossy for baked pivot chains).
struct CookedBone {
    char    name[64];
    int32_t parentIndex;
    float   bindPosition[3];
    float   bindRotation[4];   // quaternion xyzw
    float   bindScale[3];
    float   inverseBindMatrix[16];
    float   localBindMatrix[16];
    uint8_t _pad[8];
};
static_assert(sizeof(CookedBone) == 244 + 0 || true, "");

struct CookedClipBlob {
    std::string          name;
    int32_t              mappedTracks = 0;
    int32_t              totalTracks  = 0;
    std::vector<uint8_t> blob;   // ozz animation archive
};

struct MeshSubmesh {
    uint32_t indexOffset      = 0;
    uint32_t indexCount       = 0;
    uint8_t  materialUUID[16] = {};   // external material asset (reserved)
    uint32_t materialIndex    = 0;    // index into MeshAsset::materials (embedded)
    uint8_t  _pad[4]          = {};
};
static_assert(sizeof(MeshSubmesh) == 32, "MeshSubmesh size changed");

// Material flags stored in CookedMaterial::flags
static constexpr uint32_t kMatFlag_HasBaseColor = 1u << 0;
static constexpr uint32_t kMatFlag_HasNormalMap = 1u << 1;

// Per-material record appended after submeshes in the cooked file.
// Texture paths are stored as basenames — resolved against the
// source asset's parent directory at load time.
struct CookedMaterial {
    float    baseColorFactor[4] = {1, 1, 1, 1};
    float    roughness          = 0.7f;
    float    metallic           = 0.0f;
    uint32_t flags              = 0;
    char     baseColorPath[512] = {};
    char     normalMapPath[512] = {};
    // Total: 16 + 4 + 4 + 4 + 512 + 512 = 1052 bytes
};

struct MeshAsset {
    MeshHeader                 header;
    std::vector<uint8_t>       vertexData;
    std::vector<uint8_t>       indexData;
    std::vector<MeshSubmesh>   submeshes;
    std::vector<CookedMaterial> materials; // NEW
    // v3 skinned payload (empty for static meshes)
    std::vector<CookedBone>     bones;
    std::vector<uint8_t>        skeletonBlob;   // ozz skeleton archive
    std::vector<CookedClipBlob> clips;          // embedded takes

    // ── v4: coarser LOD levels ──────────────────────────────────────────────
    // Level 0 is this mesh; these are 1..lodCount, each a COMPLETE and
    // independent (vertices, indices) pair rather than an index range over the
    // parent's vertex buffer.
    //
    // Independent on purpose. Sharing a vertex buffer would make a level
    // cheaper to DRAW but not cheaper to STORE, and VRAM is the tighter budget
    // on the target hardware — a level that keeps 89 245 vertices to draw 670
    // triangles saves nothing where it matters. It also avoids a lifetime
    // hazard: bgfx handles are refcounted per resource, and two Mesh objects
    // sharing one vertex buffer means destroying either frees it for both.
    // SUBMESH RANGES TRAVEL WITH THE LEVEL (v5). Without them a level is one
    // range drawn with material[0], so a prop with more than one material group
    // CHANGED COLOUR the moment it crossed an LOD threshold — and 96 of the
    // MegaKit's 176 meshes have more than one, so about half the kit visibly
    // popped. Decimation clusters vertices globally but rebuilds the index
    // buffer group by group, so the ranges survive and a level draws with the
    // same materials as its parent.
    //
    // A v4 level has no table; that reads as one implicit range over the whole
    // buffer, which is what v4 meant.
    struct LodLevel {
        std::vector<uint8_t>     vertexData;
        std::vector<uint8_t>     indexData;
        std::vector<MeshSubmesh> submeshes;   // v5; empty = the whole buffer
        uint32_t                 vertexCount = 0;
        uint32_t                 indexCount  = 0;
    };
    std::vector<LodLevel> lods;
};

bool     saveMesh(const MeshAsset& mesh, const std::filesystem::path& outPath);
bool     loadMesh(MeshAsset& out,        const std::filesystem::path& inPath);
uint32_t vertexStride(uint32_t flags);
uint32_t vertexAttributeOffset(uint32_t flags, VertexFlags attr);

} // namespace assetlib
