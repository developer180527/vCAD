#include "cad/io/DocumentStore.h"

#include "cad/naming/ElementName.h"

#include <sqlite3.h>

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace cad::io {
namespace {

using document::ObjectData;
using document::ObjectId;
using document::PropertyValue;
using kernel::Error;
using kernel::ErrorCode;

/// Property type tags written to disk.
///
/// These are FILE FORMAT CONSTANTS, not document::PropertyType. They are spelled out here, with
/// explicit values, precisely so that reordering the PropertyValue variant — a harmless-looking
/// edit — cannot silently reinterpret every property in every saved file. A variant index is an
/// implementation detail; a file format is a promise.
enum class Tag : int {
    Bool = 1,
    Int = 2,
    Real = 3,
    Text = 4,
    Length = 5,
    Angle = 6,
    Object = 7,
    Element = 8,
    ElementList = 9,
    ObjectList = 10,
};

/// RAII for the connection, so an early return cannot leak it. sqlite3_close on a null handle is
/// a documented no-op, so the deleter needs no guard.
using Db = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

/// Doubles are written as text with 17 significant digits: %.17g round-trips every IEEE-754
/// double exactly. Storing them as SQLite REAL would also be exact, but text keeps the file
/// readable with the sqlite3 CLI, which ADR 0003 lists as a reason for choosing SQLite at all.
std::string real(double v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.17g", v);
    return buffer;
}

double parseReal(const std::string& s) { return std::strtod(s.c_str(), nullptr); }

/// Element lists are newline-separated. ElementName::toString() produces '/'-separated steps with
/// bracketed hex parent lists and never a newline, so no escaping is needed — and a round-trip
/// test in the Rust suite pins that, because the day it stops being true this silently corrupts
/// every fillet in every saved file.
constexpr char kListSeparator = '\n';

std::string encode(const PropertyValue& value, Tag& tag) {
    return std::visit(
        [&tag](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                tag = Tag::Bool;
                return v ? "1" : "0";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                tag = Tag::Int;
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                tag = Tag::Real;
                return real(v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                tag = Tag::Text;
                return v;
            } else if constexpr (std::is_same_v<T, units::Length>) {
                tag = Tag::Length;
                // Base units (mm), not display units. A file must not depend on what the user
                // happened to have selected in the UI when they pressed save.
                return real(v.base());
            } else if constexpr (std::is_same_v<T, units::Angle>) {
                tag = Tag::Angle;
                return real(v.base());
            } else if constexpr (std::is_same_v<T, ObjectId>) {
                tag = Tag::Object;
                return std::to_string(v.value);
            } else if constexpr (std::is_same_v<T, naming::ElementName>) {
                tag = Tag::Element;
                return v.toString();
            } else if constexpr (std::is_same_v<T, std::vector<naming::ElementName>>) {
                tag = Tag::ElementList;
                std::string out;
                for (const auto& name : v) {
                    if (!out.empty()) out.push_back(kListSeparator);
                    out += name.toString();
                }
                return out;
            } else {
                tag = Tag::ObjectList;
                std::string out;
                for (const ObjectId id : v) {
                    if (!out.empty()) out.push_back(kListSeparator);
                    out += std::to_string(id.value);
                }
                return out;
            }
        },
        value);
}

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> parts;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line, kListSeparator)) {
        if (!line.empty()) parts.push_back(line);
    }
    return parts;
}

