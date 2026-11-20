#pragma once

// Falcon's built-in library: the LSL functions that need nothing from a region
// — they read their arguments and compute an answer — and the conversions LSL
// defines between its types, which the VM's cast instructions share.
//
// A built-in is reached by its index in the table (Op::CallBuiltin), not by
// name, so calling one costs no lookup. Compiled bytecode holds those indexes,
// which makes the table's order part of the bytecode ABI: add at the end.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "homeworldz/script/bytecode.h"

namespace homeworldz::script {

struct Builtin {
    const char* name;
    Value (*call)(const std::vector<Value>& args);
};

// The index of the built-in with this LSL name, or -1.
int find_builtin(std::string_view name);
// The built-in at an index find_builtin gave, or null for any other number.
const Builtin* builtin_at(std::int32_t index);

// LSL's casts, as Second Life performs them.

// (string) of a float: six decimals; "Infinity", "-Infinity", "NaN".
std::string float_to_string(float value);
// (integer) of a float: toward zero, and INT_MIN for anything an integer
// cannot hold.
std::int32_t float_to_integer(float value);
// (integer) of a string: the decimal or 0x hexadecimal number it starts with
// after any spaces, 0 when there is none. A number past 32 bits is all ones.
std::int32_t string_to_integer(const std::string& text);
// (float) of a string: the number it starts with after any spaces, with an
// optional exponent; "inf" and "nan" in any case; 0.0 when there is none.
float string_to_float(const std::string& text);
// A value as it is written inside (string) of a list, which is also what
// llList2String and llDumpList2String give: an integer in decimal, a float
// with six decimals, a vector or rotation with five, text as it is.
std::string element_string(const Value& value);

} // namespace homeworldz::script
