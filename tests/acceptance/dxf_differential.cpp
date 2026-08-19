// Differential testing of the two DXF readers: the same bytes, both parsers, compared.
//
// # What this proves that nothing else does
//
// The Rust parser has its own unit tests, and it passes the DXF acceptance tests that dime passes.
// Both facts are about ONE parser measured against expectations someone wrote down. This is a
// different kind of statement: two independently written parsers, given bytes neither author
// anticipated, agreeing about what those bytes MEAN.
//
// That catches the class of bug tests written alongside a parser cannot. A test asserts what its
// author believed; where the author misread the format, the test agrees with the mistake. The
// other parser did not make the same mistake, because it was written from the format by someone
// else at a different time -- so a disagreement is a signal that at least one of them is wrong,
// with no need to have guessed in advance which case would expose it.
//
// # Where the two are ENTITLED to differ, and why that is not noise
//
// A naive "the results must be identical" assertion fails immediately, and each reason is real:
//
//   * PRECISION. dime declares `typedef float dxfdouble` and holds coordinates in single
//     precision; the Rust reader parses f64. Comparisons here are to a relative tolerance near
//     1e-6, which is dime's floor and is documented on importDxf.
//
//   * ERROR POLICY. dime returns nothing usable for a malformed file, so that path refuses the
//     whole import. The Rust reader keeps what parsed and counts the rest. On mutated input the
//     two therefore disagree about SUCCESS constantly, by design.
//
// So the assertions are shaped around what a disagreement would actually mean:
//
//   1. On well-formed input, the two must agree on the geometry, to dime's precision.
//   2. When BOTH succeed on mutated input, they must agree on the geometry. This is the real
//      differential: two parsers reaching the same answer about a file neither expected.
//   3. Rust refusing a file dime accepted is a REGRESSION -- the user loses a file that used to
//      open -- and is bounded. The opposite direction is the intended policy difference and is
//      merely counted.
//
// Assertion 3 is the one that had to be calibrated against a measured rate rather than guessed;
// see the constant.

#include "cad/sketch/Dxf.h"
#include "cad/sketch/Sketch.h"

#include <catch2/catch_test_macros.hpp>
#include "Platform.h"

// std::max over an initializer list. Included explicitly because libc++ happens to pull
// <algorithm> in through <string> and libstdc++ does not -- so omitting it builds on macOS and
// fails on gcc, which is exactly the class of bug the Linux job exists to catch.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <fstream>
#include <string>
#include <vector>

using namespace cad;

namespace {

/// xorshift64. Deterministic, so a failure is reproducible from its seed forever -- the same
/// generator and the same mutation kinds as the Rust fuzzer in `tests-rs`, so a case found there
/// can be reproduced here and the two corpora are comparable rather than merely similar.
struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
};

