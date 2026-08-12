# SQLite (amalgamation)

Vendored **fallback only**. `modules/assetlib/CMakeLists.txt` prefers a system
SQLite when one is found (macOS ships it in the SDK; Linux gets it from
`libsqlite3-dev`) and builds this copy otherwise — which in practice means
**Windows**, where there is no system SQLite and CI previously failed at
configure with `Could NOT find SQLite3`.

Keeping it a fallback rather than the default means the platforms that were
already green keep using exactly what they were using; only Windows changes.

Version: 3.53.4 (amalgamation, public domain).
Source: https://www.sqlite.org/download.html

Only `sqlite3.c` + `sqlite3.h` are vendored — not `shell.c` (the CLI) or
`sqlite3ext.h` (loadable-extension SDK), neither of which the registry uses.

To update: download a new amalgamation, replace both files, note the version
here. There are no local modifications to carry across.
