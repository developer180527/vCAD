#include "cad/naming/ElementName.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>
#include <sstream>

namespace cad::naming {
namespace {

// FNV-1a. Placeholder for BLAKE3-256, which arrives with assetlib at M2. What matters now
// is that it is a pure function of the input bytes with no pointer or allocation influence,
// so digests are identical across processes and machines.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

inline void mix(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= kFnvPrime;
    }
}

}  // namespace

const char* toString(Provenance p) noexcept {
    switch (p) {
        case Provenance::Primitive: return "P";
        case Provenance::Generated: return "G";
        case Provenance::Modified:  return "M";
        case Provenance::Boundary:  return "B";
        case Provenance::Merged:    return "U";
    }
    return "?";
}

ElementName::ElementName(std::vector<NameStep> steps) : steps_(std::move(steps)) {
    for (auto& s : steps_) {
        // Canonical parent order. Without this, {Top,Front} and {Front,Top} would be
        // different names for the same edge depending on traversal order.
        std::sort(s.parents.begin(), s.parents.end());
    }
    recomputeDigest();
}

void ElementName::recomputeDigest() noexcept {
    std::uint64_t h = kFnvOffset;
    for (const auto& s : steps_) {
        mix(h, s.featureSerial);
        mix(h, s.opTag);
        mix(h, static_cast<std::uint64_t>(s.provenance));
        mix(h, s.discriminator);
        for (auto p : s.parents) mix(h, p);
        mix(h, 0x5eULL);  // step separator
    }
    digest_ = h;
}

ElementName ElementName::derive(NameStep step) const {
    std::vector<NameStep> next = steps_;
    std::sort(step.parents.begin(), step.parents.end());
    next.push_back(std::move(step));
    return ElementName(std::move(next));
}

ElementName ElementName::family() const {
    if (steps_.empty()) return *this;
    std::vector<NameStep> copy = steps_;
    copy.back().discriminator = 0;
    return ElementName(std::move(copy));
}

bool operator<(const ElementName& a, const ElementName& b) {
    // Full lexicographic order over the chain, NOT over digests. Merge canonicalisation
    // depends on this being a stable, meaningful order rather than a hash order.
    const auto& as = a.steps_;
    const auto& bs = b.steps_;
    const std::size_t n = std::min(as.size(), bs.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (as[i].featureSerial != bs[i].featureSerial) return as[i].featureSerial < bs[i].featureSerial;
        if (as[i].opTag != bs[i].opTag) return as[i].opTag < bs[i].opTag;
        if (as[i].provenance != bs[i].provenance) return as[i].provenance < bs[i].provenance;
        if (as[i].parents != bs[i].parents) return as[i].parents < bs[i].parents;
        if (as[i].discriminator != bs[i].discriminator) return as[i].discriminator < bs[i].discriminator;
    }
    return as.size() < bs.size();
}

std::string ElementName::toString() const {
    std::ostringstream os;
    bool first = true;
    for (const auto& s : steps_) {
        if (!first) os << '/';
        first = false;
        // Qualified: the member ElementName::toString would otherwise hide the free function.
        os << cad::naming::toString(s.provenance) << s.featureSerial << '.' << s.opTag << '#' << s.discriminator;
        if (!s.parents.empty()) {
            os << '[';
            for (std::size_t i = 0; i < s.parents.size(); ++i) {
                if (i) os << ',';
                char buf[17];
                std::snprintf(buf, sizeof(buf), "%016llx",
                              static_cast<unsigned long long>(s.parents[i]));
                os << buf;
            }
            os << ']';
        }
    }
    return os.str();
}

namespace {

/// Reads a decimal integer that must fill `text` exactly and fit in T.
///
/// Both halves matter. `sscanf("%llu")` succeeded on a value far larger than the field it was
/// assigned to and the result was silently truncated -- `P4294967296.0#0` parsed as feature serial
/// 0 -- and it also succeeded while leaving trailing junk unread, so text that was not in the
/// format at all was accepted as though it were.
template <class T>
bool readWhole(std::string_view text, T& out) {
    if (text.empty()) return false;
    // from_chars on an unsigned type rejects a sign and leading space, which is what we want: the
    // writer never emits either, so accepting them would widen the format by accident.
    std::uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [stopped, ec] = std::from_chars(begin, end, value, 10);
    if (ec != std::errc{} || stopped != end) return false;
    if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) return false;
    out = static_cast<T>(value);
    return true;
}

}  // namespace

