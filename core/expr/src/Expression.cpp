#include "cad/expr/Expression.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <unordered_set>

namespace cad::expr {
namespace {

using base::Error;
using base::ErrorCode;

Error bad(std::string message, std::size_t at) {
    return Error{ErrorCode::InvalidInput, std::move(message),
                 "at character " + std::to_string(at + 1)};
}

// ---------------------------------------------------------------------------------------------
// Dimensions

Dim add(Dim a, Dim b) {
    return Dim{a.length + b.length, a.mass + b.mass, a.time + b.time, a.angle + b.angle};
}
Dim subtract(Dim a, Dim b) {
    return Dim{a.length - b.length, a.mass - b.mass, a.time - b.time, a.angle - b.angle};
}
Dim scale(Dim d, int n) {
    return Dim{d.length * n, d.mass * n, d.time * n, d.angle * n};
}

/// Millimetres per unit of the display system, obtained by asking core/units rather than keeping a
/// second copy of the table. Two tables of unit factors is one table too many: they diverge on the
/// day someone adds a unit to one of them, and the divergence is a silent scale error.
double lengthFactorOf(units::UnitSystem system) {
    const auto one = units::parseLength("1", system);
    return one.ok() ? one.value().base() : 1.0;
}

// ---------------------------------------------------------------------------------------------
// Tokens

enum class TokenKind : std::uint8_t { Number, Name, Operator, LParen, RParen, Comma, End };

struct Token {
    TokenKind kind = TokenKind::End;
    std::size_t position = 0;
    std::string text;    ///< the name, or the operator character
    Value value;         ///< for Number
    bool suffixed = false;   ///< the literal carried a unit ("10mm"), as opposed to a bare "10"
};

/// True for a byte that may appear in a unit suffix. Includes bytes above ASCII so that the degree
/// sign survives: "45°" arrives as two bytes, neither of which is a letter.
bool suffixByte(unsigned char c) {
    return std::isalpha(c) != 0 || c >= 0x80 || c == '"' || c == '\'';
}

bool nameStart(unsigned char c) { return std::isalpha(c) != 0 || c == '_'; }
bool nameByte(unsigned char c) { return std::isalnum(c) != 0 || c == '_'; }

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Whether a word is a unit rather than a name. Asked of core/units instead of duplicating its
/// table: "is 'furlong' a unit" has exactly one correct answer and one place that knows it.
bool isUnitWord(std::string_view word) {
    if (word.empty()) return false;
    return units::parseLength("1" + std::string(word), units::UnitSystem::Millimetre).ok()
           || units::parseAngle("1" + std::string(word)).ok();
}

bool isAngleSuffix(std::string_view suffix) {
    const std::string s = lower(suffix);
    return s == "deg" || s == "degree" || s == "degrees" || s == "rad" || s == "radian"
           || s == "radians" || s == "\xc2\xb0";
}

base::Result<std::vector<Token>> tokenize(std::string_view text, units::UnitSystem assumed) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isspace(c) != 0) { ++i; continue; }

        if (std::isdigit(c) != 0 || (c == '.' && i + 1 < text.size()
                                     && std::isdigit(static_cast<unsigned char>(text[i + 1])) != 0)) {
            const std::string buf(text.substr(i));
            char* end = nullptr;
            const double number = std::strtod(buf.c_str(), &end);
            const std::size_t consumed = static_cast<std::size_t>(end - buf.c_str());

            // The suffix may be separated from the number by spaces -- "10 mm" is what a person
            // types, and units::parseLength has always accepted it. But a gap makes the following
            // word ambiguous with a name, so a detached word is only taken as a unit if it IS one:
            // "10 mm" is a length, "2 width" stays the syntax error it should be.
            std::size_t gap = consumed;
            while (gap < buf.size() && std::isspace(static_cast<unsigned char>(buf[gap])) != 0) {
                ++gap;
            }
            std::size_t n = gap;
            while (n < buf.size() && suffixByte(static_cast<unsigned char>(buf[n]))) ++n;
            std::string suffix = buf.substr(gap, n - gap);
            if (gap != consumed && !isUnitWord(suffix)) {
                suffix.clear();
                n = consumed;
            }

            Token token;
            token.kind = TokenKind::Number;
            token.position = i;
            token.suffixed = !suffix.empty();
            if (suffix.empty()) {
                // Bare: it does not know what it is yet. See Expression.h.
                token.value = Value{number, kScalar, /*unitless=*/true};
            } else if (isAngleSuffix(suffix)) {
                const auto angle = units::parseAngle(buf.substr(0, consumed) + suffix);
                if (!angle.ok()) return angle.error();
                token.value = Value{angle.value().base(), kAngle, false};
            } else {
                // Any other suffix must be a length, and units:: owns the table of which ones are.
                const auto length = units::parseLength(buf.substr(0, consumed) + suffix, assumed);
                if (!length.ok()) {
                    return bad("'" + suffix + "' is not a unit I recognise.", i + gap);
                }
                token.value = Value{length.value().base(), kLength, false};
            }
            out.push_back(std::move(token));
            i += n;
            continue;
        }

