#pragma once

/// Naming serials for shapes the HOST builds on a plugin's behalf, outside any feature compute.
///
/// # The problem this exists for
///
/// A host-built shape has to be named, and its naming serial is derived from the REQUEST -- the
/// operation and its arguments -- so that identical calls name identically. Counting interns
/// instead would make the names depend on session history, and two runs of the same plugin would
/// disagree about what a face is called.
///
/// But `NameStep::featureSerial` is 32 bits, and that width is written into saved documents, so the
/// derived value must be folded down to fit. Folding puts the birthday bound at roughly 2^16
/// distinct requests -- inside what a long-running plugin session reaches -- and a collision means
/// two different requests mint the SAME names for different geometry. A reference to a face of one
/// shape would then be a perfectly valid reference to a face of the other.
///
/// # Why detect rather than avoid
///
/// The field cannot be widened without changing a persisted format. Probing for the next free
/// serial would work, but it makes the answer depend on the order requests arrived in, which
/// destroys the one property the derivation exists to provide.
///
/// So the collision is caught instead: the full 64-bit fingerprint is kept beside the folded
/// serial, and a serial that comes back for a different fingerprint is refused rather than reused.
/// The same move `ElementMap::collisions` makes one layer down -- stop trying to be lucky, notice
/// when you were not.
///
/// Inside a feature compute none of this applies: the serial there is the feature's object id,
/// which is unique by construction. This is only for host calls made outside one.

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace cad::abi {

class SerialLedger {
public:
    /// The serial for a request fingerprint, or nothing if that serial already belongs to a
    /// DIFFERENT request. Asking twice for the same fingerprint returns the same serial, which is
    /// the point: identical calls must name identically.
    [[nodiscard]] std::optional<std::uint32_t> serialFor(std::uint64_t fingerprint) {
        // Never zero: 0 is what an uninitialised serial looks like, so a name carrying it would
        // collide with anything built before its serial was set -- invisibly, rather than merely
        // wrongly.
        //
        // The remapping does put a fingerprint whose halves cancel on top of whatever genuinely
        // folds to 1, which makes serial 1 likelier to collide than chance alone suggests. That is
        // fine, and deliberately not special-cased: it is caught by the same check as any other
        // collision, and one more branch here would be a second thing to get right.
        const auto folded =
            static_cast<std::uint32_t>((fingerprint >> 32) ^ (fingerprint & 0xFFFFFFFFULL));
        const std::uint32_t serial = folded == 0 ? 1u : folded;

        const auto [entry, inserted] = seen_.emplace(serial, fingerprint);
        if (inserted || entry->second == fingerprint) return serial;
        return std::nullopt;
    }

    [[nodiscard]] std::size_t size() const noexcept { return seen_.size(); }

private:
    std::unordered_map<std::uint32_t, std::uint64_t> seen_;
};

}  // namespace cad::abi
