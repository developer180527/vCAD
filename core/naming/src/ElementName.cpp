#include "cad/naming/ElementName.h"

#include <algorithm>
#include <cstdio>
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

ElementName ElementName::parse(std::string_view text) {
    std::vector<NameStep> steps;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('/', pos);
        // A '/' inside a bracketed parent list would break this; parents are hex digests
        // and commas, so it cannot occur.
        if (end == std::string_view::npos) end = text.size();
        std::string_view tok = text.substr(pos, end - pos);
        pos = end + 1;
        if (tok.empty()) continue;

        NameStep step;
        switch (tok[0]) {
            case 'P': step.provenance = Provenance::Primitive; break;
            case 'G': step.provenance = Provenance::Generated; break;
            case 'M': step.provenance = Provenance::Modified;  break;
            case 'B': step.provenance = Provenance::Boundary;  break;
            case 'U': step.provenance = Provenance::Merged;    break;
            default:  return {};
        }
        std::string body(tok.substr(1));
        unsigned long long feat = 0, op = 0, disc = 0;
        const std::size_t bracket = body.find('[');
        std::string head = body.substr(0, bracket);
        if (std::sscanf(head.c_str(), "%llu.%llu#%llu", &feat, &op, &disc) != 3) return {};
        step.featureSerial = static_cast<std::uint32_t>(feat);
        step.opTag = static_cast<std::uint16_t>(op);
        step.discriminator = static_cast<std::uint32_t>(disc);

        if (bracket != std::string::npos) {
            std::string list = body.substr(bracket + 1);
            if (!list.empty() && list.back() == ']') list.pop_back();
            std::istringstream is(list);
            std::string item;
            while (std::getline(is, item, ',')) {
                if (item.empty()) continue;
                step.parents.push_back(std::stoull(item, nullptr, 16));
            }
        }
        steps.push_back(std::move(step));
    }
    return ElementName(std::move(steps));
}

}  // namespace cad::naming