kernel::Result<PropertyValue> decode(int tag, const std::string& text) {
    switch (static_cast<Tag>(tag)) {
        case Tag::Bool:   return PropertyValue{text == "1"};
        case Tag::Int:    return PropertyValue{static_cast<std::int64_t>(std::strtoll(
                                     text.c_str(), nullptr, 10))};
        case Tag::Real:   return PropertyValue{parseReal(text)};
        case Tag::Text:   return PropertyValue{text};
        // fromBase, because the stored number IS base units — mm and radians. The unit-bearing
        // constructors are private precisely so a raw double cannot be mistaken for a quantity
        // in whatever unit the reader assumed.
        case Tag::Length: return PropertyValue{units::Length::fromBase(parseReal(text))};
        case Tag::Angle:  return PropertyValue{units::Angle::fromBase(parseReal(text))};
        case Tag::Object:
            return PropertyValue{ObjectId{static_cast<std::uint64_t>(
                std::strtoull(text.c_str(), nullptr, 10))}};
        case Tag::Element: {
            auto name = naming::ElementName::parse(text);
            if (name.isNull() && !text.empty()) {
                return Error{ErrorCode::InvalidInput,
                             "This document contains a geometric reference we cannot read.", text};
            }
            return PropertyValue{std::move(name)};
        }
        case Tag::ElementList: {
            std::vector<naming::ElementName> names;
            for (const auto& part : split(text)) {
                auto name = naming::ElementName::parse(part);
                if (name.isNull()) {
                    return Error{ErrorCode::InvalidInput,
                                 "This document contains a geometric reference we cannot read.",
                                 part};
                }
                names.push_back(std::move(name));
            }
            return PropertyValue{std::move(names)};
        }
        case Tag::ObjectList: {
            std::vector<ObjectId> ids;
            for (const auto& part : split(text)) {
                ids.push_back(ObjectId{static_cast<std::uint64_t>(
                    std::strtoull(part.c_str(), nullptr, 10))});
            }
            return PropertyValue{std::move(ids)};
        }
    }
    // An unknown tag means a newer version of vCAD wrote this. ADR 0003 requires round-tripping
    // what we do not understand rather than dropping it, but we cannot honour that with the
    // current schema — a property we cannot represent has nowhere to live in the document. Say so
    // plainly instead of discarding the property and handing back a subtly different model.
    return Error{ErrorCode::Unsupported,
                 "This document was written by a newer version of vCAD.",
                 "unknown property tag " + std::to_string(tag)};
}

Error sqlError(sqlite3* db, const std::string& what) {
    return Error{ErrorCode::Internal, what,
                 db != nullptr ? sqlite3_errmsg(db) : "could not open the database"};
}

kernel::Result<void> exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string detail = message != nullptr ? message : "unknown error";
        sqlite3_free(message);
        return Error{ErrorCode::Internal, "The document could not be written.", detail};
    }
    return {};
}

/// Prepared-statement wrapper. Only what this file needs; a general SQLite layer would be a
/// bigger commitment than one format justifies.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        ok_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) == SQLITE_OK;
    }
    ~Stmt() { sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }

    void bind(int i, std::int64_t v) { sqlite3_bind_int64(stmt_, i, v); }
    void bind(int i, const std::string& v) {
        // SQLITE_TRANSIENT: sqlite copies the bytes. The alternative aliases a std::string that
        // may be a temporary, which is a use-after-free that usually looks like corrupt data.
        sqlite3_bind_text(stmt_, i, v.c_str(), -1, SQLITE_TRANSIENT);
    }

    [[nodiscard]] bool step() { return sqlite3_step(stmt_) == SQLITE_ROW; }
    [[nodiscard]] bool done() { return sqlite3_step(stmt_) == SQLITE_DONE; }
    void reset() { sqlite3_reset(stmt_); }

    [[nodiscard]] std::int64_t asInt(int col) const { return sqlite3_column_int64(stmt_, col); }
    [[nodiscard]] std::string asText(int col) const {
        const auto* p = sqlite3_column_text(stmt_, col);
        return p == nullptr ? std::string{} : reinterpret_cast<const char*>(p);
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
    bool ok_ = false;
};

