#include "homeworldz/script/library.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace homeworldz::script {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string fixed(double value, int decimals) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    char buffer[64]; // the largest float has 39 digits before the point
    const auto end =
        std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed, decimals);
    return std::string(buffer, end.ptr);
}

// ---------------------------------------------------------------------------
// Lists. The functions follow Second Life's rules for indexes: a negative
// index counts from the end, and a range whose start is past its end means
// everything except what lies strictly between the two.

using List = std::vector<Value>;

std::int64_t from_end(std::int32_t index, std::size_t size) {
    return index < 0 ? static_cast<std::int64_t>(index) + static_cast<std::int64_t>(size) : index;
}

// Elements first..last of a list, inclusive, both already inside it.
void append_range(List& out, const List& from, std::int64_t first, std::int64_t last) {
    if (first > last) return;
    out.insert(out.end(), from.begin() + first, from.begin() + last + 1);
}

// The element at an index, or null when the index is outside the list.
const Value* element(const std::vector<Value>& args) {
    const List& list = args[0].items();
    const std::int64_t index = from_end(args[1].integer, list.size());
    if (index < 0 || index >= static_cast<std::int64_t>(list.size())) return nullptr;
    return &list[static_cast<std::size_t>(index)];
}

// Whether two elements are the same value of the same type. An integer is
// not the float beside it, nor a string the key with its text.
bool same_element(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case Type::Integer: return a.integer == b.integer;
    case Type::Float: return a.real() == b.real();
    case Type::String:
    case Type::Key: return a.str == b.str;
    default: return a.components == b.components;
    }
}

Value llGetListLength(const std::vector<Value>& args) {
    return Value::make_integer(static_cast<std::int32_t>(args[0].items().size()));
}

Value llList2Integer(const std::vector<Value>& args) {
    const Value* found = element(args);
    if (found == nullptr) return Value::make_integer(0);
    switch (found->type) {
    case Type::Integer: return *found;
    case Type::Float: return Value::make_integer(float_to_integer(found->real()));
    case Type::String: return Value::make_integer(string_to_integer(found->str));
    default: return Value::make_integer(0);
    }
}

Value llList2Float(const std::vector<Value>& args) {
    const Value* found = element(args);
    if (found == nullptr) return Value::make_float(0.0);
    switch (found->type) {
    case Type::Float: return *found;
    case Type::Integer: return Value::make_float(static_cast<double>(found->integer));
    case Type::String: return Value::make_float(string_to_float(found->str));
    default: return Value::make_float(0.0);
    }
}

Value llList2String(const std::vector<Value>& args) {
    const Value* found = element(args);
    return Value::make_string(found == nullptr ? std::string() : element_string(*found));
}

Value llList2Key(const std::vector<Value>& args) {
    const Value* found = element(args);
    return Value::make_key(found != nullptr && found->is_text() ? found->str : std::string());
}

// TYPE_INTEGER to TYPE_ROTATION, and TYPE_INVALID outside the list.
Value llGetListEntryType(const std::vector<Value>& args) {
    const Value* found = element(args);
    if (found == nullptr) return Value::make_integer(0);
    switch (found->type) {
    case Type::Integer: return Value::make_integer(1);
    case Type::Float: return Value::make_integer(2);
    case Type::String: return Value::make_integer(3);
    case Type::Key: return Value::make_integer(4);
    case Type::Vector: return Value::make_integer(5);
    case Type::Rotation: return Value::make_integer(6);
    default: return Value::make_integer(0);
    }
}

Value llList2List(const std::vector<Value>& args) {
    const List& list = args[0].items();
    const auto size = static_cast<std::int64_t>(list.size());
    std::int64_t first = from_end(args[1].integer, list.size());
    std::int64_t last = from_end(args[2].integer, list.size());
    List out;
    if (first <= last) {
        if (last < 0 || first >= size) return Value::make_list({});
        append_range(out, list, (std::max<std::int64_t>)(first, 0), (std::min)(last, size - 1));
    } else if (first < 0 || last >= size) {
        return args[0];
    } else {
        append_range(out, list, 0, last);
        append_range(out, list, first, size - 1);
    }
    return Value::make_list(std::move(out));
}

