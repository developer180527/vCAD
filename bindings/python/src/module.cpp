// Python bindings.
//
// Design rule, from the M2 plan: the core API is designed AS IF Python is the primary
// consumer. If something is awkward to express here, the C++ API is wrong and gets fixed
// rather than papered over with a bespoke shim.
//
// Scope is deliberately the same surface the C ABI exposes — document, recompute, import,
// export — because that is the surface we have committed to keeping stable. Reaching deeper
// into the kernel from Python would create a second contract to maintain.

#include "cad/document/Document.h"
#include "cad/io/Format.h"
#include "cad/naming/ElementMap.h"
#include "cad/recompute/DdcCache.h"
#include "cad/features/Builtins.h"
#include "cad/recompute/Engine.h"
#include "cad/units/Units.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>

namespace py = pybind11;
using namespace cad;

namespace {

/// Turns a core Result into a Python exception. Every fallible call goes through this, so
/// Python users get exceptions with the same messages the UI shows — never a status code.
template <class T>
T unwrap(kernel::Result<T> r) {
    if (!r) {
        const auto& e = r.error();
        const std::string what = e.detail.empty() ? e.message : e.message + " (" + e.detail + ")";
        switch (e.code) {
            case kernel::ErrorCode::InvalidInput:
                throw py::value_error(what);
            case kernel::ErrorCode::Unsupported:
                throw py::type_error(what);
            default:
                throw std::runtime_error(what);
        }
    }
    if constexpr (!std::is_void_v<T>) return std::move(r).value();
}

/// A document, a registry and a cache, matching the C ABI's session. Python owns one of
/// these; everything else hangs off it.
class Session {
public:
    explicit Session(const std::string& cacheDir) {
        auto l0 = std::make_unique<recompute::MemoryCache>();
        if (cacheDir.empty()) {
            cache_ = std::move(l0);
        } else {
            // Opting into the DDC gives this session the shared-tier behaviour: a colleague
            // or CI that already built this geometry means we fetch instead of recompute.
            cache_ = std::make_unique<recompute::TieredCache>(
                std::move(l0), std::make_unique<recompute::DdcCache>(cacheDir));
        }
    }

    document::ObjectId add(const std::string& type) {
        if (registry_.find(type) == nullptr) {
            throw py::type_error("unknown feature type '" + type + "'");
        }
        auto [next, id] = history_.current().add(type);
        history_.commit(std::move(next), "Add " + type);
        return id;
    }

    template <class T>
    void setProperty(document::ObjectId id, const std::string& name, T value,
                     bool cosmetic = false) {
        const auto object = history_.current().find(id);
        if (!object) throw py::value_error("no such object");
        auto updated = object->withProperty(name, document::PropertyValue{std::move(value)},
                                            cosmetic);
        auto next = history_.current().replace(
            std::make_shared<const document::ObjectData>(std::move(updated)));
        next = recompute::Engine::invalidate(next, id);
        history_.commit(std::move(next), "Edit");
    }

    recompute::RecomputeReport recompute() {
        recompute::Engine engine(registry_, *cache_);
        auto result = unwrap(engine.recompute(history_.current()));
        history_.replaceCurrent(std::move(result.first));
        return result.second;
    }

    const document::ObjectData& object(document::ObjectId id) const {
        const auto o = history_.current().find(id);
        if (!o) throw py::value_error("no such object");
        return *o;
    }

    bool undo() { return history_.undo(); }
    bool redo() { return history_.redo(); }
    std::size_t size() const { return history_.current().size(); }
    std::vector<document::ObjectId> ids() const { return history_.current().ids(); }

    void exportTo(document::ObjectId id, const std::string& path) {
        const auto& o = object(id);
        if (o.output() == nullptr) {
            throw std::runtime_error("this object has no geometry yet; recompute first");
        }
        unwrap(io::exportFile(formats_, path, o.output()->shape));
    }

    const io::FormatRegistry& formats() const { return formats_; }
    const recompute::Cache& cache() const { return *cache_; }

private:
    recompute::FeatureRegistry registry_ = features::builtins();
    io::FormatRegistry formats_ = io::FormatRegistry::builtins();
    std::unique_ptr<recompute::Cache> cache_;
    document::History history_{document::Document{}};
};

}  // namespace

