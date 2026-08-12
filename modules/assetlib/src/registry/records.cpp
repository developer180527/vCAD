// ── Record I/O: row mapping, CRUD, queries ───────────────────────────────────
// AssetRecord <-> SQLite rows, and every read/write of a single record. kCols is
// the ONE column list every SELECT shares, so a schema addition cannot be
// half-applied: rowToRecord reads positionally and both live here, side by
// side, where a mismatch is visible.
#include "assetlib/asset_registry.h"
#include "registry/internal.h"

#include <optional>
#include <string>
#include <vector>

namespace assetlib {

// ── Row helper ────────────────────────────────────────────────────────────────
static const char* kCols =
    "uuid,type,state,source_path,cooked_path,source_hash,"
    "source_mtime,source_size,cook_version,importer_version,"
    "import_settings,error_message,cooked_at,ddc_key";

static AssetRecord rowToRecord(sqlite3_stmt* s) {
    AssetRecord r;
    auto txt = [&](int col) -> std::string {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(s, col));
        return p ? p : "";
    };
    r.uuid              = UUID::fromString(txt(0));
    r.type              = static_cast<AssetType> (sqlite3_column_int  (s,1));
    r.state             = static_cast<AssetState>(sqlite3_column_int  (s,2));
    r.sourcePath        = txt(3);
    r.cookedPath        = txt(4);
    r.sourceHash        = txt(5);
    r.sourceMtime       = sqlite3_column_int64(s,6);
    r.sourceSize        = sqlite3_column_int64(s,7);
    r.cookVersion       = static_cast<uint32_t>(sqlite3_column_int(s,8));
    r.importerVersion   = static_cast<uint32_t>(sqlite3_column_int(s,9));
    r.importSettings.json = txt(10);
    r.errorMessage      = txt(11);
    r.cookedAt          = sqlite3_column_int64(s,12);
    r.ddcKey            = txt(13);
    return r;
}

// ── CRUD ──────────────────────────────────────────────────────────────────────
bool AssetRegistry::insert(const AssetRecord& r) {
    const char* sql =
        "INSERT INTO assets"
        "(uuid,type,state,source_path,cooked_path,source_hash,"
        " source_mtime,source_size,cook_version,importer_version,"
        " import_settings,error_message,cooked_at,ddc_key)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(uuid) DO UPDATE SET"
        "  type=excluded.type, state=excluded.state,"
        "  source_path=excluded.source_path,"
        "  cooked_path=excluded.cooked_path,"
        "  source_hash=excluded.source_hash,"
        "  source_mtime=excluded.source_mtime,"
        "  source_size=excluded.source_size,"
        "  cook_version=excluded.cook_version,"
        "  importer_version=excluded.importer_version,"
        "  import_settings=excluded.import_settings,"
        "  error_message=excluded.error_message,"
        "  cooked_at=excluded.cooked_at,"
        "  ddc_key=excluded.ddc_key;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql,-1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto us = r.uuid.toString();
    sqlite3_bind_text (stmt,1,  us.c_str(),                    -1,SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt,2,  static_cast<int>(r.type));
    sqlite3_bind_int  (stmt,3,  static_cast<int>(r.state));
    sqlite3_bind_text (stmt,4,  r.sourcePath.c_str(),          -1,SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt,5,  r.cookedPath.c_str(),          -1,SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt,6,  r.sourceHash.c_str(),          -1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,7,  r.sourceMtime);
    sqlite3_bind_int64(stmt,8,  r.sourceSize);
    sqlite3_bind_int  (stmt,9,  static_cast<int>(r.cookVersion));
    sqlite3_bind_int  (stmt,10, static_cast<int>(r.importerVersion));
    sqlite3_bind_text (stmt,11, r.importSettings.json.c_str(), -1,SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt,12, r.errorMessage.c_str(),        -1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,13, r.cookedAt);
    sqlite3_bind_text (stmt,14, r.ddcKey.c_str(),              -1,SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}
bool AssetRegistry::update(const AssetRecord& r) { return insert(r); }

bool AssetRegistry::remove(const UUID& uuid) {
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),
        "DELETE FROM assets WHERE uuid=?;",-1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto s=uuid.toString();
    sqlite3_bind_text(stmt,1,s.c_str(),-1,SQLITE_TRANSIENT);
    bool ok=sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}

bool AssetRegistry::setState(const UUID& uuid, AssetState state,
                             const std::string& errorMsg) {
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),
        "UPDATE assets SET state=?,error_message=? WHERE uuid=?;",
        -1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto s=uuid.toString();
    sqlite3_bind_int (stmt,1,static_cast<int>(state));
    sqlite3_bind_text(stmt,2,errorMsg.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,3,s.c_str(),-1,SQLITE_TRANSIENT);
    bool ok=sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}

// ── Queries ───────────────────────────────────────────────────────────────────
static std::optional<AssetRecord> stepOne(sqlite3_stmt* s) {
    std::optional<AssetRecord> r;
    if (sqlite3_step(s)==SQLITE_ROW) r=rowToRecord(s);
    sqlite3_finalize(s); return r;
}
static std::vector<AssetRecord> stepAll(sqlite3_stmt* s) {
    std::vector<AssetRecord> v;
    while (sqlite3_step(s)==SQLITE_ROW) v.push_back(rowToRecord(s));
    sqlite3_finalize(s); return v;
}

std::optional<AssetRecord> AssetRegistry::findByUUID(const UUID& uuid) const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets WHERE uuid=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    auto s=uuid.toString();
    sqlite3_bind_text(stmt,1,s.c_str(),-1,SQLITE_TRANSIENT);
    return stepOne(stmt);
}
std::optional<AssetRecord> AssetRegistry::findBySourcePath(const std::string& rel) const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets WHERE source_path=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    sqlite3_bind_text(stmt,1,rel.c_str(),-1,SQLITE_TRANSIENT);
    return stepOne(stmt);
}
std::vector<AssetRecord> AssetRegistry::findByType(AssetType type) const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets WHERE type=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    sqlite3_bind_int(stmt,1,static_cast<int>(type));
    return stepAll(stmt);
}
std::vector<AssetRecord> AssetRegistry::findByState(AssetState state) const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets WHERE state=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    sqlite3_bind_int(stmt,1,static_cast<int>(state));
    return stepAll(stmt);
}
std::vector<AssetRecord> AssetRegistry::all() const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    return stepAll(stmt);
}

} // namespace assetlib