        if (nameStart(c)) {
            std::size_t n = i;
            while (n < text.size() && nameByte(static_cast<unsigned char>(text[n]))) ++n;
            out.push_back(Token{TokenKind::Name, i, std::string(text.substr(i, n - i)), {}, false});
            i = n;
            continue;
        }

        switch (c) {
            case '(': out.push_back(Token{TokenKind::LParen, i, "(", {}, false}); ++i; continue;
            case ')': out.push_back(Token{TokenKind::RParen, i, ")", {}, false}); ++i; continue;
            case ',': out.push_back(Token{TokenKind::Comma, i, ",", {}, false}); ++i; continue;
            case '+': case '-': case '*': case '/': case '^':
                out.push_back(Token{TokenKind::Operator, i, std::string(1, static_cast<char>(c)),
                                    {}, false});
                ++i;
                continue;
            default: break;
        }
        return bad(std::string("I don't understand '") + static_cast<char>(c) + "' here.", i);
    }
    out.push_back(Token{TokenKind::End, text.size(), "", {}, false});
    return out;
}

// ---------------------------------------------------------------------------------------------
// Built-ins

const std::unordered_set<std::string>& functionNames() {
    static const std::unordered_set<std::string> names{
        "sin",  "cos",  "tan",  "asin", "acos",  "atan", "atan2", "sqrt",
        "abs",  "min",  "max",  "floor", "ceil", "round", "pow",   "log",
        "ln",   "log10", "exp"};
    return names;
}

const std::unordered_set<std::string>& constantNames() {
    static const std::unordered_set<std::string> names{"pi", "e"};
    return names;
}

// ---------------------------------------------------------------------------------------------
// Parser
//
// Recursive descent, one pass, evaluating as it goes. No AST: nothing needs one. The dependency
// graph wants NAMES, which referencedNames collects from the token stream directly, and nobody
// asks to re-evaluate an expression without re-reading its text.

class Parser {
public:
    Parser(std::vector<Token> tokens, units::UnitSystem assumed, const Resolver& resolver)
        : tokens_(std::move(tokens)), assumed_(assumed), resolver_(resolver) {}

    base::Result<Value> run() {
        auto value = additive();
        if (!value.ok()) return value.error();
        if (peek().kind != TokenKind::End) {
            return bad("There is something extra after the end of this expression.",
                       peek().position);
        }
        return value;
    }

private:
    const Token& peek() const { return tokens_[index_]; }
    const Token& take() { return tokens_[index_++]; }

    bool atOperator(char c) const {
        return peek().kind == TokenKind::Operator && peek().text[0] == c;
    }

    /// A bare number takes on the dimension it is being combined with. Only + and - do this: see
    /// the header for why `width * 2` must NOT.
    base::Result<Value> adopt(Value bare, Dim target, std::size_t at) {
        if (target == kLength) return Value{bare.magnitude * lengthFactorOf(assumed_), kLength, false};
        if (target == kAngle) return Value{units::degrees(bare.magnitude).base(), kAngle, false};
        if (target.isScalar()) return Value{bare.magnitude, kScalar, true};
        return bad("A number with no unit can't be combined with a value in " + toString(target)
                       + ". Give it a unit.",
                   at);
    }