ElementName ElementName::parse(std::string_view text) {
    // A strict inverse of toString, and strict on purpose.
    //
    // These strings arrive from saved documents, from DDC cache blobs, and across the C ABI from
    // plugins and from the iPad shell -- so some of this input is genuinely untrusted. The previous
    // parser accepted several kinds of text that toString cannot produce: an unterminated parent
    // list, a parent digest with trailing junk, out-of-range numbers silently truncated into their
    // fields, and empty path components, so that `P1.0#0`, `/P1.0#0` and `P1.0#0//P2.0#0` were all
    // the same name.
    //
    // A malformed reference must be REFUSED, not reinterpreted. Every caller already treats a null
    // result as "this document contains a geometric reference we cannot read"; the failure that
    // matters is the one where a wrong-but-plausible name resolves to real geometry.
    //
    // The empty string is the null name, which is what toString writes for it.
    if (text.empty()) return {};

    std::vector<NameStep> steps;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t slash = text.find('/', pos);
        const std::string_view token =
            slash == std::string_view::npos ? text.substr(pos) : text.substr(pos, slash - pos);
        // Empty means a leading, trailing or doubled separator. The writer emits exactly one '/'
        // between steps and none at either end.
        if (token.empty()) return {};

        NameStep step;
        switch (token[0]) {
            case 'P': step.provenance = Provenance::Primitive; break;
            case 'G': step.provenance = Provenance::Generated; break;
            case 'M': step.provenance = Provenance::Modified;  break;
            case 'B': step.provenance = Provenance::Boundary;  break;
            case 'U': step.provenance = Provenance::Merged;    break;
            default:  return {};
        }

        const std::string_view body = token.substr(1);
        std::string_view head = body;
        std::string_view parents;

        if (const std::size_t bracket = body.find('['); bracket != std::string_view::npos) {
            // The list must be closed, and closed at the very END of the step.
            if (body.empty() || body.back() != ']') return {};
            head = body.substr(0, bracket);
            parents = body.substr(bracket + 1, body.size() - bracket - 2);
            if (parents.empty()) return {};   // toString never writes an empty list
        } else if (body.find(']') != std::string_view::npos) {
            return {};   // a closing bracket with nothing to close
        }

        const std::size_t dot = head.find('.');
        const std::size_t hash = head.find('#');
        if (dot == std::string_view::npos || hash == std::string_view::npos || hash < dot) {
            return {};
        }
        if (!readWhole(head.substr(0, dot), step.featureSerial)) return {};
        if (!readWhole(head.substr(dot + 1, hash - dot - 1), step.opTag)) return {};
        if (!readWhole(head.substr(hash + 1), step.discriminator)) return {};

        while (!parents.empty()) {
            const std::size_t comma = parents.find(',');
            const std::string_view item =
                comma == std::string_view::npos ? parents : parents.substr(0, comma);
            // Exactly the sixteen hex digits toString writes. Checking the length is what rejects
            // a digest that was truncated in transit, which from_chars alone would happily read as
            // a smaller number.
            if (item.size() != 16) return {};
            std::uint64_t digest = 0;
            const char* const begin = item.data();
            const char* const end = begin + item.size();
            const auto [stopped, ec] = std::from_chars(begin, end, digest, 16);
            if (ec != std::errc{} || stopped != end) return {};
            step.parents.push_back(digest);
            if (comma == std::string_view::npos) break;
            parents = parents.substr(comma + 1);
            if (parents.empty()) return {};   // a trailing comma
        }

        steps.push_back(std::move(step));
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }

    return ElementName(std::move(steps));
}

}  // namespace cad::naming
