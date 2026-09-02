#pragma once

/// Asking the model a question.
///
/// # Why this is a core system and not a tool
///
/// Modelling is a loop: make a change, check it, adjust. Until now vCAD could do the first and the
/// third and not the second — `Shape::measure` and `Shape::volume` existed in this header's
/// neighbour and nothing above the kernel ever called them, so there was no way for a user to learn
/// how long an edge is. A modeller you cannot interrogate is one you have to trust, and nobody
/// trusts a CAD system they cannot check.
///
/// # What it deliberately answers
///
/// The questions an engineer asks first, and no more: how long, how big, how far apart, how wide is
/// that hole. Angles between faces, minimum radius of curvature, mass given a density, and section
/// properties are all real questions and none of them is the first one anybody asks.
///
/// Radius is here because "what is the diameter of this hole" is the single most common measurement
/// in mechanical CAD, and answering it from a circular edge or a cylindrical face is the difference
/// between a measure tool and a curiosity.

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"

namespace cad::kernel {

/// What one element measures.
///
/// Fields are filled wherever they MEAN something, which is more of them than one kind each: a
/// solid has a total edge length as well as a volume, and a face's `length` is its perimeter --
/// which the readout above this depends on and labels as such. What is left at zero is what the
/// element genuinely has none of, and `volume` on a face is the case that matters: a face bounds
/// nothing, so it is not asked for rather than reported as zero.
///
/// Lengths are millimetres and areas are square millimetres, like everything else below the
/// display layer. Formatting into the user's units happens once, at the boundary.
struct Measurement {
    ShapeType type = ShapeType::Unknown;

    double length = 0.0;   ///< an edge's length, or a wire's
    double area = 0.0;     ///< a face's area, or a shell's or solid's total surface
    double volume = 0.0;   ///< a solid's enclosed volume

    /// Circular radius, when the element has one: a CIRCULAR edge, or a cylindrical or spherical
    /// face. Not an ellipse -- an ellipse has two radii and reporting one of them as "the radius"
    /// would be a wrong answer rather than a missing one.
    ///
    /// `hasRadius` rather than a zero test, because a radius of zero is a degenerate element rather
    /// than an absent answer.
    double radius = 0.0;
    bool hasRadius = false;

    /// Centre of mass — a vertex's position, an edge's midpoint, a face's centroid.
    double centre[3]{};
};

/// Measures one element. Fails rather than guessing for a null shape.
[[nodiscard]] Result<Measurement> measure(const Shape&);

/// The shortest distance between two shapes, in millimetres.
///
/// Zero when they touch or intersect, which is a real answer and not an error: "these two faces are
/// 0 apart" is what a user needs to hear when checking a fit.
[[nodiscard]] Result<double> distanceBetween(const Shape&, const Shape&);

}  // namespace cad::kernel
