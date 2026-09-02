#pragma once

/// What a feature needs before it can run, said once.
///
/// # The bug this exists to stop
///
/// A feature type used to declare a name, a version and a compute function -- and nothing about its
/// own inputs. So four separate places had to agree, independently, on what "Hole" requires:
///
///   * the command's enable predicate, deciding whether the button lights up;
///   * the command panel, listing the values to type;
///   * the feature's own guard, re-checking the selection before it builds anything;
///   * and the property names all three store and read.
///
/// They drift. Hole and Revolve were both UNREACHABLE for a period -- greyed out with exactly the
/// right thing selected -- because the predicate and the implementation had come to disagree about
/// what "selected" meant. That was the fourth fix of the same shape, and the fourth is the one that
/// says the shape is the problem rather than the instances.
///
/// A feature now says "I take one planar face, a diameter and a depth" in one place, and the
/// enablement, the panel, the refusal message and the stored property names are all derived from
/// that sentence.
///
/// # Deliberately a small vocabulary
///
/// This is not a query language. It describes the selections real features actually ask for, and a
/// feature whose requirement it cannot express keeps its own predicate -- Fillet does, because "one
/// body that HAS edges" needs to look at the geometry rather than at the selection. Inventing a
/// general expression system to avoid one exception would be a larger thing to get wrong than the
/// exception is.

#include "cad/kernel/Shape.h"

#include <cstddef>
#include <string>
#include <vector>

namespace cad::recompute {

struct FeatureInputs {
    /// One acceptable shape of selection.
    struct Requirement {
        /// What kind of thing is picked. `Object` means a whole feature in the tree; the rest are
        /// sub-elements of one.
        enum class Of : std::uint8_t { Object, Face, Edge, Vertex };

        Of of = Of::Object;
        std::size_t least = 1;
        std::size_t most = 1;   ///< 0 means no upper bound

        /// The feature type the selection must belong to, or empty for any. For an element, this
        /// is the type of the object that OWNS it -- a Revolve's axis has to be an edge of the
        /// sketch being revolved, because the axis is resolved in that sketch's own element map.
        std::string ownerType;
    };

    /// Any ONE of these satisfies the feature. Empty means it needs no selection at all, which is
    /// the correct answer for a primitive.
    ///
    /// Alternatives rather than a conjunction because that is what features actually want: Fillet
    /// takes picked edges OR a whole body. No built-in needs two different kinds at once, and a
    /// vocabulary that supported it would be carrying weight nothing uses.
    std::vector<Requirement> accepts;

    /// What to tell the user when nothing acceptable is selected. One string, so the button's
    /// tooltip, the panel's refusal and the feature's own guard cannot say three different things.
    std::string prompt;

    /// A value the user types. Stored under `name`, which is also the property the compute reads --
    /// so the panel and the compute cannot disagree about what a field is called.
    struct Value {
        enum class Kind : std::uint8_t { Length, Angle, Number, Count, Bool };

        std::string name;
        std::string label;
        Kind kind = Kind::Length;

        /// The default, in BASE units -- millimetres, radians. Never in display units: the
        /// declaration is a property of the feature and must not change meaning with a preference.
        double base = 0.0;
    };
    std::vector<Value> values;
};

}  // namespace cad::recompute
