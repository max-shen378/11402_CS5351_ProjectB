#pragma once
//
// value.hpp -- the dynamically typed cell value used by tables and queries.
//
// A Value is one of: null, bool, int (64-bit), float (double) or string.
// The variant alternative order deliberately matches the Type enum so that
// `type()` is just the variant index.

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace minidb {

enum class Type { Null = 0, Bool, Int, Float, String };

inline const char* typeName(Type t) {
    switch (t) {
    case Type::Null:   return "null";
    case Type::Bool:   return "bool";
    case Type::Int:    return "int";
    case Type::Float:  return "float";
    case Type::String: return "string";
    }
    return "?";
}

struct Value {
    std::variant<std::monostate, bool, long long, double, std::string> v;

    Value() = default;
    static Value ofBool(bool b)         { Value x; x.v = b; return x; }
    static Value ofInt(long long i)     { Value x; x.v = i; return x; }
    static Value ofFloat(double d)      { Value x; x.v = d; return x; }
    static Value ofString(std::string s){ Value x; x.v = std::move(s); return x; }

    Type type() const { return static_cast<Type>(v.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isNumeric() const { return type() == Type::Int || type() == Type::Float; }

    bool getBool() const { return std::get<bool>(v); }
    long long getInt() const { return std::get<long long>(v); }
    double getFloat() const { return std::get<double>(v); }
    const std::string& getString() const { return std::get<std::string>(v); }

    double asDouble() const {
        if (type() == Type::Int) return static_cast<double>(getInt());
        if (type() == Type::Float) return getFloat();
        throw std::runtime_error(std::string("value of type ") + typeName(type()) +
                                 " is not numeric");
    }

    // Canonical text form, also used as hash-index key. Null renders empty.
    std::string toString() const {
        switch (type()) {
        case Type::Null: return "";
        case Type::Bool: return getBool() ? "true" : "false";
        case Type::Int:  return std::to_string(getInt());
        case Type::Float: {
            double d = getFloat();
            if (d == 0.0) d = 0.0;  // canonicalize -0.0 so index keys match
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.10g", d);
            return buf;
        }
        case Type::String: return getString();
        }
        return "";
    }
};

// Three-way comparison following SQL-ish semantics: returns nullopt when
// either side is null or when the types are not comparable (string vs number).
// Int/Float compare numerically.
inline std::optional<int> compareValues(const Value& a, const Value& b) {
    if (a.isNull() || b.isNull()) return std::nullopt;
    if (a.isNumeric() && b.isNumeric()) {
        if (a.type() == Type::Int && b.type() == Type::Int) {
            long long x = a.getInt(), y = b.getInt();
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        double x = a.asDouble(), y = b.asDouble();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.type() != b.type()) return std::nullopt;
    if (a.type() == Type::String) {
        int c = a.getString().compare(b.getString());
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (a.type() == Type::Bool) {
        int x = a.getBool(), y = b.getBool();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    return std::nullopt;
}

// Total order used by ORDER BY and MIN/MAX: nulls sort first; values of
// incomparable types fall back to ordering by type id so sorting never fails.
inline int orderCompare(const Value& a, const Value& b) {
    if (a.isNull() && b.isNull()) return 0;
    if (a.isNull()) return -1;
    if (b.isNull()) return 1;
    if (auto c = compareValues(a, b)) return *c;
    int x = static_cast<int>(a.type()), y = static_cast<int>(b.type());
    return x < y ? -1 : (x > y ? 1 : 0);
}

}  // namespace minidb