    base::Result<Value> additive() {
        auto left = multiplicative();
        if (!left.ok()) return left.error();

        for (;;) {
            char op = 0;
            if (atOperator('+')) op = '+';
            else if (atOperator('-')) op = '-';
            else if (peek().kind == TokenKind::Number && peek().suffixed && lastWasSuffixed_) {
                // Juxtaposition: "2ft 6in", "2' 6\"". Still normal in US mechanical drawings, and
                // units::parseLength already accepts it, so an expression field that refused it
                // would be a downgrade for exactly the users least willing to tolerate one.
                //
                // Narrow on purpose: BOTH sides must be unit-suffixed literals. `width 2` stays the
                // syntax error it is.
                op = '+';
                const auto position = peek().position;
                const auto right = multiplicative();
                if (!right.ok()) return right.error();
                auto sum = combineAdditive(left.value(), right.value(), '+', position);
                if (!sum.ok()) return sum.error();
                left = sum.value();
                continue;
            }
            if (op == 0) break;

            const auto position = take().position;
            const auto right = multiplicative();
            if (!right.ok()) return right.error();
            auto sum = combineAdditive(left.value(), right.value(), op, position);
            if (!sum.ok()) return sum.error();
            left = sum.value();
        }
        return left;
    }

    base::Result<Value> combineAdditive(Value a, Value b, char op, std::size_t at) {
        if (a.dim != b.dim) {
            if (a.unitless) {
                const auto adopted = adopt(a, b.dim, at);
                if (!adopted.ok()) return adopted.error();
                a = adopted.value();
            } else if (b.unitless) {
                const auto adopted = adopt(b, a.dim, at);
                if (!adopted.ok()) return adopted.error();
                b = adopted.value();
            } else {
                return bad("You can't " + std::string(op == '+' ? "add " : "subtract ")
                               + toString(b.dim) + (op == '+' ? " to " : " from ") + toString(a.dim)
                               + ".",
                           at);
            }
        }
        return Value{op == '+' ? a.magnitude + b.magnitude : a.magnitude - b.magnitude, a.dim,
                     a.unitless && b.unitless};
    }

    base::Result<Value> multiplicative() {
        auto left = power();
        if (!left.ok()) return left.error();

        while (atOperator('*') || atOperator('/')) {
            const bool multiply = atOperator('*');
            const auto position = take().position;
            const auto right = power();
            if (!right.ok()) return right.error();
            const Value a = left.value();
            const Value b = right.value();

            if (!multiply && b.magnitude == 0.0) return bad("This divides by zero.", position);

            // A unitless operand acts as a pure multiplier here -- it does NOT adopt. That is the
            // whole reason `width * 2` is 2 x width rather than width + 2mm.
            Value result;
            result.magnitude = multiply ? a.magnitude * b.magnitude : a.magnitude / b.magnitude;
            // No adoption here, and that is the point: a bare number in a product is a pure
            // multiplier. `width * 2` is twice the width; `width + 2` is two display units more
            // than it. One rule cannot serve both, so this one deliberately differs from
            // combineAdditive. A unitless value already carries a scalar dim, so the arithmetic
            // below needs no special case -- only the absence of the adopt() call.
            result.dim = multiply ? add(a.dim, b.dim) : subtract(a.dim, b.dim);
            result.unitless = result.dim.isScalar() && (a.unitless || b.unitless);
            left = result;
        }
        return left;
    }

    base::Result<Value> power() {
        auto base = unary();
        if (!base.ok()) return base.error();
        if (!atOperator('^')) return base;

        const auto position = take().position;
        const auto exponent = power();   // right-associative: 2^3^2 is 2^9
        if (!exponent.ok()) return exponent.error();
        return raise(base.value(), exponent.value(), position);
    }

    base::Result<Value> raise(Value base, Value exponent, std::size_t at) {
        if (!exponent.dim.isScalar()) {
            return bad("An exponent has to be a plain number, not " + toString(exponent.dim) + ".",
                       at);
        }
        if (base.dim.isScalar()) {
            const double v = std::pow(base.magnitude, exponent.magnitude);
            if (!std::isfinite(v)) return bad("That power isn't a real number.", at);
            return Value{v, kScalar, base.unitless && exponent.unitless};
        }
        // Dimensioned base: mm^2.5 is not a unit anything can hold, so the exponent must be whole.
        const double rounded = std::round(exponent.magnitude);
        if (std::abs(exponent.magnitude - rounded) > 1e-9) {
            return bad("A value in " + toString(base.dim)
                           + " can only be raised to a whole power.",
                       at);
        }
        return Value{std::pow(base.magnitude, rounded), scale(base.dim, static_cast<int>(rounded)),
                     false};
    }