constexpr const char* kSchema = R"sql(
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL) STRICT;
CREATE TABLE objects (
  id       INTEGER PRIMARY KEY,
  type     TEXT NOT NULL,
  label    TEXT NOT NULL
) STRICT;
CREATE TABLE properties (
  object_id INTEGER NOT NULL REFERENCES objects(id) ON DELETE CASCADE,
  name      TEXT NOT NULL,
  tag       INTEGER NOT NULL,
  -- Persisted because it feeds the recompute CACHE KEY: a cosmetic property is excluded from it.
  -- Reloading a colour as non-cosmetic changes every downstream key, which at best throws away
  -- the cache and at worst makes two documents that should share a cache entry disagree.
  cosmetic  INTEGER NOT NULL DEFAULT 0,
  value     TEXT NOT NULL,
  -- The text the user typed when it was more than a number: "width * 2". Empty for a plain value,
  -- which is most of them. Keeping only the evaluated number would mean reopening a file silently
  -- discarded every relationship in the model.
  expression TEXT NOT NULL DEFAULT '',
  -- The display unit that expression was ENTERED in, as its UnitSystem ordinal. A bare number
  -- inside the text means this unit forever. Without it, a colleague whose preference is inches
  -- re-evaluates `width + 10` as `width + 254mm` -- geometry that depends on who opened the file.
  expression_units INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (object_id, name)
) STRICT;
-- Named parameters: `width = 40mm`, `wall = width / 8`. Document-level rather than properties of
-- some object, because that is what they are -- they compute no geometry and appear in no tree.
-- Same columns as a property, because a parameter IS a property: one representation, one evaluator.
CREATE TABLE parameters (
  name             TEXT PRIMARY KEY,
  tag              INTEGER NOT NULL,
  value            TEXT NOT NULL,
  expression       TEXT NOT NULL DEFAULT '',
  expression_units INTEGER NOT NULL DEFAULT 0
) STRICT;
)sql";

/// A UnitSystem ordinal read back from a file. An unrecognised one becomes millimetres rather
/// than an error: the value in the row is already correct in base units, and refusing to open a
/// document over an unknown enum would lose the whole model to protect one bare number.
units::UnitSystem unitSystemFrom(std::int64_t ordinal) {
    switch (ordinal) {
        case 0: return units::UnitSystem::Millimetre;
        case 1: return units::UnitSystem::Centimetre;
        case 2: return units::UnitSystem::Metre;
        case 3: return units::UnitSystem::Inch;
        case 4: return units::UnitSystem::Foot;
        default: return units::UnitSystem::Millimetre;
    }
}

Db openDb(const std::filesystem::path& path, int flags) {
    sqlite3* raw = nullptr;
    sqlite3_open_v2(path.string().c_str(), &raw, flags, nullptr);
    return Db(raw, &sqlite3_close);
}

}  // namespace

std::string extensionForKind(const std::string& kind) {
    if (kind == "Assembly") return ".vasm";
    if (kind == "Drawing") return ".vdrw";
    if (kind == "Presentation") return ".vpres";
    return ".vpart";
}

