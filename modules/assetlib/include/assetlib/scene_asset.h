#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <filesystem>

namespace assetlib {

// ═══════════════════════════════════════════════════════════════════════════
// Binary scene format — "SCNE" (0x53434E45)
//
// Layout:
//   [SceneHeader]                     fixed 32 bytes
//   [SceneEntity * entityCount]       fixed 256 bytes each
//   [string table]                    packed null-terminated strings
//
// Entity records are fixed-size so the loader can memcpy a contiguous block.
// Variable-length data (names, paths) are stored as offsets into the string
// table at the end of the file. A componentMask bitfield tells which
// components are meaningful; unused fields are zeroed.
//
// This format is the RUNTIME counterpart of the editor's JSON .scene files.
// The editor cook pipeline bakes JSON → binary; the SceneService loads binary.
// ═══════════════════════════════════════════════════════════════════════════

static constexpr uint32_t kSceneMagic   = 0x53434E45;  // "SCNE"
static constexpr uint32_t kSceneVersion = 3;           // v3: material BY NAME

// Component presence flags (bitfield for SceneEntity::componentMask)
enum SceneComponentBit : uint32_t {
    kComp_Transform           = 1u << 0,
    kComp_Name                = 1u << 1,
    kComp_MeshRenderer        = 1u << 2,
    kComp_Camera              = 1u << 3,
    kComp_RigidBody           = 1u << 4,
    kComp_Script              = 1u << 5,
    kComp_CharacterController = 1u << 6,
    kComp_Light               = 1u << 7,
    kComp_Animator            = 1u << 8,   // v2
};

struct SceneHeader {
    uint32_t magic            = kSceneMagic;
    uint32_t version          = kSceneVersion;
    uint32_t entityCount      = 0;
    uint32_t stringTableSize  = 0;   // total bytes in the string table
    uint8_t  _pad[16]         = {};  // reserved for future use
};
static_assert(sizeof(SceneHeader) == 32, "SceneHeader size changed");

// Fixed-size entity record (256 bytes). All component data is inlined.
// Strings are stored as (offset, length) pairs into the string table.
struct SceneEntity {
    // ── Identity ──
    uint64_t entityId        = 0;
    uint64_t parentId        = 0;
    uint32_t componentMask   = 0;
    uint8_t  _pad0[4]        = {};

    // ── Transform (28 bytes) ──
    float position[3]        = {0, 0, 0};
    float rotation[4]        = {0, 0, 0, 1};  // quaternion (x,y,z,w)
    float scale[3]           = {1, 1, 1};

    // ── Name (8 bytes → string table) ──
    uint32_t nameOffset      = 0xFFFFFFFF;  // into string table (0xFFFFFFFF = none)
    uint32_t nameLength      = 0;

    // ── MeshRenderer (24 bytes) ──
    uint32_t meshCookedOffset  = 0xFFFFFFFF;
    uint32_t meshCookedLength  = 0;
    uint32_t meshSourceOffset  = 0xFFFFFFFF;  // editor fallback
    uint32_t meshSourceLength  = 0;
    // v3: the material asset's AUTHORED NAME, into the string table
    // (0xFFFFFFFF = none). Was `matOverrideId`, a session-local MaterialHandle
    // slot index — persisting that to disk was meaningless: handles are indices
    // over a free list with no generation, so a reloaded scene got whichever
    // material happened to occupy the slot. Same width, real reference.
    // Null-terminated in the table, so no length field is needed (there is no
    // room: SceneEntity is exactly 256 bytes with zero tail).
    uint32_t materialNameOffset = 0xFFFFFFFF;
    uint8_t  meshSourceType    = 0;  // 0=normal, 1=primitive
    uint8_t  _padMesh[3]       = {};

    // ── Camera (28 bytes) ──
    uint8_t  cameraIsPrimary   = 1;
    uint8_t  cameraProjection  = 0;  // 0=perspective, 1=ortho
    uint8_t  _padCam[2]        = {};
    float    cameraFov         = 60.0f;
    float    cameraOrthoSize   = 10.0f;
    float    cameraNearPlane   = 0.1f;
    float    cameraFarPlane    = 1000.0f;
    float    cameraClearColor[4] = {0.1f, 0.1f, 0.12f, 1.0f};

    // ── RigidBody (40 bytes) ──
    uint8_t  rbBodyType        = 1;  // 0=static, 1=kinematic, 2=dynamic
    uint8_t  rbShape           = 0;  // 0=box, 1=sphere, 2=capsule
    uint8_t  rbUseGravity      = 1;
    uint8_t  _padRB            = 0;
    float    rbMass            = 1.0f;
    float    rbRestitution     = 0.3f;
    float    rbFriction        = 0.6f;
    float    rbHalfExtent[3]   = {0.5f, 0.5f, 0.5f};
    float    rbRadius          = 0.5f;
    float    rbHalfHeight      = 0.5f;