    base::Result<Value> unary() {
        if (atOperator('-')) {
            take();
            auto v = unary();
            if (!v.ok()) return v.error();
            Value negated = v.value();
            negated.magnitude = -negated.magnitude;
            return negated;
        }
        if (atOperator('+')) {
            take();
            return unary();
        }
        return primary();
    }

    base::Result<Value> primary() {
        lastWasSuffixed_ = false;
        const Token& token = peek();

        if (token.kind == TokenKind::Number) {
            lastWasSuffixed_ = token.suffixed;
            return take().value;
        }

        if (token.kind == TokenKind::LParen) {
            take();
            auto inner = additive();
            if (!inner.ok()) return inner.error();
            if (peek().kind != TokenKind::RParen) {
                return bad("This bracket is never closed.", token.position);
            }
            take();
            lastWasSuffixed_ = false;
            return inner;
        }

        if (token.kind == TokenKind::Name) {
            const std::string name = lower(token.text);
            const std::size_t position = token.position;
            take();
            if (peek().kind == TokenKind::LParen) return call(name, position);
            if (name == "pi") return Value{std::numbers::pi, kScalar, false};
            if (name == "e") return Value{std::numbers::e, kScalar, false};
            if (functionNames().count(name) != 0) {
                return bad("'" + token.text + "' is a function; it needs brackets, like " + name
                               + "(...).",
                           position);
            }
            if (!resolver_) {
                return bad("There are no parameters here, so '" + token.text + "' means nothing.",
                           position);
            }
            const auto found = resolver_(token.text);
            if (!found) return bad("There is no parameter called '" + token.text + "'.", position);
            return *found;
        }

        if (token.kind == TokenKind::End) return bad("This expression is incomplete.",
                                                    token.position);
        return bad("'" + token.text + "' can't start a value.", token.position);
    }

    base::Result<Value> call(const std::string& name, std::size_t at) {
        take();   // '('
        std::vector<Value> args;
        if (peek().kind != TokenKind::RParen) {
            for (;;) {
                auto arg = additive();
                if (!arg.ok()) return arg.error();
                args.push_back(arg.value());
                if (peek().kind != TokenKind::Comma) break;
                take();
            }
        }
        if (peek().kind != TokenKind::RParen) return bad("This bracket is never closed.", at);
        take();
        lastWasSuffixed_ = false;
        return apply(name, args, at);
    }

    base::Result<Value> wrongArity(const std::string& name, std::size_t want, std::size_t got,
                                   std::size_t at) {
        return bad(name + " takes " + std::to_string(want) + (want == 1 ? " value" : " values")
                       + ", not " + std::to_string(got) + ".",
                   at);
    }

