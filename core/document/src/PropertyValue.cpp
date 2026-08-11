#include "cad/document/PropertyValue.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace cad::document {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= kFnvPrime;
    }
}

/// Quantise before hashing, exactly as the naming layer does. Two builds of the same model
/// must produce the same key; raw double bits do not survive a recompilation, let alone a
/// different machine. 1e-9 of a millimetre is far below any meaningful CAD tolerance.
std::uint64_t hashReal(double v) {
    if (std::isnan(v)) return 0x7FF8000000000000ULL;
    const long long q = std::llround(v * 1e9);
    return static_cast<std::uint64_t>(q);
}

}  // namespace

PropertyType typeOf(const PropertyValue& v) noexcept {
    return static_cast<PropertyType>(v.index());
}

const char* toString(PropertyType t) noexcept {
    switch (t) {
        case PropertyType::Bool:        return "Bool";
        case PropertyType::Int:         return "Int";
        case PropertyType::Real:        return "Real";
        case PropertyType::Text:        return "Text";
        case PropertyType::Length:      return "Length";
        case PropertyType::Angle:       return "Angle";
        case PropertyType::Object:      return "Object";
        case PropertyType::Element:     return "Element";
        case PropertyType::ElementList: return "ElementList";
        case PropertyType::ObjectList:  return "ObjectList";
    }
    return "Unknown";
}

std::uint64_t digestOf(const PropertyValue& v) noexcept {
    std::uint64_t h = kFnvOffset;
    mix(h, v.index());   // the type is part of the identity: 1 (int) != 1.0 (real)
    std::visit([&h](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, bool>) {
            mix(h, value ? 1u : 0u);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            mix(h, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, double>) {
            mix(h, hashReal(value));
        } else if constexpr (std::is_same_v<T, std::string>) {
            for (char c : value) mix(h, static_cast<std::uint64_t>(c));
        } else if constexpr (std::is_same_v<T, units::Length> ||
                             std::is_same_v<T, units::Angle>) {
            mix(h, hashReal(value.base()));
        } else if constexpr (std::is_same_v<T, ObjectId>) {
            mix(h, value.value);
        } else if constexpr (std::is_same_v<T, naming::ElementName>) {
            mix(h, value.digest());
        } else if constexpr (std::is_same_v<T, std::vector<naming::ElementName>>) {
            for (const auto& n : value) mix(h, n.digest());
        } else if constexpr (std::is_same_v<T, std::vector<ObjectId>>) {
            for (const auto& i : value) mix(h, i.value);
        }
    }, v);
    return h;
}

std::string toString(const PropertyValue& v) {
    std::ostringstream os;
    std::visit([&os](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, bool>) {
            os << (value ? "true" : "false");
        } else if constexpr (std::is_same_v<T, std::int64_t> ||
                             std::is_same_v<T, double>) {
            os << value;
        } else if constexpr (std::is_same_v<T, std::string>) {
            os << '"' << value << '"';
        } else if constexpr (std::is_same_v<T, units::Length>) {
            os << value.base() << "mm";
        } else if constexpr (std::is_same_v<T, units::Angle>) {
            os << units::toDegrees(value) << "deg";
        } else if constexpr (std::is_same_v<T, ObjectId>) {
            os << "#" << value.value;
        } else if constexpr (std::is_same_v<T, naming::ElementName>) {
            os << value.toString();
        } else if constexpr (std::is_same_v<T, std::vector<naming::ElementName>>) {
            os << "[";
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (i) os << ", ";
                os << value[i].toString();
            }
            os << "]";
        } else if constexpr (std::is_same_v<T, std::vector<ObjectId>>) {
            os << "[";
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (i) os << ", ";
                os << "#" << value[i].value;
            }
            os << "]";
        }
    }, v);
    return os.str();
}

}  // namespace cad::document
