#pragma once
#include "uuid.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdint>

namespace assetlib {

enum class AssetType : uint32_t {
    Unknown=0, Mesh=1, Texture=2, Material=3,
    Scene=4, Prefab=5, Shader=6, Audio=7,
};
std::string assetTypeName(AssetType t);
AssetType   assetTypeFromExtension(const std::string& ext);

enum class AssetState : uint8_t {
    Unknown=0, Registered=1, Importing=2, Ready=3,
    Stale=4, Missing=5, Failed=6, Dirty=7,
};
std::string assetStateName(AssetState s);

struct ImportSettings {
    std::string json;
    bool empty() const { return json.empty(); }
};

struct AssetRecord {
    UUID          uuid;
    AssetType     type            = AssetType::Unknown;
    AssetState    state           = AssetState::Unknown;
    std::string   sourcePath;
    std::string   sourceHash;
    int64_t       sourceMtime     = 0;
    int64_t       sourceSize      = 0;
    std::string   cookedPath;
    uint32_t      cookVersion     = 0;
    uint32_t      importerVersion = 0;
    int64_t       cookedAt        = 0;
    ImportSettings importSettings;
    std::string   errorMessage;
    // DDC key of the last cook ATTEMPT (success, skip, or failure) — the
    // content address of everything that went into it. Staleness is simply
    // "stored key != current key"; a Failed record with a matching key means
    // "these exact inputs already failed, don't retry until they change".
    std::string   ddcKey;
};

class AssetRegistry {
public:
    AssetRegistry()  = default;
    ~AssetRegistry();

    bool open(const std::filesystem::path& dbPath);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    bool insert(const AssetRecord& rec);
    bool update(const AssetRecord& rec);
    bool remove(const UUID& uuid);

    std::optional<AssetRecord> findByUUID(const UUID& uuid)             const;
    std::optional<AssetRecord> findBySourcePath(const std::string& rel) const;
    std::vector<AssetRecord>   findByType(AssetType type)               const;
    std::vector<AssetRecord>   findByState(AssetState state)            const;
    std::vector<AssetRecord>   all()                                    const;

    bool setState(const UUID& uuid, AssetState state,
                  const std::string& errorMsg = {});

    bool              addDependency(const UUID& asset, const UUID& dep);
    bool              removeDependencies(const UUID& asset);
    bool              wouldCreateCycle(const UUID& from, const UUID& to) const;
    std::vector<UUID> dependents(const UUID& uuid)           const;
    std::vector<UUID> dependencies(const UUID& uuid)         const;
    // QUERY ONLY — this is NOT how a dependency change invalidates a cook.
    // Staleness is decided by the DDC key (see src/cook/key.h), so marking a
    // dependent `Stale` would not cause it to recook: cookIsStale never reads
    // `state` except to keep a Failed record failed. Invalidation happens
    // because a dependency's hash is folded INTO the dependent's key. Kept for
    // tooling that wants to show "what does this asset affect".
    std::vector<UUID> transitiveDependents(const UUID& uuid) const;

    // ── Dependency hashes, for cook-key derivation ───────────────────────────
    // asset uuid -> source hashes of everything it depends on. ONE query for
    // the whole table: the per-record alternative is a prepare-per-asset inside
    // a staleness loop, which is exactly the O(N)-compiles mistake removed from
    // scan(). Callers hash the vector into the key; ordering is normalised
    // there, not here.
    std::unordered_map<std::string, std::vector<std::string>>
        allDependencySourceHashes() const;
    // Single-record variant for the one-off paths (editor queries, cookOne).
    std::vector<std::string> dependencySourceHashes(const UUID& uuid) const;

    int scan(const std::filesystem::path& assetsRoot,
             const std::filesystem::path& projectRoot);

private:
    void* m_db = nullptr;
    bool  execSQL(const std::string& sql);
    // False when the database cannot be brought to the current schema — a real
    // SQLite error, or a file written by a NEWER build (see the impl). open()
    // fails in that case rather than handing back a registry that silently
    // drops writes.
    bool  migrate();
    // Takes the WALKED ENTRY, not a path: directory_entry caches the attributes
    // the walk already read, so this costs no syscall. Given a bare path it
    // would be two fresh stat() calls per file, on every scan, forever.
    static bool statChanged(const AssetRecord& rec,
                            const std::filesystem::directory_entry& e);
};

} // namespace assetlib
