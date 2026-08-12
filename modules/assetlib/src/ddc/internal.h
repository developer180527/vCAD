#pragma once
// Internal to the DDC TUs (hash / store / gc). Not a public header.
//
// Blobs are immutable and stored read-only, which makes every filesystem
// operation on them slightly unusual — removing one needs the read-only bit
// cleared first on Windows, and a temp name has to be unique across BOTH
// processes and threads. Those three helpers are shared by store.cpp and gc.cpp,
// so they live here rather than being duplicated (a second, subtly different
// uniqueTempPath is exactly how you get two writers on one file).
#include <filesystem>
#include <string>

namespace assetlib::ddcfs {

// Delete a blob, clearing the read-only attribute first where that is required.
void removeBlob(const std::filesystem::path& p, std::error_code& ec);
// Mark a finished blob immutable.
void makeReadOnly(const std::filesystem::path& p);
// Record "used just now" as mtime, for GC's LRU order. Best-effort, silent.
void touchForLru(const std::filesystem::path& p);
// A temp name unique across processes AND threads — see the definition.
std::filesystem::path uniqueTempPath(const std::filesystem::path& dir,
                                     const std::string& key, const char* tag);

} // namespace assetlib::ddcfs
