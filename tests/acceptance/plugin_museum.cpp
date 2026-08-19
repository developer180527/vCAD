// The compatibility museum: every plugin binary this project has ever built, loaded forever.
//
// ADR 0011 calls this "the one that actually delivers the goal", and everything else in that
// document hygiene. The reason is that rules like "additive only, never change a signature" are
// what every project intends and what discipline alone never delivers. What survives a decade of
// contributors is a test that fails.
//
// The exhibits are COMPILED ARTEFACTS and are never rebuilt. That is the entire distinction:
// rebuilding from source would test SOURCE compatibility, and the promise is about a binary whose
// author may be gone, whose compiler may be gone, and whose company may be gone. `tests/plugins`
// builds from source on every run and checks something different and also useful; this checks the
// thing nothing else can.
//
// WHEN THIS FAILS, THE HOST IS WRONG. Do not refresh an exhibit to make CI green — that converts
// the one test that can detect a broken promise into a record of having broken it.

#include "cad/abi/cad_plugin_abi.h"

#include "../../abi/src/Loader.h"

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "Platform.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace cad;

namespace {

/// Where this build's exhibits live. Binaries are per-platform, so a triple that has none is a
/// platform nobody has frozen an exhibit on yet — reported, never silently skipped.
std::string triple() {
#if defined(_WIN32) && defined(__aarch64__)
    return "arm64-windows";
#elif defined(_WIN32)
    return "x64-windows";
#elif defined(__APPLE__)
    return "arm64-osx";
#elif defined(__aarch64__)
    return "arm64-linux";
#else
    return "x64-linux";
#endif
}

fs::path museumRoot() { return fs::path(CAD_REPO_ROOT) / "tests" / "museum"; }

/// A session and its host vtable, released together. Same shape as plugin_loader.cpp's, because
/// an exhibit must be loaded through the REAL host a user's plugin gets.
class Host {
public:
    Host() : session_(cad_session_create()) { host_ = cad_plugin_host(session_); }
    ~Host() { cad_session_release(session_); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    [[nodiscard]] const CadHost* host() const { return host_; }

private:
    CadSession session_ = 0;
    const CadHost* host_ = nullptr;
};

/// SHA-256 of a file, or empty if it cannot be read.
///
/// Written out rather than pulled from a dependency, and sha256 rather than the blake3 already
/// vendored for the asset cache, for one reason: `shasum -a 256` exists on every machine. The
/// PROVENANCE file records a digest a human can check with a standard tool and no build, which is
/// what makes the record independently verifiable rather than a number only this test understands.
std::string sha256Of(const fs::path& path) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};

    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8;
    bytes.push_back('\x80');
    while (bytes.size() % 64 != 56) bytes.push_back('\0');
    for (int i = 7; i >= 0; --i) {
        bytes.push_back(static_cast<char>((bitLength >> (i * 8)) & 0xFF));
    }

    const auto ror = [](std::uint32_t v, int n) { return (v >> n) | (v << (32 - n)); };
    for (std::size_t at = 0; at < bytes.size(); at += 64) {
        std::uint32_t w[64] = {};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i * 4])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i * 4 + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i * 4 + 3]));
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (const std::uint32_t word : h) hex << std::setw(8) << word;
    return hex.str();
}

/// A `key = value` line from a PROVENANCE file, or empty.
std::string provenanceValue(const fs::path& file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        };
        if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return {};
}

/// Whether the museum holds an exhibit for ANY platform.
///
/// The distinction that lets a per-platform skip stay honest: a triple nobody has frozen a binary
/// on yet is a gap, but a museum with no exhibits at all means the guarantee is being checked
/// nowhere, and those must not report the same thing.
bool museumHasAnyExhibit() {
    std::error_code ec;
    if (!fs::exists(museumRoot(), ec)) return false;
    for (const auto& entry : fs::recursive_directory_iterator(museumRoot(), ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && entry.path().filename() == "plugin.manifest") return true;
    }
    return false;
}

/// Why an empty museum is a failure, worded for whoever hits it.
std::string emptyMuseumFailure() {
    return "the museum is empty for every platform, so ADR 0011's compatibility promise is being "
           "checked nowhere. Freeze an exhibit; see tests/museum/*/PROVENANCE.";
}

/// Why no exhibit for THIS platform is only a gap, worded for whoever hits it.
std::string missingExhibitSkip() {
    return "no frozen exhibit for " + triple() +
           " yet. Build tests/plugins/demo on this platform once, copy it and its manifest into "
           "tests/museum/abi-<minor>/" + triple() + "/, and record a PROVENANCE beside it.";
}

