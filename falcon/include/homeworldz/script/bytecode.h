#pragma once

// Homeworldz "Falcon" script bytecode: the immutable, versioned instruction
// format the handwritten LSL compiler emits and the explicit-stack VM consumes.
// Per ADR 0021 the authoritative scene never depends on this representation
// directly; it is an opaque module behind the script-engine boundary.
//
// This proof-of-concept covers a representative slice: persistent globals,
// multiple event handlers with typed parameters (state_entry, touch_start),
// integers and strings, arithmetic, string concatenation, comparisons, a cast,
// control flow, and bounded host calls (llOwnerSay, llSay) -- enough to confirm
// the compile -> dispatch -> run -> snapshot/restore pipeline end to end,
// including state surviving across events and across a crossing.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace homeworldz::script {

// Bumped when the emitted bytecode or snapshot layout changes. A destination
// region refuses to restore state produced by an incompatible ABI. v2 added
// globals and the event-dispatch model.
// v3 added the logical, bitwise and string-comparison operators, keys, and
// vector and rotation values in snapshots.
// v5 added float arithmetic, comparison and casts, and integer division.
// v6 added lists, keys as a runtime type, and the built-in library.
// v7 added functions and call frames, and evaluates the right operand first.
// v8 added Dup, for assignment as an expression.
inline constexpr std::uint32_t kBytecodeAbiVersion = 8;
inline constexpr std::uint32_t kCompilerVersion = 9;

// The value types. Void exists only for typing calls used as statements. A key
// holds the same text a string does and keeps its own type at runtime, because
// a list remembers which of the two each element is (llGetListEntryType).
enum class Type : std::uint8_t { Void, Integer, String, Vector, Rotation, Key, Float, List };

// A tagged LSL value. LSL integers are 32-bit; strings are UTF-8 bytes.
//
// Vector and Rotation are literal-only for now: they can be written and handed
// to a host call, and that is all. There is no arithmetic on them yet, so
// their `components` are never the result of a computation. Full vector and
// rotation types are part of a roadmap item, and this will grow into them
// rather than be replaced.
struct Value {
    Type type = Type::Integer;
    std::int32_t integer = 0;
    std::string str;
    std::array<double, 4> components{};
    // A list's elements. Shared and never changed once made, so copying a list
    // value costs nothing and every operation on one makes a new list, which
    // is LSL's own rule. Null is the empty list.
    std::shared_ptr<const std::vector<Value>> list;

    static Value make_integer(std::int32_t v) { return Value{Type::Integer, v, {}, {}, {}}; }
    static Value make_string(std::string v) {
        return Value{Type::String, 0, std::move(v), {}, {}};
    }
    static Value make_key(std::string v) { return Value{Type::Key, 0, std::move(v), {}, {}}; }
    static Value make_list(std::vector<Value> v) {
        Value value{Type::List, 0, {}, {}, {}};
        if (!v.empty()) value.list = std::make_shared<const std::vector<Value>>(std::move(v));
        return value;
    }
    // The elements of a list; none for any other value.
    const std::vector<Value>& items() const {
        static const std::vector<Value> none;
        return list ? *list : none;
    }
    bool is_text() const { return type == Type::String || type == Type::Key; }
    static Value make_vector(double x, double y, double z) {
        return Value{Type::Vector, 0, {}, {x, y, z, 0.0}, {}};
    }
    static Value make_rotation(double x, double y, double z, double w) {
        return Value{Type::Rotation, 0, {}, {x, y, z, w}, {}};
    }
    // A float is held in the first component, rounded to the 32 bits an LSL
    // float has. Every float a script holds is made here, so the rounding
    // happens at each result, as it does in Second Life.
    static Value make_float(double v) {
        return Value{Type::Float, 0, {}, {static_cast<double>(static_cast<float>(v)), 0.0, 0.0, 0.0}, {}};
    }
    double number() const { return components[0]; }
    float real() const { return static_cast<float>(components[0]); }
};