PYBIND11_MODULE(_cad, m) {
    m.doc() = "Parametric CAD core";
    m.attr("__version__") = "0.1.0";

    py::enum_<document::ObjectState>(m, "State")
        .value("CLEAN", document::ObjectState::Clean)
        .value("DIRTY", document::ObjectState::Dirty)
        .value("FAILED", document::ObjectState::Failed)
        .value("BLOCKED", document::ObjectState::Blocked);

    py::enum_<units::UnitSystem>(m, "Unit")
        .value("MM", units::UnitSystem::Millimetre)
        .value("CM", units::UnitSystem::Centimetre)
        .value("M", units::UnitSystem::Metre)
        .value("IN", units::UnitSystem::Inch)
        .value("FT", units::UnitSystem::Foot);

    py::class_<document::ObjectId>(m, "ObjectId")
        .def_readonly("value", &document::ObjectId::value)
        .def("__repr__", [](const document::ObjectId& i) {
            return "<ObjectId #" + std::to_string(i.value) + ">";
        })
        .def("__hash__", [](const document::ObjectId& i) { return i.value; })
        .def("__eq__", [](const document::ObjectId& a, const document::ObjectId& b) {
            return a == b;
        });

    py::class_<recompute::RecomputeReport>(m, "RecomputeReport")
        .def_readonly("computed", &recompute::RecomputeReport::computed)
        .def_readonly("cached", &recompute::RecomputeReport::cached)
        .def_readonly("skipped", &recompute::RecomputeReport::skipped)
        .def_readonly("failed", &recompute::RecomputeReport::failed)
        .def_readonly("blocked", &recompute::RecomputeReport::blocked)
        .def("all_succeeded", &recompute::RecomputeReport::allSucceeded)
        .def("__repr__", [](const recompute::RecomputeReport& r) {
            return "<RecomputeReport computed=" + std::to_string(r.computed) +
                   " cached=" + std::to_string(r.cached) +
                   " skipped=" + std::to_string(r.skipped) +
                   " failed=" + std::to_string(r.failed) +
                   " blocked=" + std::to_string(r.blocked) + ">";
        });

    py::class_<io::ImportReport>(m, "ImportReport")
        .def_readonly("format", &io::ImportReport::format)
        .def_readonly("solids", &io::ImportReport::solids)
        .def_readonly("faces", &io::ImportReport::faces)
        .def_readonly("declared_units", &io::ImportReport::declaredUnits)
        .def_readonly("units_were_assumed", &io::ImportReport::unitsWereAssumed)
        .def_readonly("warnings", &io::ImportReport::warnings)
        .def_readonly("unsupported", &io::ImportReport::unsupported)
        .def("lossless", &io::ImportReport::lossless)
        .def("summary", &io::ImportReport::summary);

    py::class_<Session>(m, "Session")
        .def(py::init<const std::string&>(), py::arg("cache_dir") = "",
             "Create a session. Pass cache_dir to enable the on-disk DDC tier;\n"
             "leave it empty for an in-memory cache only.")
        .def("add", &Session::add, py::arg("type"))
        .def("set_length", &Session::setProperty<units::Length>, py::arg("obj"),
             py::arg("name"), py::arg("value"), py::arg("cosmetic") = false)
        .def("set_real", &Session::setProperty<double>, py::arg("obj"), py::arg("name"),
             py::arg("value"), py::arg("cosmetic") = false)
        .def("set_text", &Session::setProperty<std::string>, py::arg("obj"), py::arg("name"),
             py::arg("value"), py::arg("cosmetic") = false)
        .def("set_input", &Session::setProperty<document::ObjectId>, py::arg("obj"),
             py::arg("name"), py::arg("value"), py::arg("cosmetic") = false)
        .def("set_element", &Session::setProperty<naming::ElementName>, py::arg("obj"),
             py::arg("name"), py::arg("value"), py::arg("cosmetic") = false)
        .def("recompute", &Session::recompute)
        .def("undo", &Session::undo)
        .def("redo", &Session::redo)
        .def("ids", &Session::ids)
        .def("__len__", &Session::size)
        .def("state", [](const Session& s, document::ObjectId id) {
            return s.object(id).state();
        })
        .def("error", [](const Session& s, document::ObjectId id) {
            return s.object(id).error().message;
        })
        .def("error_detail", [](const Session& s, document::ObjectId id) {
            return s.object(id).error().detail;
        })
        .def("face_count", [](const Session& s, document::ObjectId id) {
            const auto* o = s.object(id).output();
            if (o == nullptr) throw std::runtime_error("no geometry yet; recompute first");
            return o->shape.subShapes(kernel::ShapeType::Face).size();
        })
        .def("volume", [](const Session& s, document::ObjectId id) {
            const auto* o = s.object(id).output();
            if (o == nullptr) throw std::runtime_error("no geometry yet; recompute first");
            return o->shape.volume();
        })
        .def("content_hash", [](const Session& s, document::ObjectId id) {
            const auto* o = s.object(id).output();
            return o == nullptr ? std::string{}
                                : naming::contentHash(o->shape, o->map).hex();
        })
        .def("cache_key", [](const Session& s, document::ObjectId id) {
            return s.object(id).cacheKey();
        })
        .def("cache_stats", [](const Session& s) {
            return py::make_tuple(s.cache().hits(), s.cache().misses());
        })
        .def("box_edge_between", [](const Session& s, document::ObjectId id, int a, int b) {
            const auto* out = s.object(id).output();
            if (out == nullptr) throw std::runtime_error("no geometry yet; recompute first");
            const auto faceName = [&](int f) {
                naming::NameStep step;
                step.featureSerial = static_cast<std::uint32_t>(id.value);
                step.provenance = naming::Provenance::Primitive;
                step.discriminator = static_cast<std::uint32_t>(f);
                return naming::ElementName({step});
            };
            naming::NameStep step;
            step.provenance = naming::Provenance::Boundary;  // no feature serial, by design
            step.parents = {faceName(a).digest(), faceName(b).digest()};
            const naming::ElementName edge({step});
            return out->map.resolveAll(edge).empty() ? naming::ElementName{} : edge;
        })
        .def("export_file", &Session::exportTo, py::arg("obj"), py::arg("path"))
        .def("readable_extensions", [](const Session& s) {
            return s.formats().readableExtensions();
        })
        .def("writable_extensions", [](const Session& s) {
            return s.formats().writableExtensions();
        });

    py::class_<naming::ElementName>(m, "ElementName")
        .def(py::init<>())
        .def_static("parse", &naming::ElementName::parse)
        .def("is_null", &naming::ElementName::isNull)
        .def("__str__", &naming::ElementName::toString)
        .def("__repr__", [](const naming::ElementName& n) {
            return "<ElementName " + n.toString() + ">";
        })
        .def("__bool__", [](const naming::ElementName& n) { return !n.isNull(); });

    py::class_<units::Length>(m, "Length")
        .def_static("mm", &units::millimetres)
        .def_static("inch", &units::inches)
        .def("to_mm", &units::toMillimetres)
        .def("__repr__", [](units::Length l) {
            return "<Length " + std::to_string(l.base()) + "mm>";
        });

    m.def("parse_length",
          [](const std::string& text, units::UnitSystem assumed) {
              return unwrap(units::parseLength(text, assumed)).base();
          },
          py::arg("text"), py::arg("assumed") = units::UnitSystem::Millimetre,
          "Parse a length to millimetres. `assumed` applies only when the text carries no "
          "unit suffix — the core never guesses.");

    m.def("import_file",
          [](const std::string& path, units::UnitSystem assumed) {
              static const io::FormatRegistry registry = io::FormatRegistry::builtins();
              io::ImportOptions options;
              options.assumedUnits = assumed;
              auto result = unwrap(io::importFile(registry, path, options));
              return result.report;
          },
          py::arg("path"), py::arg("assumed_units") = units::UnitSystem::Millimetre,
          "Read a file and return its import report. The geometry itself is reached by\n"
          "adding an 'Import' feature to a Session.");
}