    // ── Script (8 bytes → string table) ──
    uint32_t scriptPathOffset  = 0xFFFFFFFF;
    uint32_t scriptPathLength  = 0;

    // ── CharacterController (24 bytes) ──
    float    ccRadius          = 0.3f;
    float    ccHeight          = 1.8f;
    float    ccMaxSlopeDeg     = 45.0f;
    float    ccStepHeight      = 0.3f;
    float    ccMass            = 70.0f;
    float    ccGravityScale    = 1.0f;

    // ── Light (36 bytes) ──
    uint8_t  lightType         = 0;  // 0=directional, 1=point, 2=spot
    uint8_t  lightCastShadows  = 0;
    uint8_t  lightUseTemp      = 0;
    uint8_t  _padLight         = 0;
    float    lightColor[3]     = {1, 1, 1};
    float    lightIntensity    = 3.0f;
    float    lightRange        = 15.0f;
    float    lightSpotInner    = 25.0f;
    float    lightSpotOuter    = 35.0f;
    float    lightTemperatureK = 6500.0f;

    // ── Animator (v2, 20 bytes — carved exactly from the former pad) ──
    // Without this, cooked scenes silently dropped skeletal animation:
    // shipped skinned meshes T-posed because clip selection never survived
    // the cook. clipPath = standalone clip source ("" = embedded clipIndex).
    float    animSpeed          = 1.0f;
    float    animFade           = 0.2f;
    uint32_t animClipPathOffset = 0xFFFFFFFF;   // into string table
    uint32_t animClipPathLength = 0;
    int16_t  animClipIndex      = 0;
    uint8_t  animPlaying        = 0;
    uint8_t  animLooping        = 1;
};
static_assert(sizeof(SceneEntity) == 256, "SceneEntity must be exactly 256 bytes");

struct SceneAsset {
    SceneHeader               header;
    std::vector<SceneEntity>  entities;
    std::vector<char>         stringTable;  // packed null-terminated strings
};

// Write a cooked binary scene.
bool saveScene(const SceneAsset& scene, const std::filesystem::path& outPath);

// Read a cooked binary scene. Returns false on bad magic/version or I/O error.
bool loadScene(SceneAsset& out, const std::filesystem::path& inPath);

// ── String table helpers ──

// Append a string to the table, return (offset, length). Empty strings get
// the sentinel offset 0xFFFFFFFF.
inline std::pair<uint32_t, uint32_t> stringTableAppend(
        std::vector<char>& table, const std::string& s) {
    if (s.empty()) return {0xFFFFFFFF, 0};
    uint32_t off = static_cast<uint32_t>(table.size());
    uint32_t len = static_cast<uint32_t>(s.size());
    table.insert(table.end(), s.begin(), s.end());
    table.push_back('\0');  // null terminator for safety
    return {off, len};
}

// Read a NULL-TERMINATED string by offset alone. stringTableAppend always
// writes a terminator, so a field with no room for a length (see
// SceneEntity::materialNameOffset) can still carry a string. Bounded by the
// table size, so a corrupt offset yields empty rather than running off the end.
inline std::string stringTableReadZ(const std::vector<char>& table,
                                    uint32_t offset) {
    if (offset == 0xFFFFFFFF || offset >= table.size()) return {};
    const char* b = table.data() + offset;
    const char* e = (const char*)memchr(b, '\0', table.size() - offset);
    return e ? std::string(b, (size_t)(e - b)) : std::string{};
}

// Read a string from the table by (offset, length). Returns empty if sentinel.
inline std::string stringTableRead(const std::vector<char>& table,
                                   uint32_t offset, uint32_t length) {
    if (offset == 0xFFFFFFFF || length == 0) return {};
    // 64-bit, and it has to be. Both fields are uint32_t, so `offset + length`
    // computes in 32-bit and WRAPS: offset=0xFFFFFFF0 with length=0x20 yields
    // 0x10, sails past the bound, and hands back a std::string built from a
    // pointer 4 GB beyond the buffer. A corrupt cooked scene — a bad disk, a
    // truncated DDC blob from another machine — is enough to reach it.
    if ((uint64_t)offset + (uint64_t)length > (uint64_t)table.size()) return {};
    return std::string(table.data() + offset, length);
}

} // namespace assetlib