std::vector<std::uint8_t> readFixture() {
    const std::filesystem::path path =
        std::filesystem::path(CAD_REPO_ROOT) / "tests/data/sketch_profile.dxf";
    std::ifstream in(path, std::ios::binary);
    INFO("fixture " << path.string());
    REQUIRE(in);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The minimum DXF the importer accepts, built rather than stored.
///
/// Mutating a large file mostly produces garbage far from the interesting paths; mutating a small
/// one lands on structure -- section markers, group codes, counts -- which is where a parser
/// breaks and where two parsers are most likely to disagree.
std::vector<std::uint8_t> minimalSeed() {
    const std::string text =
        "0\nSECTION\n2\nENTITIES\n"
        "0\nLINE\n8\n0\n10\n0\n20\n0\n11\n10\n21\n10\n"
        "0\nCIRCLE\n8\n0\n10\n5\n20\n5\n40\n2.5\n"
        // Group 90, the vertex count, is not optional in practice: dime reads it and ignores an
        // LWPOLYLINE without one. A seed missing it makes dime silently skip the entity and every
        // mutation of it produce a structural disagreement that says nothing about either parser.
        "0\nLWPOLYLINE\n8\n0\n90\n3\n70\n1\n10\n0\n20\n0\n10\n4\n20\n0\n10\n4\n20\n4\n"
        "0\nARC\n8\n0\n10\n1\n20\n1\n40\n2\n50\n0\n51\n90\n"
        "0\nENDSEC\n0\nEOF\n";
    return {text.begin(), text.end()};
}

std::vector<std::uint8_t> mutate(std::vector<std::uint8_t> bytes, Rng& rng) {
    // One to three edits, where the Rust fuzzer next door uses up to eight. The two harnesses
    // want different things from a corpus: the fuzzer wants damage, because it is asking whether
    // a parser survives, and a file mangled beyond recognition still answers that. This is asking
    // whether two parsers AGREE, which only has an answer when both can still read the file --
    // and at eight edits almost nothing survives both, so the comparison stops happening.
    const int edits = static_cast<int>(rng.below(3)) + 1;
    for (int i = 0; i < edits && !bytes.empty(); ++i) {
        switch (rng.below(7)) {
            case 0:   // flip a byte
                bytes[rng.below(bytes.size())] = static_cast<std::uint8_t>(rng.below(256));
                break;
            case 1:   // truncate
                bytes.resize(rng.below(bytes.size()));
                break;
            case 2:   // insert a byte
                bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(rng.below(bytes.size())),
                             static_cast<std::uint8_t>(rng.below(256)));
                break;
            case 3: {   // a huge number where a coordinate or count goes
                const std::string big = "999999999999999999999";
                const auto at = static_cast<std::ptrdiff_t>(rng.below(bytes.size()));
                bytes.insert(bytes.begin() + at, big.begin(), big.end());
                break;
            }
            case 4: {   // a run of digits into a NaN or an infinity
                const std::string poison = (rng.next() & 1u) ? "NaN" : "-inf";
                const auto at = static_cast<std::ptrdiff_t>(rng.below(bytes.size()));
                bytes.insert(bytes.begin() + at, poison.begin(), poison.end());
                break;
            }
            case 5:   // delete a byte, which reliably breaks group-code/value pairing
                bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(rng.below(bytes.size())));
                break;
            default: {   // duplicate a span
                const auto from = rng.below(bytes.size());
                const auto len = std::min(rng.below(64) + 1, bytes.size() - from);
                bytes.insert(bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(from),
                             bytes.begin() + static_cast<std::ptrdiff_t>(from + len));
                break;
            }
        }
    }
    return bytes;
}

