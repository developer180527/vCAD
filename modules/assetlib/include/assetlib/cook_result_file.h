#pragma once
// ── Cook worker result file: the framing, in ONE place ───────────────────────
// A cook worker reports its outcome through a sidecar file rather than stdout
// (cookers print freely, so stdout is not a channel). The payload is
// line-oriented and deliberately human-readable — you can cat one while
// debugging a failing cook:
//
//     ENGINE_COOK_RESULT 1              <- magic + version
//     RESULT ok|skip|fail
//     ERROR  <single-line message>      (0..1)
//     OUTPUT <extra output path>        (0..n — a mesh's sibling .ctex)
//     DEP    <uuid>                     (0..n)
//     END <line-count> <hex digest>     <- trailer
//
// WHY THE TRAILER EXISTS. `RESULT ok` is the FIRST body line, so a worker killed
// part-way through writing (the deadline SIGKILL, an rlimit OOM, a signal out of
// a corrupt parse) left a file that parsed as a clean success with its OUTPUT
// lines simply absent. The parent believed it, committed the asset, and shipped
// a mesh whose sibling textures were never registered — the silently-untextured
// build, arriving through the IPC channel instead of the packager. A result is
// now only valid if the trailer agrees with the body, so a partial write is
// *detectably* partial and becomes a per-asset failure.
//
// The digest is FNV-1a, not BLAKE3: this detects truncation and interrupted
// writes, and the file lives in our own temp directory alongside the output it
// describes. It is not a security boundary — anyone who can rewrite that file
// can rewrite the cooked artifact next to it, and the DDC's content hash is what
// guards the artifact itself. A 64-bit non-cryptographic digest is the right
// tool for "did this write finish".
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace assetlib::cookresult {

inline constexpr std::string_view kMagic   = "ENGINE_COOK_RESULT";
inline constexpr int              kVersion = 1;

inline uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 1469598103934665603ull;            // offset basis
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// Wrap a completed body (each line already '\n'-terminated) into file contents.
inline std::string frame(std::string_view body, size_t lines) {
    char trailer[64];
    std::snprintf(trailer, sizeof(trailer), "END %zu %llx\n", lines,
                  (unsigned long long)fnv1a64(body));
    std::string out;
    out.reserve(kMagic.size() + 8 + body.size() + 40);
    out.append(kMagic).append(" ").append(std::to_string(kVersion)).append("\n");
    out.append(body);
    out.append(trailer);
    return out;
}

// Strip and validate the frame. On success `body` holds the payload lines. On
// failure `err` says what was wrong, in terms a human debugging a cook can act
// on. Never throws; every malformed shape is a `false` return.
inline bool unframe(std::string_view file, std::string& body, std::string& err) {
    body.clear();

    const size_t h = file.find('\n');
    if (h == std::string_view::npos) { err = "no header line"; return false; }
    const std::string_view header = file.substr(0, h);
    if (header.rfind(kMagic, 0) != 0) {
        err = "not a cook result file (bad magic)";
        return false;
    }
    // Version is advisory but must parse: a mismatch means the worker binary and
    // the host disagree, which is a build problem worth naming precisely.
    const std::string_view verStr = header.substr(kMagic.size());
    int ver = 0;
    if (std::sscanf(std::string(verStr).c_str(), "%d", &ver) != 1) {
        err = "header has no version"; return false;
    }
    if (ver != kVersion) {
        err = "result file version " + std::to_string(ver) + ", expected "
            + std::to_string(kVersion) + " (stale engine_cook_worker binary?)";
        return false;
    }

    // The trailer is the last '\n'-terminated line. A file that does not end in
    // a newline was cut mid-line, which is the common truncation shape.
    if (file.empty() || file.back() != '\n') {
        err = "truncated: does not end at a line boundary"; return false;
    }
    const size_t lastEnd = file.size() - 1;                  // index of final \n
    const size_t lastBeg = file.rfind('\n', lastEnd ? lastEnd - 1 : 0);
    if (lastBeg == std::string_view::npos || lastBeg < h) {
        err = "truncated: no END trailer"; return false;
    }
    const std::string_view trailer = file.substr(lastBeg + 1, lastEnd - lastBeg - 1);
    if (trailer.rfind("END ", 0) != 0) {
        err = "truncated: last line is not END (worker died mid-write)";
        return false;
    }

    unsigned long long claimedLines = 0, claimedHash = 0;
    if (std::sscanf(std::string(trailer).c_str(), "END %llu %llx",
                    &claimedLines, &claimedHash) != 2) {
        err = "malformed END trailer"; return false;
    }

    const std::string_view payload = file.substr(h + 1, lastBeg - h);
    size_t lines = 0;
    for (char c : payload) if (c == '\n') ++lines;
    if (lines != claimedLines) {
        err = "incomplete: END claims " + std::to_string(claimedLines)
            + " line(s), found " + std::to_string(lines);
        return false;
    }
    if (fnv1a64(payload) != claimedHash) {
        err = "corrupt: body digest does not match END";
        return false;
    }

    body.assign(payload);
    return true;
}

} // namespace assetlib::cookresult
