#pragma once

/// The error and result types the whole application shares.
///
/// # Why this is not in core/kernel any more
///
/// It was, and that made every module wanting to report an error link the B-rep kernel. `cad_units`
/// pulled in OCCT to say "that unit is not recognised" -- not because units have anything to do
/// with solids, but because `Result` happened to be filed next to them. An error type is the most
/// widely used thing in a codebase and therefore the worst possible thing to put behind a heavy
/// dependency.
///
/// `ErrorCode` still names kernel conditions (`BooleanFailed`, `NamingLost`) and that is fine: it
/// is a fixed vocabulary crossing the C ABI as an integer, and inventing a second parallel one for
/// non-geometry modules would mean translating between them at every boundary. Coarse and shared
/// beats precise and duplicated.

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace cad::base {

/// Why an operation failed. Deliberately coarse: the UI needs to say something useful about a
/// red feature in the tree, and finer detail belongs in `detail`.
enum class ErrorCode {
    Ok = 0,
    InvalidInput,       ///< caller handed us a null/empty/degenerate shape
    NotDone,            ///< BRepBuilderAPI_MakeShape::IsDone() == false
    InvalidResult,      ///< operation "succeeded" but BRepCheck_Analyzer rejects the result
    BooleanFailed,      ///< BOPAlgo reported errors
    Unsupported,        ///< we do not implement this case (yet)
    NamingLost,         ///< the op produced topology we could not name; see docs/decisions/0005
    KernelException,    ///< OCCT threw
    Cancelled,
    Internal,
};

const char* toString(ErrorCode) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;   ///< user-facing, already legible; no OCCT jargon
    std::string detail;    ///< developer-facing: exception text, BOP error codes, shape stats

    Error() = default;
    Error(ErrorCode c, std::string msg, std::string det = {})
        : code(c), message(std::move(msg)), detail(std::move(det)) {}
};

/// Minimal Result. We do not use std::expected yet because Apple Clang's availability
/// annotations on <expected> complicate the iOS build; revisit at M3.
template <class T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }

    template <class U>
    [[nodiscard]] T valueOr(U&& fallback) const& {
        return ok() ? value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, Error> storage_;
};

/// Void specialisation for operations performed for effect.
template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}        // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const& { return *error_; }

private:
    std::optional<Error> error_;
};

}  // namespace cad::base

/// Early-return on failure, propagating the error. Usage:
///   CAD_TRY(auto shape, makeBox(...));
#define CAD_DETAIL_CAT2(a, b) a##b
#define CAD_DETAIL_CAT(a, b) CAD_DETAIL_CAT2(a, b)
#define CAD_TRY(decl, expr)                                        \
    auto&& CAD_DETAIL_CAT(_cad_r_, __LINE__) = (expr);             \
    if (!CAD_DETAIL_CAT(_cad_r_, __LINE__)) {                      \
        return CAD_DETAIL_CAT(_cad_r_, __LINE__).error();          \
    }                                                              \
    decl = std::move(CAD_DETAIL_CAT(_cad_r_, __LINE__)).value()