kernel::Result<void> saveDocument(const document::Document& doc,
                                 const std::filesystem::path& path, const std::string& kind) {
    // Write beside the target, then rename. std::filesystem::rename is atomic within a filesystem,
    // so the user either has the old file or the new one — never a half-written one. Writing in
    // place would mean a crash mid-save destroys the document being saved.
    std::filesystem::path temp = path;
    temp += ".saving";
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);

    {
        Db db = openDb(temp, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        if (!db) return sqlError(nullptr, "The document could not be written.");

        if (auto r = exec(db.get(), kSchema); !r) return r;
        if (auto r = exec(db.get(), "BEGIN"); !r) return r;

        {
            Stmt meta(db.get(), "INSERT INTO meta (key, value) VALUES (?, ?)");
            if (!meta.ok()) return sqlError(db.get(), "The document could not be written.");
            const std::pair<const char*, std::string> rows[]{
                {"schema_version", std::to_string(kDocumentSchemaVersion)},
                {"kind", kind},
                {"application", "vCAD 0.0.1"},
                // The id allocator's high-water mark. See Document::withNextId for why losing
                // this is a correctness bug and not a cosmetic one.
                {"next_object_id", std::to_string(doc.nextId())},
                // 0 means "no marker": object ids start at 1, so zero is unambiguous and needs no
                // separate presence flag.
                {"rollback_after",
                 std::to_string(doc.rollbackAfter().has_value() ? doc.rollbackAfter()->value : 0)},
            };
            for (const auto& [key, value] : rows) {
                meta.bind(1, std::string(key));
                meta.bind(2, value);
                if (!meta.done()) return sqlError(db.get(), "The document could not be written.");
                meta.reset();
            }
        }

        {
            Stmt parameter(db.get(),
                           "INSERT INTO parameters (name, tag, value, expression, expression_units) "
                           "VALUES (?,?,?,?,?)");
            if (!parameter.ok()) return sqlError(db.get(), "The document could not be written.");
            for (const auto& p : doc.parameters()) {
                Tag tag = Tag::Text;
                const std::string encoded = encode(p.value, tag);
                parameter.bind(1, p.name);
                parameter.bind(2, static_cast<std::int64_t>(tag));
                parameter.bind(3, encoded);
                parameter.bind(4, p.expression);
                parameter.bind(5, static_cast<std::int64_t>(p.expressionUnits));
                if (!parameter.done()) {
                    return sqlError(db.get(), "The document could not be written.");
                }
                parameter.reset();
            }
        }

        Stmt object(db.get(), "INSERT INTO objects (id, type, label) VALUES (?, ?, ?)");
        Stmt property(
            db.get(),
            "INSERT INTO properties (object_id, name, tag, cosmetic, value, expression, "
            "expression_units) VALUES (?,?,?,?,?,?,?)");
        if (!object.ok() || !property.ok()) {
            return sqlError(db.get(), "The document could not be written.");
        }

        for (const ObjectId id : doc.ids()) {
            const auto data = doc.find(id);
            if (!data) continue;

            object.bind(1, static_cast<std::int64_t>(id.value));
            object.bind(2, data->type());
            object.bind(3, data->label());
            if (!object.done()) return sqlError(db.get(), "The document could not be written.");
            object.reset();

            for (const auto& prop : data->properties()) {
                Tag tag = Tag::Text;
                const std::string encoded = encode(prop.value, tag);
                property.bind(1, static_cast<std::int64_t>(id.value));
                property.bind(2, prop.name);
                property.bind(3, static_cast<std::int64_t>(tag));
                property.bind(4, static_cast<std::int64_t>(prop.cosmetic ? 1 : 0));
                property.bind(5, encoded);
                property.bind(6, prop.expression);
                property.bind(7, static_cast<std::int64_t>(prop.expressionUnits));
                if (!property.done()) {
                    return sqlError(db.get(), "The document could not be written.");
                }
                property.reset();
            }
        }

        if (auto r = exec(db.get(), "COMMIT"); !r) return r;
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ignored);
        return Error{ErrorCode::Internal, "The document could not be saved to that location.",
                     ec.message()};
    }
    return {};
}

kernel::Result<DocumentInfo> readDocumentInfo(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return Error{ErrorCode::InvalidInput, "That document does not exist.", path.string()};
    }
    Db db = openDb(path, SQLITE_OPEN_READONLY);
    if (!db) return sqlError(nullptr, "That document could not be opened.");

    DocumentInfo info;
    Stmt meta(db.get(), "SELECT key, value FROM meta");
    if (!meta.ok()) {
        return Error{ErrorCode::InvalidInput, "That file is not a vCAD document.", path.string()};
    }
    while (meta.step()) {
        const std::string key = meta.asText(0);
        const std::string value = meta.asText(1);
        if (key == "schema_version") info.schemaVersion = std::atoi(value.c_str());
        else if (key == "kind") info.kind = value;
        else if (key == "application") info.application = value;
    }
    if (info.schemaVersion == 0) {
        return Error{ErrorCode::InvalidInput, "That file is not a vCAD document.", path.string()};
    }

    Stmt count(db.get(), "SELECT COUNT(*) FROM objects");
    if (count.ok() && count.step()) info.objectCount = static_cast<std::size_t>(count.asInt(0));
    return info;
}

