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

#include <cstdint>
#include <string>
#include <vector>

namespace homeworldz::script {

// Bumped when the emitted bytecode or snapshot layout changes. A destination
// region refuses to restore state produced by an incompatible ABI. v2 added
// globals and the event-dispatch model.
inline constexpr std::uint32_t kBytecodeAbiVersion = 2;
inline constexpr std::uint32_t kCompilerVersion = 2;

// Compile-time value types. Void exists only for typing host calls used as
// statements; a runtime Value is always Integer or String.
enum class Type : std::uint8_t { Void, Integer, String };

// A tagged LSL value. LSL integers are 32-bit; strings are UTF-8 bytes.
struct Value {
    Type type = Type::Integer;
    std::int32_t integer = 0;
    std::string str;

    static Value make_integer(std::int32_t v) { return Value{Type::Integer, v, {}}; }
    static Value make_string(std::string v) { return Value{Type::String, 0, std::move(v)}; }
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

// A compiled script: the shared instruction stream, the constant pool, the host
// functions it calls (resolved at the boundary, not baked in), the persistent
// globals (with their initial values), and the event dispatch table. Cached by
// source hash plus compiler and ABI version. This PoC compiles one default
// state; states are future work.
struct Program {
    std::vector<Instruction> code;
    std::vector<Value> constants;
    std::vector<std::string> host_names;
    std::vector<Value> global_defaults; // initial value per global slot
    std::vector<EventEntry> events;
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