Value llDeleteSubList(const std::vector<Value>& args) {
    const List& list = args[0].items();
    const auto size = static_cast<std::int64_t>(list.size());
    const std::int64_t first = from_end(args[1].integer, list.size());
    const std::int64_t last = from_end(args[2].integer, list.size());
    List out;
    if (first <= last) {
        if (last < 0 || first >= size) return args[0];
        append_range(out, list, 0, (std::max<std::int64_t>)(first, 0) - 1);
        append_range(out, list, (std::min)(last, size - 1) + 1, size - 1);
    } else if (first < 0 || last >= size) {
        return Value::make_list({});
    } else {
        // Only what lies strictly between the two is kept.
        append_range(out, list, (std::max<std::int64_t>)(last + 1, 0), (std::min)(first - 1, size - 1));
    }
    return Value::make_list(std::move(out));
}

Value llListInsertList(const std::vector<Value>& args) {
    const List& list = args[0].items();
    const List& added = args[1].items();
    const auto size = static_cast<std::int64_t>(list.size());
    const std::int64_t at =
        std::clamp<std::int64_t>(from_end(args[2].integer, list.size()), 0, size);
    List out;
    append_range(out, list, 0, at - 1);
    out.insert(out.end(), added.begin(), added.end());
    append_range(out, list, at, size - 1);
    return Value::make_list(std::move(out));
}

Value llListReplaceList(const std::vector<Value>& args) {
    const List& list = args[0].items();
    const List& added = args[1].items();
    const auto size = static_cast<std::int64_t>(list.size());
    const std::int64_t first = from_end(args[2].integer, list.size());
    const std::int64_t last = from_end(args[3].integer, list.size());
    List out;
    if (first <= last) {
        append_range(out, list, 0, std::clamp<std::int64_t>(first, 0, size) - 1);
        out.insert(out.end(), added.begin(), added.end());
        append_range(out, list, std::clamp<std::int64_t>(last + 1, 0, size), size - 1);
    } else {
        // The two ends are removed and the new elements follow what is left.
        append_range(out, list, (std::max<std::int64_t>)(last + 1, 0), (std::min)(first - 1, size - 1));
        out.insert(out.end(), added.begin(), added.end());
    }
    return Value::make_list(std::move(out));
}

// Every stride-th element of the range, counted from the start of the list.
Value llList2ListStrided(const std::vector<Value>& args) {
    const List& list = args[0].items();
    const auto size = static_cast<std::int64_t>(list.size());
    const std::int64_t first = from_end(args[1].integer, list.size());
    const std::int64_t last = from_end(args[2].integer, list.size());
    const std::int64_t stride = (std::max<std::int64_t>)(args[3].integer, 1);
    List out;
    const auto take = [&](std::int64_t from, std::int64_t to) {
        from = (std::max<std::int64_t>)(from, 0);
        to = (std::min)(to, size - 1);
        if (from % stride != 0) from += stride - from % stride;
        for (std::int64_t i = from; i <= to; i += stride) out.push_back(list[static_cast<std::size_t>(i)]);
    };
    if (first <= last) {
        take(first, last);
    } else {
        take(0, last);
        take(first, size - 1);
    }
    return Value::make_list(std::move(out));
}

// Where the second list first appears in the first, or -1. An empty list is
// found at 0, which is Second Life's documented behaviour.
Value llListFindList(const std::vector<Value>& args) {
    const List& list = args[0].items();
    const List& wanted = args[1].items();
    if (wanted.empty()) return Value::make_integer(0);
    for (std::size_t at = 0; at + wanted.size() <= list.size(); ++at) {
        bool match = true;
        for (std::size_t k = 0; match && k < wanted.size(); ++k) {
            match = same_element(list[at + k], wanted[k]);
        }
        if (match) return Value::make_integer(static_cast<std::int32_t>(at));
    }
    return Value::make_integer(-1);
}

Value llDumpList2String(const std::vector<Value>& args) {
    std::string out;
    bool first = true;
    for (const Value& item : args[0].items()) {
        if (!first) out += args[1].str;
        first = false;
        out += element_string(item);
    }
    return Value::make_string(std::move(out));
}

