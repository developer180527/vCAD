#pragma once
// Internal to the registry TUs (schema / records / dependencies / scanner).
// Not a public header: nothing outside src/registry/ may include it.
//
// AssetRegistry stores its sqlite3* as a void* so the PUBLIC header carries no
// <sqlite3.h> dependency — consumers of assetlib link SQLite but must not have
// to compile against it. This is the one cast, in one place.
#include <sqlite3.h>

namespace assetlib {
inline sqlite3* db(void* p) { return static_cast<sqlite3*>(p); }
}