    base::Result<Value> apply(const std::string& name, std::vector<Value>& args, std::size_t at) {
        if (functionNames().count(name) == 0) {
            return bad("There is no function called '" + name + "'.", at);
        }
        const std::size_t want = (name == "atan2" || name == "min" || name == "max"
                                  || name == "pow")
                                     ? 2
                                     : 1;
        if (args.size() != want) return wrongArity(name, want, args.size(), at);

        const auto scalarOnly = [&](const Value& v) -> std::optional<Error> {
            if (v.dim.isScalar()) return std::nullopt;
            return Error{ErrorCode::InvalidInput,
                         name + " takes a plain number, not " + toString(v.dim) + ".",
                         "at character " + std::to_string(at + 1)};
        };

        if (name == "sin" || name == "cos" || name == "tan") {
            Value a = args[0];
            if (a.dim.isScalar()) {
                // A bare number in an angle position is degrees -- the same convention
                // units::parseAngle uses, so `sin(30)` and `45` in an angle field agree.
                const auto adopted = adopt(Value{a.magnitude, kScalar, true}, kAngle, at);
                if (!adopted.ok()) return adopted.error();
                a = adopted.value();
            } else if (a.dim != kAngle) {
                return bad(name + " takes an angle, not " + toString(a.dim) + ".", at);
            }
            const double r = name == "sin"   ? std::sin(a.magnitude)
                             : name == "cos" ? std::cos(a.magnitude)
                                             : std::tan(a.magnitude);
            if (!std::isfinite(r)) return bad("That angle has no tangent.", at);
            return Value{r, kScalar, false};
        }

        if (name == "asin" || name == "acos" || name == "atan") {
            if (auto e = scalarOnly(args[0])) return *e;
            const double x = args[0].magnitude;
            if (name != "atan" && (x < -1.0 || x > 1.0)) {
                return bad(name + " only works between -1 and 1.", at);
            }
            const double r = name == "asin"   ? std::asin(x)
                             : name == "acos" ? std::acos(x)
                                              : std::atan(x);
            return Value{r, kAngle, false};
        }

        if (name == "atan2") {
            if (args[0].dim != args[1].dim) {
                return bad("atan2 needs both values in the same units.", at);
            }
            return Value{std::atan2(args[0].magnitude, args[1].magnitude), kAngle, false};
        }

        if (name == "sqrt") {
            const Dim d = args[0].dim;
            if (d.length % 2 != 0 || d.mass % 2 != 0 || d.time % 2 != 0 || d.angle % 2 != 0) {
                return bad("The square root of " + toString(d) + " isn't a unit anything can hold.",
                           at);
            }
            if (args[0].magnitude < 0.0) return bad("That square root isn't a real number.", at);
            return Value{std::sqrt(args[0].magnitude),
                         Dim{d.length / 2, d.mass / 2, d.time / 2, d.angle / 2}, false};
        }

        if (name == "abs") {
            Value v = args[0];
            v.magnitude = std::abs(v.magnitude);
            return v;
        }

        if (name == "min" || name == "max") {
            Value a = args[0];
            Value b = args[1];
            if (a.dim != b.dim) {
                if (a.unitless) {
                    const auto adopted = adopt(a, b.dim, at);
                    if (!adopted.ok()) return adopted.error();
                    a = adopted.value();
                } else if (b.unitless) {
                    const auto adopted = adopt(b, a.dim, at);
                    if (!adopted.ok()) return adopted.error();
                    b = adopted.value();
                } else {
                    return bad(name + " needs both values in the same units.", at);
                }
            }
            const bool takeA = name == "min" ? a.magnitude <= b.magnitude
                                             : a.magnitude >= b.magnitude;
            Value chosen = takeA ? a : b;
            chosen.unitless = a.unitless && b.unitless;
            return chosen;
        }

        if (name == "floor" || name == "ceil" || name == "round") {
            // Scalar only, deliberately. `round(width)` would have to round to SOME unit, and
            // whichever one it picked would be an invisible assumption in a value the user is
            // about to cut metal with. `round(width/1mm) * 1mm` says which.
            if (auto e = scalarOnly(args[0])) return *e;
            const double x = args[0].magnitude;
            const double r = name == "floor"  ? std::floor(x)
                             : name == "ceil" ? std::ceil(x)
                                              : std::round(x);
            return Value{r, kScalar, args[0].unitless};
        }

        if (name == "pow") return raise(args[0], args[1], at);

        // log/ln/log10/exp
        if (auto e = scalarOnly(args[0])) return *e;
        const double x = args[0].magnitude;
        if ((name == "log" || name == "ln" || name == "log10") && x <= 0.0) {
            return bad("You can only take the logarithm of a positive number.", at);
        }
        const double r = name == "exp"     ? std::exp(x)
                         : name == "log10" ? std::log10(x)
                                           : std::log(x);
        return Value{r, kScalar, false};
    }

    std::vector<Token> tokens_;
    std::size_t index_ = 0;
    units::UnitSystem assumed_;
    const Resolver& resolver_;

    /// Whether the primary just parsed was a unit-suffixed literal, which is the only situation in
    /// which juxtaposition means addition.
    bool lastWasSuffixed_ = false;
};

/// Turns a scalar or unitless result into the dimension a field asked for. Separate from adopt()
/// because the rules differ: mid-expression only a UNITLESS value may adopt, but a field that asked
/// for a length and got a plain number is in exactly the position units::parseLength is in when
/// handed "10" -- it applies the display unit, and always has.
base::Result<Value> coerce(Value v, Dim target, units::UnitSystem assumed, const char* what) {
    if (v.dim == target) return v;
    if (v.dim.isScalar()) {
        if (target == kLength) return Value{v.magnitude * lengthFactorOf(assumed), kLength, false};
        if (target == kAngle) return Value{units::degrees(v.magnitude).base(), kAngle, false};
    }
    return Error{ErrorCode::InvalidInput,
                 "This needs " + std::string(what) + ", but that works out to " + toString(v.dim)
                     + "."};
}

}  // namespace