/// Every exhibit directory for this platform, oldest ABI first.
std::vector<fs::path> exhibits() {
    std::vector<fs::path> found;
    std::error_code ec;
    if (!fs::exists(museumRoot(), ec)) return found;
    for (const auto& generation : fs::directory_iterator(museumRoot(), ec)) {
        if (!generation.is_directory()) continue;
        const fs::path dir = generation.path() / triple();
        if (fs::exists(dir / "plugin.manifest", ec)) found.push_back(dir);
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace

TEST_CASE("every plugin ever built still loads", "[museum][abi]") {
    if (!cad::testing::canLoadPlugins()) SKIP("this platform only permits loading code shipped inside the signed app bundle");
    const auto all = exhibits();

    INFO("triple: " << triple() << ", museum: " << museumRoot().string());

    // Two different situations that must not report the same thing.
    //
    // No exhibit for THIS platform is a gap, not a broken promise: an arm64-osx binary cannot be
    // produced on Linux, so a frozen exhibit has to be built once on each platform and committed
    // there. Skipping says so out loud rather than passing quietly.
    //
    // No exhibit for ANY platform is a failure, because then the guarantee is being checked
    // nowhere and a silently-empty loop is exactly how that stops being noticed.
    if (all.empty()) {
        if (!museumHasAnyExhibit()) FAIL(emptyMuseumFailure());
        SKIP(missingExhibitSkip());
    }

    for (const fs::path& dir : all) {
        INFO("exhibit: " << dir.string());

        // Loaded through the REAL loader, with the real host vtable, exactly as a user's plugin
        // would be. A bespoke loading path here would test itself rather than the shipping one.
        Host host;
        REQUIRE(host.host() != nullptr);

        auto loaded = abi::loadPluginFrom(dir, host.host());
        if (!loaded) {
            FAIL("a plugin built against an older ABI no longer loads: "
                 << loaded.error().message << " (" << loaded.error().detail << ")\n"
                 << "ADR 0011 promises this binary keeps working. The HOST is wrong, not the "
                    "exhibit. Do not refresh it.");
        }

        // Loading is not enough. The plugin's own descriptor must have come back, and it must
        // still identify itself: a host that opens an old library and then ignores what it says
        // has broken the promise just as thoroughly, and more quietly.
        REQUIRE(loaded.value().desc != nullptr);
        CHECK(loaded.value().desc->id != nullptr);
        CHECK_FALSE(loaded.value().manifest.id.empty());

        // And the generation it was built against must be the one it still reports. If this ever
        // reads as the CURRENT minor, the exhibit has been rebuilt and the museum is measuring
        // nothing.
        CHECK(loaded.value().desc->abi_major == CAD_ABI_VERSION_MAJOR);
    }
}

TEST_CASE("an exhibit's bytes have not changed", "[museum][abi]") {
    if (!cad::testing::canLoadPlugins()) SKIP("this platform only permits loading code shipped inside the signed app bundle");
    // The museum only means anything if the binaries are frozen, so the recorded digest is
    // VERIFIED rather than left for a human. An earlier version of this test checked only that a
    // file was present and larger than a kilobyte, and recorded the sha256 in PROVENANCE for
    // someone to check by hand -- which is the same shape as a manifest that is allowed to
    // disagree with its library: a number nothing compares is a number that can drift.
    //
    // It is not a security boundary. Anyone who can overwrite an exhibit can also edit its
    // PROVENANCE. It catches the accident this is actually exposed to: a build system writing a
    // fresh artefact over a frozen one that happens to share its name.
    const std::vector<fs::path> all = exhibits();
    INFO("triple: " << triple() << ", museum: " << museumRoot().string());
    if (all.empty()) {
        // Same distinction as above. Verifying the digests of no files is not a passing check.
        if (!museumHasAnyExhibit()) FAIL(emptyMuseumFailure());
        SKIP(missingExhibitSkip());
    }

    for (const fs::path& dir : all) {
        INFO("exhibit: " << dir.string());
        const fs::path provenance = dir / "PROVENANCE";
        REQUIRE(fs::exists(provenance));

        const std::string recorded = provenanceValue(provenance, "sha256");
        INFO("PROVENANCE records sha256 = " << recorded);
        REQUIRE_FALSE(recorded.empty());

        bool sawLibrary = false;
        for (const auto& file : fs::directory_iterator(dir)) {
            const std::string name = file.path().filename().string();
            if (name == "PROVENANCE" || name == "plugin.manifest") continue;
            sawLibrary = true;
            CHECK(file.file_size() > 1024);

            const std::string actual = sha256Of(file.path());
            INFO("actual sha256 of " << name << " = " << actual);
            if (actual != recorded) {
                FAIL("exhibit " << name << " does not match the digest recorded beside it. If a "
                     "build overwrote it, restore the frozen binary from git rather than updating "
                     "PROVENANCE -- refreshing the record is how the museum stops measuring "
                     "anything.");
            }
        }
        CHECK(sawLibrary);
    }
}
