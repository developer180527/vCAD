// ── Dependency edges ─────────────────────────────────────────────────────────
// The asset -> asset graph: edges, cycle rejection, dependents, and the
// dependency SOURCE HASHES that feed cook keys. Note what this file is NOT:
// staleness. Cook invalidation is decided by the DDC key alone, so nothing here
// marks anything stale — see the comment on transitiveDependents.
#include "assetlib/asset_registry.h"
#include "registry/internal.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace assetlib {

// ── Dependencies ──────────────────────────────────────────────────────────────
bool AssetRegistry::wouldCreateCycle(const UUID& from, const UUID& to) const {
    std::unordered_set<std::string> visited;
    std::vector<UUID> stack={to};
    while (!stack.empty()) {
        UUID cur=stack.back(); stack.pop_back();
        auto s=cur.toString();
        if (s==from.toString()) return true;
        if (!visited.insert(s).second) continue;
        for (const UUID& d:dependencies(cur)) stack.push_back(d);
    }
    return false;
}
bool AssetRegistry::addDependency(const UUID& asset, const UUID& dep) {
    if (wouldCreateCycle(asset,dep)) {
        std::fprintf(stderr,"[AssetLib] Cycle rejected: %s -> %s\n",
            asset.toString().c_str(), dep.toString().c_str());
        return false;
    }
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),
        "INSERT OR IGNORE INTO dependencies(asset_uuid,depends_on_uuid) VALUES(?,?);",
        -1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto a=asset.toString(), d=dep.toString();
    sqlite3_bind_text(stmt,1,a.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,d.c_str(),-1,SQLITE_TRANSIENT);
    bool ok=sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}
bool AssetRegistry::removeDependencies(const UUID& asset) {
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),
        "DELETE FROM dependencies WHERE asset_uuid=?;",
        -1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto a=asset.toString();
    sqlite3_bind_text(stmt,1,a.c_str(),-1,SQLITE_TRANSIENT);
    bool ok=sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}
static std::vector<UUID> queryUUIDs(sqlite3* d,const char* sql,const std::string& bind) {
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(d,sql,-1,&stmt,nullptr)!=SQLITE_OK) return {};
    sqlite3_bind_text(stmt,1,bind.c_str(),-1,SQLITE_TRANSIENT);
    std::vector<UUID> out;
    while (sqlite3_step(stmt)==SQLITE_ROW)
        out.push_back(UUID::fromString(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt,0))));
    sqlite3_finalize(stmt); return out;
}
// A dependency's SOURCE hash, not its cooked key. Deliberate: folding a
// dependency's cooked identity in would make the fold transitive, and it would
// also drag in things the dependent provably does not care about — a material's
// output depends on the shader's declared INTERFACE (the .shader manifest),
// while the shader's cooked key also covers its .sc stage sources. Keying
// materials on that would recook every material in the project on every
// shading-code edit. Source hashes keep the invalidation as narrow as what the
// cooker actually reads. The cost is that a change does not propagate through
// two hops on its own; a cooker that needs that must declare the far dependency
// too.
std::unordered_map<std::string, std::vector<std::string>>
AssetRegistry::allDependencySourceHashes() const {
    std::unordered_map<std::string, std::vector<std::string>> out;
    static const char* kSql =
        "SELECT d.asset_uuid, a.source_hash FROM dependencies d "
        "JOIN assets a ON a.uuid = d.depends_on_uuid;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db(m_db), kSql, -1, &stmt, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* asset = reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
        const char* hash  = reinterpret_cast<const char*>(sqlite3_column_text(stmt,1));
        if (asset && hash && *hash) out[asset].push_back(hash);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<std::string>
AssetRegistry::dependencySourceHashes(const UUID& uuid) const {
    std::vector<std::string> out;
    static const char* kSql =
        "SELECT a.source_hash FROM dependencies d "
        "JOIN assets a ON a.uuid = d.depends_on_uuid WHERE d.asset_uuid=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db(m_db), kSql, -1, &stmt, nullptr) != SQLITE_OK)
        return out;
    const auto s = uuid.toString();
    sqlite3_bind_text(stmt, 1, s.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        if (const char* h = reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
            h && *h)
            out.push_back(h);
    sqlite3_finalize(stmt);
    return out;
}

std::vector<UUID> AssetRegistry::dependents(const UUID& uuid) const {
    return queryUUIDs(db(m_db),
        "SELECT asset_uuid FROM dependencies WHERE depends_on_uuid=?;",
        uuid.toString());
}
std::vector<UUID> AssetRegistry::dependencies(const UUID& uuid) const {
    return queryUUIDs(db(m_db),
        "SELECT depends_on_uuid FROM dependencies WHERE asset_uuid=?;",
        uuid.toString());
}
std::vector<UUID> AssetRegistry::transitiveDependents(const UUID& uuid) const {
    std::vector<UUID> result;
    std::unordered_set<std::string> visited;
    std::vector<UUID> queue={uuid};
    while (!queue.empty()) {
        UUID cur=queue.back(); queue.pop_back();
        auto s=cur.toString();
        if (!visited.insert(s).second) continue;
        for (const UUID& d:dependents(cur)) { result.push_back(d); queue.push_back(d); }
    }
    return result;
}

} // namespace assetlib
