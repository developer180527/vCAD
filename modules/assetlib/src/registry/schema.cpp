// ── Schema, migrations, connection lifecycle ─────────────────────────────────
// The table definitions, the additive-ALTER migration list, PRAGMA user_version,
// and open/close. Isolated because schema evolution is the one part of the
// registry where a mistake is not recoverable by re-running: it is edited rarely
// and reviewed carefully, and mixing it into the CRUD churn hides that.
#include "assetlib/asset_registry.h"
#include "registry/internal.h"

#include <cstdio>
#include <string>

namespace assetlib {

// ── Schema ────────────────────────────────────────────────────────────────────
static const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS assets (
    uuid             TEXT    PRIMARY KEY,
    type             INTEGER NOT NULL DEFAULT 0,
    state            INTEGER NOT NULL DEFAULT 0,
    source_path      TEXT    NOT NULL,
    cooked_path      TEXT    NOT NULL DEFAULT '',
    source_hash      TEXT    NOT NULL DEFAULT '',
    source_mtime     INTEGER NOT NULL DEFAULT 0,
    source_size      INTEGER NOT NULL DEFAULT 0,
    cook_version     INTEGER NOT NULL DEFAULT 0,
    importer_version INTEGER NOT NULL DEFAULT 0,
    import_settings  TEXT    NOT NULL DEFAULT '',
    error_message    TEXT    NOT NULL DEFAULT '',
    cooked_at        INTEGER NOT NULL DEFAULT 0,
    ddc_key          TEXT    NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS dependencies (
    asset_uuid      TEXT NOT NULL,
    depends_on_uuid TEXT NOT NULL,
    PRIMARY KEY (asset_uuid, depends_on_uuid),
    FOREIGN KEY (asset_uuid)      REFERENCES assets(uuid) ON DELETE CASCADE,
    FOREIGN KEY (depends_on_uuid) REFERENCES assets(uuid) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_source_path ON assets(source_path);
CREATE INDEX IF NOT EXISTS idx_type        ON assets(type);
CREATE INDEX IF NOT EXISTS idx_state       ON assets(state);
CREATE INDEX IF NOT EXISTS idx_dep_on      ON dependencies(depends_on_uuid);
)SQL";

// Bump when kMigrations grows. Stored in PRAGMA user_version so a database
// written by a NEWER build can be refused instead of silently downgraded.
static constexpr int kSchemaVersion = 2;

static const char* kMigrations[] = {
    "ALTER TABLE assets ADD COLUMN state            INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE assets ADD COLUMN source_mtime     INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE assets ADD COLUMN source_size      INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE assets ADD COLUMN importer_version INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE assets ADD COLUMN import_settings  TEXT    NOT NULL DEFAULT '';",
    "ALTER TABLE assets ADD COLUMN error_message    TEXT    NOT NULL DEFAULT '';",
    "ALTER TABLE assets ADD COLUMN ddc_key          TEXT    NOT NULL DEFAULT '';",
};

// ── Lifecycle ─────────────────────────────────────────────────────────────────
AssetRegistry::~AssetRegistry() { close(); }

bool AssetRegistry::open(const std::filesystem::path& dbPath) {
    std::filesystem::create_directories(dbPath.parent_path());
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.string().c_str(), &raw) != SQLITE_OK)
        { sqlite3_close(raw); return false; }
    m_db = raw;
    sqlite3_exec(db(m_db), "PRAGMA foreign_keys  = ON;",     nullptr,nullptr,nullptr);
    sqlite3_exec(db(m_db), "PRAGMA journal_mode  = WAL;",    nullptr,nullptr,nullptr);
    sqlite3_exec(db(m_db), "PRAGMA synchronous   = NORMAL;", nullptr,nullptr,nullptr);
    // Connections open mid-cook (scene tasks on the worker pool) while the
    // drain lane writes — retry briefly on lock contention instead of
    // failing a read that would have succeeded 5ms later.
    sqlite3_busy_timeout(db(m_db), 5000);
    // A registry that could not be brought to the current schema is not usable.
    // Reporting success here is what let a corrupt or newer-versioned database
    // look fine and then silently record nothing.
    if (!migrate()) { close(); return false; }
    return true;
}
void AssetRegistry::close() {
    if (m_db) { sqlite3_close(db(m_db)); m_db = nullptr; }
}
bool AssetRegistry::execSQL(const std::string& sql) {
    char* err = nullptr;
    bool ok = sqlite3_exec(db(m_db), sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
    if (err) sqlite3_free(err);
    return ok;
}
// The additive-ALTER list is idempotent by construction: each statement is its
// own exec, so one failing cannot skip the next. What it could not do before was
// tell "this column already exists" apart from "this database is corrupt, or
// read-only, or the disk is full" — every error was discarded, migrate() returned
// void, and open() returned true regardless. An unusable registry reported
// success, and the cook then silently recorded nothing.
//
// So: match the already-applied case EXACTLY (SQLite says "duplicate column
// name: <col>") and treat anything else as a real failure. PRAGMA user_version
// additionally lets us refuse a database written by a NEWER engine — which only
// matters, and matters a lot, once a DDC is shared across machines running
// different builds.
bool AssetRegistry::migrate() {
    char* err = nullptr;
    if (sqlite3_exec(db(m_db), kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "[AssetLib] schema creation failed: %s\n",
                     err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }

    // Forward-incompatibility check BEFORE any write. A newer build may have
    // added columns this one does not know about; opening is fine (SQLite
    // ignores unknown columns on named SELECTs, and kCols is explicit), but
    // WRITING would drop whatever those columns hold on every update().
    int64_t onDisk = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db(m_db), "PRAGMA user_version;", -1, &st, nullptr)
            == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) onDisk = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (onDisk > kSchemaVersion) {
        std::fprintf(stderr,
            "[AssetLib] registry schema v%lld is NEWER than this build's v%d — "
            "refusing to open. A newer engine wrote this cache; writing to it "
            "would discard columns this build cannot see. Use a matching build, "
            "or delete .cache/registry.db to re-scan from scratch.\n",
            (long long)onDisk, kSchemaVersion);
        return false;
    }

    for (const char* m : kMigrations) {
        err = nullptr;
        if (sqlite3_exec(db(m_db), m, nullptr, nullptr, &err) == SQLITE_OK) {
            if (err) sqlite3_free(err);
            continue;
        }
        const std::string msg = err ? err : "";
        if (err) sqlite3_free(err);
        // The one benign failure: this column landed on an earlier open.
        if (msg.rfind("duplicate column name", 0) == 0) continue;
        std::fprintf(stderr, "[AssetLib] migration failed (%s): %s\n", m,
                     msg.empty() ? "(no message)" : msg.c_str());
        return false;
    }

    if (onDisk < kSchemaVersion) {
        const std::string bump =
            "PRAGMA user_version = " + std::to_string(kSchemaVersion) + ";";
        sqlite3_exec(db(m_db), bump.c_str(), nullptr, nullptr, nullptr);
    }
    return true;
}

} // namespace assetlib