std::string toString(Dim d) {
    if (d.isScalar()) return "a plain number";

    const auto part = [](const char* unit, int exponent, std::string& into) {
        if (exponent == 0) return;
        if (!into.empty()) into += "*";
        into += unit;
        if (std::abs(exponent) != 1) into += "^" + std::to_string(std::abs(exponent));
    };
    std::string numerator;
    std::string denominator;
    part("mm", d.length > 0 ? d.length : 0, numerator);
    part("kg", d.mass > 0 ? d.mass : 0, numerator);
    part("s", d.time > 0 ? d.time : 0, numerator);
    part("rad", d.angle > 0 ? d.angle : 0, numerator);
    part("mm", d.length < 0 ? d.length : 0, denominator);
    part("kg", d.mass < 0 ? d.mass : 0, denominator);
    part("s", d.time < 0 ? d.time : 0, denominator);
    part("rad", d.angle < 0 ? d.angle : 0, denominator);

    if (numerator.empty()) numerator = "1";
    return denominator.empty() ? numerator : numerator + "/" + denominator;
}

base::Result<Value> evaluate(std::string_view text, units::UnitSystem assumedLength,
                             const Resolver& resolver) {
    const auto tokens = tokenize(text, assumedLength);
    if (!tokens.ok()) return tokens.error();
    if (tokens.value().size() == 1) {   // End only
        return Error{ErrorCode::InvalidInput, "Enter a value."};
    }
    Parser parser(tokens.value(), assumedLength, resolver);
    return parser.run();
}

base::Result<units::Length> evaluateLength(std::string_view text, units::UnitSystem assumed,
                                           const Resolver& resolver) {
    const auto value = evaluate(text, assumed, resolver);
    if (!value.ok()) return value.error();
    const auto length = coerce(value.value(), kLength, assumed, "a length");
    if (!length.ok()) return length.error();
    return units::Length::fromBase(length.value().magnitude);
}

base::Result<units::Angle> evaluateAngle(std::string_view text, const Resolver& resolver) {
    const auto value = evaluate(text, units::UnitSystem::Millimetre, resolver);
    if (!value.ok()) return value.error();
    const auto angle = coerce(value.value(), kAngle, units::UnitSystem::Millimetre, "an angle");
    if (!angle.ok()) return angle.error();
    return units::Angle::fromBase(angle.value().magnitude);
}

base::Result<double> evaluateNumber(std::string_view text, const Resolver& resolver) {
    const auto value = evaluate(text, units::UnitSystem::Millimetre, resolver);
    if (!value.ok()) return value.error();
    if (!value.value().dim.isScalar()) {
        return Error{ErrorCode::InvalidInput, "This needs a plain number, but that works out to "
                                                  + toString(value.value().dim) + "."};
    }
    return value.value().magnitude;
}

base::Result<std::vector<std::string>> referencedNames(std::string_view text) {
    const auto tokens = tokenize(text, units::UnitSystem::Millimetre);
    if (!tokens.ok()) return tokens.error();

    std::vector<std::string> names;
    const auto& list = tokens.value();
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].kind != TokenKind::Name) continue;
        // A name followed by '(' is a call, and a call is not a dependency on a parameter.
        if (i + 1 < list.size() && list[i + 1].kind == TokenKind::LParen) continue;
        const std::string key = lower(list[i].text);
        if (constantNames().count(key) != 0 || functionNames().count(key) != 0) continue;
        if (std::find(names.begin(), names.end(), list[i].text) == names.end()) {
            names.push_back(list[i].text);
        }
    }
    return names;
}

bool isValidName(std::string_view name) {
    if (name.empty()) return false;
    if (!nameStart(static_cast<unsigned char>(name.front()))) return false;
    for (const char c : name) {
        if (!nameByte(static_cast<unsigned char>(c))) return false;
    }
    const std::string key = lower(name);
    // Units are reserved too. A parameter called `in` or `m` could never be read back: "2 in"
    // is two inches everywhere else in the application, and a name that only works in some
    // expressions is worse than a name that is refused.
    return constantNames().count(key) == 0 && functionNames().count(key) == 0 && !isUnitWord(key);
}

}  // namespace cad::expr
