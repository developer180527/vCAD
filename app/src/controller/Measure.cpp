/// Turning a selection into an answer.
///
/// The kernel measures; this decides WHICH measurements a given selection deserves, and formats
/// them in the user's units. Both halves are model rules rather than Qt ones -- "two selected edges
/// means the user wants the distance between them" is as true on the iPad -- so they live here and
/// each shell only draws the rows.

#include "Internal.h"

#include "cad/kernel/Measurement.h"

namespace cad::app {

namespace {

/// A point, in display units. Formatted per axis rather than as one string so the units appear
/// where a reader expects them, on each number.
std::string pointText(const double p[3], units::UnitSystem display) {
    return units::format(units::Length::fromBase(p[0]), display) + ", "
           + units::format(units::Length::fromBase(p[1]), display) + ", "
           + units::format(units::Length::fromBase(p[2]), display);
}

/// Area, in the SQUARE of the display unit. There is no Quantity for area, and inventing one for a
/// readout would be a dimension system's worth of work for one string -- but reporting mm² to
/// someone working in inches would be worse than the arithmetic here.
std::string areaText(double squareMillimetres, units::UnitSystem display) {
    const double perUnit = units::parseLength("1", display).ok()
                               ? units::parseLength("1", display).value().base()
                               : 1.0;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4g %s²", squareMillimetres / (perUnit * perUnit),
                  units::suffix(display));
    return buffer;
}

std::string volumeText(double cubicMillimetres, units::UnitSystem display) {
    const double perUnit = units::parseLength("1", display).ok()
                               ? units::parseLength("1", display).value().base()
                               : 1.0;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4g %s³",
                  cubicMillimetres / (perUnit * perUnit * perUnit), units::suffix(display));
    return buffer;
}

}   // namespace

std::vector<Controller::MeasureRow> Controller::measureSelection() const {
    std::vector<MeasureRow> rows;
    const auto display = preferences_.displayUnits;

    // Everything the user has picked, of any kind, in one list -- because what to measure depends
    // on how MANY things are selected as much as on what they are.
    std::vector<kernel::Shape> picked;
    const auto selected = selectionByKind();
    for (const auto* group : {&selected.faces, &selected.edges, &selected.vertices}) {
        for (const auto& element : *group) {
            const auto object = history_.current().find(element.object);
            if (!object || object->output() == nullptr) continue;
            if (auto shape = object->output()->map.resolve(element.element)) {
                picked.push_back(std::move(*shape));
            }
        }
    }
    // No elements picked: fall back to whole bodies, which is what a click on nothing-in-particular
    // leaves selected and is still a fair question to ask.
    if (picked.empty()) {
        for (const auto& id : selection_) {
            const auto object = history_.current().find(id);
            if (object && object->output() != nullptr) picked.push_back(object->output()->shape);
        }
    }
    if (picked.empty()) return rows;

    if (picked.size() == 1) {
        const auto measured = kernel::measure(picked.front());
        if (!measured) return rows;
        const auto& m = measured.value();

        switch (m.type) {
            case kernel::ShapeType::Vertex:
                rows.push_back({"Position", pointText(m.centre, display)});
                break;
            case kernel::ShapeType::Edge:
                rows.push_back({"Length",
                                units::format(units::Length::fromBase(m.length), display)});
                if (m.hasRadius) {
                    rows.push_back({"Radius",
                                    units::format(units::Length::fromBase(m.radius), display)});
                    // Diameter as well as radius. A drawing calls a hole by its diameter and a
                    // machinist orders a drill by it, so making the user double it is making them
                    // do arithmetic the tool exists to avoid.
                    rows.push_back({"Diameter", units::format(
                                                    units::Length::fromBase(m.radius * 2.0),
                                                    display)});
                }
                rows.push_back({"Midpoint", pointText(m.centre, display)});
                break;
            case kernel::ShapeType::Face:
                rows.push_back({"Area", areaText(m.area, display)});
                rows.push_back({"Perimeter",
                                units::format(units::Length::fromBase(m.length), display)});
                if (m.hasRadius) {
                    rows.push_back({"Radius",
                                    units::format(units::Length::fromBase(m.radius), display)});
                    rows.push_back({"Diameter", units::format(
                                                    units::Length::fromBase(m.radius * 2.0),
                                                    display)});
                }
                rows.push_back({"Centroid", pointText(m.centre, display)});
                break;
            default:
                if (m.volume != 0.0) rows.push_back({"Volume", volumeText(m.volume, display)});
                rows.push_back({"Surface area", areaText(m.area, display)});
                rows.push_back({"Centre of mass", pointText(m.centre, display)});
                break;
        }
        return rows;
    }

    // Two or more: the distance, which is the question that needs two of anything.
    if (const auto gap = kernel::distanceBetween(picked[0], picked[1])) {
        rows.push_back({"Distance",
                        units::format(units::Length::fromBase(gap.value()), display)});
    }
    // And what each one is worth on its own, so picking a second thing never takes information
    // away -- a readout that shrinks when you add to the selection reads as a bug.
    for (std::size_t i = 0; i < picked.size() && i < 2; ++i) {
        const auto measured = kernel::measure(picked[i]);
        if (!measured) continue;
        const auto& m = measured.value();
        const std::string which = i == 0 ? "First" : "Second";
        if (m.type == kernel::ShapeType::Edge) {
            rows.push_back({which + " length",
                            units::format(units::Length::fromBase(m.length), display)});
        } else if (m.type == kernel::ShapeType::Face) {
            rows.push_back({which + " area", areaText(m.area, display)});
        }
    }
    return rows;
}

}  // namespace cad::app