// Add at the end: compiled bytecode holds these indexes.
constexpr Builtin builtins[] = {
    {"llGetListLength", llGetListLength},
    {"llList2Integer", llList2Integer},
    {"llList2Float", llList2Float},
    {"llList2String", llList2String},
    {"llList2Key", llList2Key},
    {"llGetListEntryType", llGetListEntryType},
    {"llList2List", llList2List},
    {"llDeleteSubList", llDeleteSubList},
    {"llListInsertList", llListInsertList},
    {"llListReplaceList", llListReplaceList},
    {"llList2ListStrided", llList2ListStrided},
    {"llListFindList", llListFindList},
    {"llDumpList2String", llDumpList2String},
};

} // namespace

int find_builtin(std::string_view name) {
    for (std::size_t i = 0; i < std::size(builtins); ++i) {
        if (name == builtins[i].name) return static_cast<int>(i);
    }
    return -1;
}

const Builtin* builtin_at(std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= std::size(builtins)) return nullptr;
    return &builtins[index];
}

std::string float_to_string(float value) { return fixed(static_cast<double>(value), 6); }

std::int32_t float_to_integer(float value) {
    if (!(value >= -2147483648.0f && value < 2147483648.0f)) return INT32_MIN; // NaN too
    return static_cast<std::int32_t>(value);
}

std::int32_t string_to_integer(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size() && is_space(text[i])) ++i;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
    std::uint64_t value = 0;
    const auto add = [&value](std::uint64_t base, std::uint64_t digit) {
        value = value * base + digit;
        if (value > 0xffffffffULL) value = 0xffffffffULL;
    };
    if (i + 1 < text.size() && text[i] == '0' && lower(text[i + 1]) == 'x') {
        for (i += 2; i < text.size(); ++i) {
            const char c = lower(text[i]);
            if (is_digit(c)) add(16, static_cast<std::uint64_t>(c - '0'));
            else if (c >= 'a' && c <= 'f') add(16, static_cast<std::uint64_t>(c - 'a' + 10));
            else break;
        }
    } else {
        for (; i < text.size() && is_digit(text[i]); ++i) add(10, static_cast<std::uint64_t>(text[i] - '0'));
    }
    const auto bits = static_cast<std::uint32_t>(value);
    return static_cast<std::int32_t>(negative ? 0u - bits : bits);
}

float string_to_float(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size() && is_space(text[i])) ++i;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
    const auto starts = [&text, i](const char* word) {
        for (std::size_t k = 0; word[k] != '\0'; ++k) {
            if (i + k >= text.size() || lower(text[i + k]) != word[k]) return false;
        }
        return true;
    };
    float value = 0.0f;
    if (starts("inf")) {
        value = std::numeric_limits<float>::infinity();
    } else if (starts("nan")) {
        value = std::numeric_limits<float>::quiet_NaN();
    } else {
        std::size_t end = i;
        while (end < text.size() && is_digit(text[end])) ++end;
        if (end < text.size() && text[end] == '.') {
            ++end;
            while (end < text.size() && is_digit(text[end])) ++end;
        }
        if (end < text.size() && lower(text[end]) == 'e') {
            std::size_t exponent = end + 1;
            if (exponent < text.size() && (text[exponent] == '-' || text[exponent] == '+')) ++exponent;
            if (exponent < text.size() && is_digit(text[exponent])) {
                end = exponent;
                while (end < text.size() && is_digit(text[end])) ++end;
            }
        }
        double parsed = 0.0;
        // from_chars reads no further than a number goes, and reads nothing
        // from a lone "." — which leaves the 0.0 that is wanted.
        std::from_chars(text.data() + i, text.data() + end, parsed);
        value = static_cast<float>(parsed);
    }
    return negative ? -value : value;
}

std::string element_string(const Value& value) {
    switch (value.type) {
    case Type::Integer: return std::to_string(value.integer);
    case Type::Float: return float_to_string(value.real());
    case Type::Vector:
    case Type::Rotation: {
        std::string out = "<";
        const std::size_t count = value.type == Type::Vector ? 3 : 4;
        for (std::size_t i = 0; i < count; ++i) {
            if (i != 0) out += ", ";
            out += fixed(static_cast<double>(static_cast<float>(value.components[i])), 5);
        }
        return out + ">";
    }
    default: return value.str;
    }
}

} // namespace homeworldz::script