/// A temporary file that removes itself. The importer takes a path, not bytes.
class TempDxf {
public:
    explicit TempDxf(const std::vector<std::uint8_t>& bytes) {
        path_ = std::filesystem::temp_directory_path() /
                ("cad-diff-" + processToken() + "-" + std::to_string(counter_++) + ".dxf");
        std::ofstream out(path_, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ~TempDxf() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TempDxf(const TempDxf&) = delete;
    TempDxf& operator=(const TempDxf&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    /// A token unique to this PROCESS, mixed into every temp filename.
    ///
    /// Catch2 registers this case through `catch_discover_tests` AND it has its own named ctest
    /// entry, so two processes run it -- and under `CTEST_PARALLEL_LEVEL` they run at the same
    /// time. With a bare per-process counter both wrote `cad-diff-0.dxf`, `cad-diff-1.dxf` and so
    /// on into one shared temp directory, each clobbering the other's file between the write and
    /// the two reads.
    ///
    /// The symptom was worth the diagnosis: the same deterministic corpus produced different
    /// counts in the two runs, and a "value disagreement" of 4.0 against 10.0 -- not a precision
    /// difference but two readers looking at different FILES. It appeared the day parallel ctest
    /// was switched on and would have been blamed on the parser. The Rust fuzzer next door already
    /// puts `std::process::id()` in its names for the same reason.
    ///
    /// `std::random_device` rather than a process id because there is no portable one in standard
    /// C++, and this needs no platform headers to be right on all five targets.
    static const std::string& processToken() {
        static const std::string token = [] {
            std::random_device rd;
            return std::to_string((static_cast<std::uint64_t>(rd()) << 32) | rd());
        }();
        return token;
    }

    std::filesystem::path path_;
    static inline int counter_ = 0;
};

/// dime's precision floor: it holds coordinates as float, so agreement can only be asserted to
/// about seven significant digits however precisely the file states them.
constexpr double kTolerance = 1e-6;

bool nearlyEqual(double a, double b) {
    if (a == b) return true;
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= kTolerance * scale;
}

/// The two ways two readers can disagree, which are not equally interesting.
///
/// STRUCTURAL means they read a different set of entities -- a different count, or a different
/// kind at the same position. On corrupt input that is expected, because the readers have
/// different and documented policies about salvage. dime's `atof` accepts a numeric PREFIX, so
/// "2NaN" becomes a radius of 2 and the arc survives; the Rust reader refuses the token and drops
/// the entity. Neither is obviously wrong -- though inventing a radius from a corrupt token is how
/// a part gets machined to a number nobody wrote -- and on a well-formed file the question never
/// arises.
///
/// VALUE means they read the SAME entities and disagree about the numbers. That is not a policy
/// difference. It means one of them is misreading the format, and it is the finding this whole
/// test exists for.
///
/// Order is compared, not sorted away. Both readers walk the file in document order and hand
/// entities to the same builder, so a difference in order is a real difference in how the file was
/// read -- sorting first would hide exactly the bug most worth finding.
enum class Disagreement { None, Structural, Value };

struct Comparison {
    Disagreement kind = Disagreement::None;
    std::string detail;
};

Comparison compare(const sketch::Sketch& a, const sketch::Sketch& b) {
    const auto& ga = a.geometry();
    const auto& gb = b.geometry();
    if (ga.size() != gb.size()) {
        return {Disagreement::Structural, "geometry count " + std::to_string(ga.size()) + " vs " +
                                              std::to_string(gb.size())};
    }
    for (std::size_t i = 0; i < ga.size(); ++i) {
        if (ga[i].kind != gb[i].kind) {
            return {Disagreement::Structural, "geometry " + std::to_string(i) + " kind " +
                                                  sketch::toString(ga[i].kind) + " vs " +
                                                  sketch::toString(gb[i].kind)};
        }
    }
    for (std::size_t i = 0; i < ga.size(); ++i) {
        if (ga[i].construction != gb[i].construction) {
            return {Disagreement::Value,
                    "geometry " + std::to_string(i) + " differs in construction flag"};
        }
        for (std::size_t p = 0; p < ga[i].p.size(); ++p) {
            if (!nearlyEqual(ga[i].p[p], gb[i].p[p])) {
                return {Disagreement::Value, "geometry " + std::to_string(i) + " parameter " +
                                                 std::to_string(p) + ": " +
                                                 std::to_string(ga[i].p[p]) + " vs " +
                                                 std::to_string(gb[i].p[p])};
            }
        }
    }
    return {};
}

}  // namespace

TEST_CASE("the two DXF readers agree on a well-formed file", "[dxf][differential]") {
    if (!cad::testing::hasRepoFixtures()) SKIP("the repository's DXF corpus is not reachable from here");
    if (!io::hasRustDxfReader()) {
        SUCCEED("build has no Rust reader; nothing to compare against");
        return;
    }

    const auto bytes = readFixture();
    const TempDxf file(bytes);

    io::DxfImportReport rustReport;
    io::DxfImportReport dimeReport;
    auto viaRust = io::importDxfWith(io::DxfReader::Rust, file.path(), {}, &rustReport);
    auto viaDime = io::importDxfWith(io::DxfReader::Dime, file.path(), {}, &dimeReport);

    REQUIRE(viaRust);
    REQUIRE(viaDime);

    // Every counter, not just the geometry. The report is what the user is shown, and two readers
    // producing the same sketch while disagreeing about how much was dropped would still be a bug
    // -- in what the application TELLS someone about their file.
    CHECK(rustReport.lines == dimeReport.lines);
    CHECK(rustReport.arcs == dimeReport.arcs);
    CHECK(rustReport.circles == dimeReport.circles);
    CHECK(rustReport.points == dimeReport.points);
    CHECK(rustReport.polylines == dimeReport.polylines);
    CHECK(rustReport.construction == dimeReport.construction);
    CHECK(rustReport.degenerate == dimeReport.degenerate);
    CHECK(rustReport.flattenedBulges == dimeReport.flattenedBulges);
    CHECK(rustReport.unsupported == dimeReport.unsupported);

    const auto difference = compare(viaRust.value(), viaDime.value());
    INFO(difference.detail);
    CHECK(difference.kind == Disagreement::None);
}

TEST_CASE("the two DXF readers agree whenever both accept a mutated file",
          "[dxf][differential]") {
    if (!io::hasRustDxfReader()) {
        SUCCEED("build has no Rust reader; nothing to compare against");
        return;
    }

    const std::vector<std::vector<std::uint8_t>> seeds{readFixture(), minimalSeed()};

    Rng rng{0x9E3779B97F4A7C15ull};
    int bothAccepted = 0;
    int bothRefused = 0;
    int onlyRust = 0;    // Rust accepted, dime refused -- the intended policy difference
    int onlyDime = 0;    // dime accepted, Rust refused -- a REGRESSION, bounded below
    std::vector<std::string> valueDisagreements;
    int structural = 0;
    std::vector<std::uint64_t> regressionSeeds;
    std::map<std::string, int> rustRefusals;

    constexpr int kCases = 600;
    for (int i = 0; i < kCases; ++i) {
        const std::uint64_t caseSeed = rng.state;
        const auto& seed = seeds[static_cast<std::size_t>(i) % seeds.size()];
        const TempDxf file(mutate(seed, rng));

        auto viaRust = io::importDxfWith(io::DxfReader::Rust, file.path());
        auto viaDime = io::importDxfWith(io::DxfReader::Dime, file.path());

        if (viaRust && viaDime) {
            ++bothAccepted;
            const auto difference = compare(viaRust.value(), viaDime.value());
            if (difference.kind == Disagreement::Structural) {
                ++structural;
            } else if (difference.kind == Disagreement::Value && valueDisagreements.size() < 8) {
                // The FILE, kept, not just the seed. Reproducing from a seed means replaying the
                // generator and knowing which corpus the case came from; a copy of the bytes can
                // be opened in another program, which is the first thing anyone will want to do
                // when two parsers disagree about what they mean.
                const auto kept = std::filesystem::temp_directory_path() /
                                  ("cad-differ-" + std::to_string(caseSeed) + ".dxf");
                std::error_code ignored;
                std::filesystem::copy_file(
                    file.path(), kept, std::filesystem::copy_options::overwrite_existing, ignored);
                valueDisagreements.push_back(difference.detail + "  (" + kept.string() + ")");
            }
        } else if (viaRust) {
            ++onlyRust;
        } else if (viaDime) {
            ++onlyDime;
            // Tallied by MESSAGE, because the bound below is only defensible if this direction is
            // dominated by the known salvage-policy difference. "Rust refused a file dime read"
            // is alarming as a raw count and unremarkable if every one of them is the Rust reader
            // declining to invent a number out of a corrupt token.
            ++rustRefusals[viaRust.error().message];
            if (regressionSeeds.size() < 8) regressionSeeds.push_back(caseSeed);
        } else {
            ++bothRefused;
        }
    }

    // One string, not an INFO per item. A scoped INFO inside a loop is destroyed at the end of its
    // iteration, so by the time the CHECK below fails every message has gone -- which is how the
    // first run of this test reported that disagreements existed and not one of them.
    std::string detail = "both accepted " + std::to_string(bothAccepted) + ", both refused " +
                         std::to_string(bothRefused) + ", only Rust " + std::to_string(onlyRust) +
                         ", only dime " + std::to_string(onlyDime) + ", structural " +
                         std::to_string(structural);
    for (const auto& text : valueDisagreements) detail += "\n  " + text;
    for (const auto& [message, count] : rustRefusals) {
        detail += "\n  dime-only, Rust said (x" + std::to_string(count) + "): " + message;
    }
    for (const auto& seed : regressionSeeds) {
        detail += "\n  dime-only seed " + std::to_string(seed);
    }
    INFO(detail);

    // THE assertion. Two parsers that read the same entities out of a file neither author
    // anticipated, and disagree about the NUMBERS, cannot both be right. Zero, with no tolerance
    // for a rate: unlike a structural difference there is no policy that explains one away.
    CHECK(valueDisagreements.empty());

    // The corpus has to actually exercise that comparison. Without this the test passes trivially
    // when every mutation is refused by one side or the other, and "no disagreements" would mean
    // "nothing was compared" -- a counter agreeing with itself.
    CHECK(bothAccepted > kCases / 8);

    // dime accepting a file the Rust reader refuses is the direction that costs a user something:
    // a drawing that used to open and no longer does. Bounded rather than forbidden, because the
    // salvage policy makes some of it inevitable -- see the measured note below.
    CHECK(onlyDime <= kCases / 20);
}

// The measured shape of this run when the bounds above were set, so a future change can be
// compared against a number rather than an impression:
//
//     both accepted 156, both refused 303, only Rust 125, only dime 16
//     value disagreements 0
//     of the 16 dime-only: 15 "contains no geometry we can import", 1 truncation
//
// The bounds are roughly a factor of two either side of that -- tight enough that a real
// regression trips them, loose enough that a mutation corpus's natural variance does not.
//
// What those numbers looked like BEFORE the two bugs this test found is the reason it exists:
// both accepted 78, only dime 94. The Rust reader was rejecting any file with a single corrupt
// group code anywhere as "not an ASCII DXF", and the CMake rule that builds it was not watching
// the file that contained the bug. Neither is visible from inside the parser's own test suite.
//
// `only Rust` being large (125) is the intended asymmetry and not a defect: dime returns nothing
// usable for a file it cannot fully parse, while the Rust reader keeps what parsed and counts the
// rest. That is the partial-import policy documented on importDxf, working.