kernel::Result<document::Document> loadDocument(const std::filesystem::path& path) {
    auto info = readDocumentInfo(path);
    if (!info) return info.error();
    if (info.value().schemaVersion > kDocumentSchemaVersion) {
        return Error{ErrorCode::Unsupported,
                     "This document was written by a newer version of vCAD.",
                     "schema version " + std::to_string(info.value().schemaVersion)};
    }

    Db db = openDb(path, SQLITE_OPEN_READONLY);
    if (!db) return sqlError(nullptr, "That document could not be opened.");

    document::Document doc;

    // Objects first, then properties, because a property can reference another object and an
    // ObjectId property is only meaningful once the target exists.
    Stmt objects(db.get(), "SELECT id, type, label FROM objects ORDER BY id");
    if (!objects.ok()) return sqlError(db.get(), "That document could not be opened.");
    while (objects.step()) {
        const ObjectId id{static_cast<std::uint64_t>(objects.asInt(0))};
        ObjectData data(id, objects.asText(1));
        data = data.withLabel(objects.asText(2));
        doc = doc.insert(std::make_shared<const ObjectData>(std::move(data)));
    }

    // Ask the FILE which columns it has, rather than trusting its declared schema version. A
    // document written before expressions existed is not damaged and must open exactly as it did
    // before -- and inferring that from a version number means a single mis-set version turns
    // "older file" into "failed to open".
    bool hasExpressions = false;
    {
        Stmt columns(db.get(), "PRAGMA table_info(properties)");
        while (columns.ok() && columns.step()) {
            if (columns.asText(1) == "expression") hasExpressions = true;
        }
    }

    Stmt properties(db.get(),
                    hasExpressions ? "SELECT object_id, name, tag, cosmetic, value, expression, "
                                     "expression_units FROM properties ORDER BY object_id, name"
                                   : "SELECT object_id, name, tag, cosmetic, value FROM properties "
                                     "ORDER BY object_id, name");
    if (!properties.ok()) return sqlError(db.get(), "That document could not be opened.");
    while (properties.step()) {
        const ObjectId id{static_cast<std::uint64_t>(properties.asInt(0))};
        const auto existing = doc.find(id);
        if (!existing) continue;   // orphan row; the FK should prevent it, older files may not

        auto value = decode(static_cast<int>(properties.asInt(2)), properties.asText(4));
        if (!value) return value.error();

        const std::string expression = hasExpressions ? properties.asText(5) : std::string{};
        auto updated = existing->withExpression(
            properties.asText(1), std::move(value.value()), expression,
            hasExpressions ? unitSystemFrom(properties.asInt(6)) : units::UnitSystem::Millimetre,
            properties.asInt(3) != 0);
        doc = doc.replace(std::make_shared<const ObjectData>(std::move(updated)));
    }

    // Parameters, when the file is new enough to have them. Asked of the file rather than of its
    // declared version, for the same reason the expression columns are.
    {
        bool hasParameters = false;
        Stmt tables(db.get(),
                    "SELECT name FROM sqlite_master WHERE type='table' AND name='parameters'");
        while (tables.ok() && tables.step()) hasParameters = true;

        if (hasParameters) {
            Stmt parameters(db.get(), "SELECT name, tag, value, expression, expression_units FROM "
                                      "parameters ORDER BY name");
            if (!parameters.ok()) return sqlError(db.get(), "That document could not be opened.");
            while (parameters.step()) {
                auto value = decode(static_cast<int>(parameters.asInt(1)), parameters.asText(2));
                if (!value) return value.error();
                doc = doc.withParameter(document::Property{
                    parameters.asText(0), std::move(value.value()), false, parameters.asText(3),
                    unitSystemFrom(parameters.asInt(4))});
            }
        }
    }

    // Restore the allocator last: insert() only raised it past the ids present, which is wrong if
    // the highest-numbered object had been deleted before the save.
    {
        Stmt marker(db.get(), "SELECT value FROM meta WHERE key = 'rollback_after'");
        if (marker.ok() && marker.step()) {
            const auto raw = static_cast<std::uint64_t>(
                std::strtoull(marker.asText(0).c_str(), nullptr, 10));
            if (raw != 0) doc = doc.withRollbackAfter(document::ObjectId{raw});
        }
    }

    Stmt next(db.get(), "SELECT value FROM meta WHERE key = 'next_object_id'");
    if (next.ok() && next.step()) {
        doc = doc.withNextId(static_cast<std::uint64_t>(std::strtoull(
            next.asText(0).c_str(), nullptr, 10)));
    }
    return doc;
}

}  // namespace cad::io