enum class Op : std::uint8_t {
    PushConst,     // a = constant index
    LoadLocal,     // a = local slot (per-event; includes event parameters)
    StoreLocal,    // a = local slot (pops the value)
    LoadGlobal,    // a = global slot (persists across events)
    StoreGlobal,   // a = global slot (pops the value)
    AddInt,        // pops b, a; pushes a + b
    SubInt,        // pops b, a; pushes a - b
    MulInt,        // pops b, a; pushes a * b
    ConcatStr,     // pops b, a (strings); pushes a ++ b
    LessInt,       // pops b, a; pushes (a < b) ? 1 : 0
    GreaterInt,    // pops b, a; pushes (a > b) ? 1 : 0
    EqualInt,      // pops b, a; pushes (a == b) ? 1 : 0
    CastIntToStr,  // pops an integer; pushes its decimal string
    Jump,          // a = target instruction index
    JumpIfZero,    // a = target; pops an integer, jumps when it is zero
    CallHost,      // a = host-function index, b = argument count
    Pop,           // discards the top of the operand stack
    Halt,          // ends execution of the current event handler
    NotEqualInt,   // pops b, a; pushes (a != b) ? 1 : 0
    EqualStr,      // pops b, a (strings or keys); pushes (a == b) ? 1 : 0
    NotEqualStr,   // pops b, a (strings or keys); pushes (a != b) ? 1 : 0
    BitAnd,        // pops b, a; pushes a & b
    BitOr,         // pops b, a; pushes a | b
    LogicalAnd,    // pops b, a; pushes (a && b) — LSL evaluates both sides
    LogicalOr,     // pops b, a; pushes (a || b)
    NegInt,        // pops a; pushes -a
    NotInt,        // pops a; pushes !a
    StrTruth,      // pops a string; pushes 1 when it is non-empty
    KeyTruth,      // pops a key; pushes 1 when it is a non-null uuid
    IntToFloat,    // pops an integer; pushes it as a float
    DivInt,        // pops b, a; pushes a / b, truncated; b == 0 is a Math Error
    ModInt,        // pops b, a; pushes a % b; b == 0 is a Math Error
    // The float operators take a float or an integer for either operand and
    // widen the integer, so `i + 1.5` needs no conversion beneath the stack top.
    AddFloat,       // pops b, a; pushes a + b
    SubFloat,       // pops b, a; pushes a - b
    MulFloat,       // pops b, a; pushes a * b
    DivFloat,       // pops b, a; pushes a / b; b == 0 is a Math Error
    NegFloat,       // pops a; pushes -a
    LessFloat,      // pops b, a; pushes (a < b) ? 1 : 0
    GreaterFloat,   // pops b, a; pushes (a > b) ? 1 : 0
    LessEqFloat,    // pops b, a; pushes (a <= b) ? 1 : 0
    GreaterEqFloat, // pops b, a; pushes (a >= b) ? 1 : 0
    EqualFloat,     // pops b, a; pushes (a == b) ? 1 : 0
    CastFloatToStr, // pops a float; pushes it with six decimals
    CastFloatToInt, // pops a float; pushes it truncated, or INT_MIN out of range
    CastStrToInt,   // pops a string; pushes the integer it starts with, or 0
    CastStrToFloat, // pops a string; pushes the float it starts with, or 0.0
    ToKey,          // pops a string or key; pushes the same text as a key
    ToStr,          // pops a string or key; pushes the same text as a string
    MakeList,       // a = count; pops that many values; pushes them as a list
    ConcatList,     // pops b, a, one of them a list; pushes a ++ b
    EqualList,      // pops b, a (lists); pushes 1 when their lengths are equal
    NotEqualList,   // pops b, a (lists); pushes length(a) - length(b)
    CastListToStr,  // pops a list; pushes each element's string, joined
    CallBuiltin,    // a = built-in function index, b = argument count
    Call,           // a = function index; pops its arguments into a new frame
    Return,         // leaves the function, its value (if any) on the stack; ends an event
    Swap,           // exchanges the top two values of the stack
    CastVecToStr,   // pops a vector or rotation; pushes it with five decimals
    Dup,            // pushes a copy of the top of the stack
};

struct Instruction {
    Op op = Op::Halt;
    std::int32_t a = 0;
    std::int32_t b = 0;
};

// One compiled event handler within the compiled unit: where its code begins,
// the types of the parameters the region must supply when dispatching it, and
// how many local slots the VM must allocate for it (parameters occupy the first
// slots). All handlers share the single instruction stream.
struct EventEntry {
    std::string name;               // e.g. "state_entry", "touch_start"
    std::int32_t entry_ip = 0;      // first instruction of this handler
    std::vector<Type> param_types;  // seeded into locals 0..N-1 on dispatch
    std::int32_t local_count = 0;   // total local slots this handler uses
};

// One compiled function: where its code begins, how many arguments a call
// passes (they become its first locals), and how many local slots it uses.
struct FunctionEntry {
    std::string name;
    std::int32_t entry_ip = 0;
    std::int32_t param_count = 0;
    std::int32_t local_count = 0;
};

// A compiled script: the shared instruction stream, the constant pool, the host
// functions it calls (resolved at the boundary, not baked in), the persistent
// globals (with their initial values), and the event dispatch table. Cached by
// source hash plus compiler and ABI version. This PoC compiles one default
// state; states are future work.
struct Program {
    std::vector<Instruction> code;
    std::vector<std::int32_t> lines; // the source line of each instruction, from 1
    std::vector<Value> constants;
    std::vector<std::string> host_names;
    std::vector<Value> global_defaults; // initial value per global slot
    std::vector<EventEntry> events;
    std::vector<FunctionEntry> functions;
    std::uint32_t compiler_version = kCompilerVersion;
    std::uint32_t abi_version = kBytecodeAbiVersion;

    const EventEntry* find_event(const std::string& name) const {
        for (const auto& event : events) {
            if (event.name == name) {
                return &event;
            }
        }
        return nullptr;
    }
};

} // namespace homeworldz::script
