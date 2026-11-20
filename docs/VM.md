# Falcon VM

The **Falcon VM** (the **Falcon Engine**) is Homeworldz's script execution
engine: a small, single-threaded bytecode interpreter that runs compiled LSL on
the authoritative region thread. It is named for the peregrine falcon — the
fastest animal on the planet, exceeding 380 km/h (240 mph) in its hunting dive —
because the design goal is exactly that: extreme speed under complete control,
able to pull out of the dive on command. A Falcon script can be stopped after
*any single instruction* and resumed elsewhere, so it never loses control of a
region no matter how the script behaves.

This document describes the Falcon **p-code** (bytecode) format and the VM that
executes it. It is the implementation-level companion to the higher-level
[SCRIPTING.md](SCRIPTING.md) and the engine-boundary decision in
[ADR 0021](adr/0021-script-runtime-boundary.md).

> **Status: proof-of-concept.** Falcon currently implements a representative
> slice of LSL sufficient to confirm the approach (see
> [ROADMAP.md](ROADMAP.md), Phase 5). The format and opcode set below are real
> and versioned, but small; the [Scope](#current-scope) section lists what is
> not yet present. Everything here is versioned, so the surface can grow without
> invalidating the design.

## Where Falcon fits

Per ADR 0021, the authoritative scene and persistence formats never depend on a
particular VM. Falcon sits *behind* the C++ script-runtime boundary:

- The **C++ region** owns the scene, physics, inventory, event production, the
  script scheduler, resource accounting, crossings, and the LSL host functions.
  It hands Falcon a bounded instruction budget each tick and services host
  calls.
- The **LSL compiler** (handwritten lexer + recursive-descent/Pratt parser)
  turns source into Falcon p-code. Compiled p-code is an immutable derived
  asset, cached by source hash plus compiler and ABI version.
- **Falcon** owns only explicit execution state: instruction pointer, operand
  stack, locals, persistent globals, and (as the engine grows) call frames,
  event queue, timers, listens, permissions, and host-operation continuations.
- The **grid** locates source and bytecode assets but never executes scripts.
  Regions transfer runtime state directly during an avatar, attachment, vehicle,
  or object handoff.

A script cannot reach the network, filesystem, clocks, or randomness directly;
all outside effects go through the host boundary.

## Values and types

Falcon has one runtime value representation, a tagged value:

| Compile-time `Type` | Runtime | Notes |
| --- | --- | --- |
| `Integer` | 32-bit signed | LSL `integer` |
| `String`  | UTF-8 bytes    | LSL `string` |
| `Void`    | — | typing only; never a runtime value (e.g. a void host call) |

The compiler tracks static types so it can pick integer vs. string operators,
validate casts, and check host-call argument types. At runtime a `Value` is
always `Integer` or `String`.

## The compiled unit (p-code container)

A compiled script is a `Program`. Its event handlers share one instruction
stream; an event dispatch table records where each begins.

| Field | Meaning |
| --- | --- |
| `code` | the shared instruction stream (`vector<Instruction>`) |
| `constants` | the constant pool (`vector<Value>`), referenced by index |
| `host_names` | names of host functions this program calls, referenced by index (resolved at the boundary, not baked in) |
| `global_defaults` | initial value of each persistent global slot (`vector<Value>`) |
| `events` | the dispatch table (`vector<EventEntry>`) |
| `compiler_version` | the compiler that produced it |
| `abi_version` | the bytecode/snapshot ABI (currently `7`) |

Each `EventEntry` carries the handler's `name` (e.g. `state_entry`,
`touch_start`), its `entry_ip` (first instruction), the `param_types` the region
must supply when dispatching it, and the `local_count` it needs (parameters
occupy the first local slots). `compiler_version` is `8` and `abi_version` `7`
(`kCompilerVersion`, `kBytecodeAbiVersion`); ABI 2 added persistent globals and
the event-dispatch model, 3 vectors and rotations as values, 4 floats, the
sleep left, and the host call in progress, 5 float arithmetic and integer
division, 6 lists, keys as a runtime type, and the built-in library, and 7
functions and call frames. `functions` holds one `FunctionEntry` per function
the script defines: its `entry_ip`, `param_count` and `local_count`. `lines` holds the source line of each instruction, which is how a
runtime error names the line it was raised on (`VM::error_line()`).

## Instruction format

Every instruction is a fixed-width triple:

```
struct Instruction { Op op; int32 a; int32 b; };
```

`a` and `b` are operands whose meaning depends on the opcode (a constant index,
a local slot, a jump target, or a host index + argument count). Fixed-width
instructions keep the instruction pointer a simple index, which is what makes
snapshotting after any instruction trivial.

## Opcode reference

The operand stack is written bottom → top. "pop b, a" means `b` is the topmost
operand and `a` is beneath it (so `a OP b` matches source order).

| Opcode | Operands | Stack effect | Description |
| --- | --- | --- | --- |
| `PushConst` | `a`=const idx | → `constants[a]` | push a literal |
| `LoadLocal` | `a`=slot | → `locals[a]` | push a local (per-event; includes params) |
| `StoreLocal` | `a`=slot | `v` → | pop into a local |
| `LoadGlobal` | `a`=slot | → `globals[a]` | push a persistent global |
| `StoreGlobal` | `a`=slot | `v` → | pop into a persistent global |
| `AddInt` | — | `a b` → `a+b` | integer add |
| `SubInt` | — | `a b` → `a-b` | integer subtract |
| `MulInt` | — | `a b` → `a*b` | integer multiply |
| `ConcatStr` | — | `a b` → `a++b` | string concatenation |
| `LessInt` | — | `a b` → `a<b` | integer `<`, result 0/1 |
| `GreaterInt` | — | `a b` → `a>b` | integer `>`, result 0/1 |
| `EqualInt` | — | `a b` → `a==b` | integer `==`, result 0/1 |
| `CastIntToStr` | — | `i` → `str(i)` | `(string)` of an integer |
| `Jump` | `a`=target | — | unconditional jump: `ip = a` |
| `JumpIfZero` | `a`=target | `i` → | pop; if `i == 0`, `ip = a` |
| `CallHost` | `a`=host idx, `b`=argc | `arg1..argN` → `[result]` | call `host_names[a]`; push a result only if the host returns one |
| `Pop` | — | `v` → | discard the top of stack |
| `Halt` | — | — | end the handler (VM reports `Finished`) |
| `DivInt`, `ModInt` | — | `a b` → `a/b`, `a%b` | integer divide and remainder, toward zero; a zero divisor is a `Math Error` |
| `AddFloat`, `SubFloat`, `MulFloat`, `DivFloat` | — | `a b` → result | float arithmetic in 32 bits; either operand may be an integer, which widens; dividing by zero is a `Math Error` |
| `NegFloat` | — | `a` → `-a` | float negation |
| `LessFloat`, `GreaterFloat`, `LessEqFloat`, `GreaterEqFloat`, `EqualFloat` | — | `a b` → 0/1 | float comparison, with the same widening; every one is 0 against NaN |
| `IntToFloat` | — | `i` → `f` | widen an integer |
| `CastFloatToStr` | — | `f` → `str(f)` | `(string)` of a float: six decimals, `Infinity`, `NaN` |
| `CastFloatToInt` | — | `f` → `i` | `(integer)` of a float: toward zero, `INT_MIN` out of range |
| `CastStrToInt`, `CastStrToFloat` | — | `s` → number | the number a string starts with, or zero |
| `ToKey`, `ToStr` | — | `t` → `t` | the same text as a key, or as a string |
| `MakeList` | `a`=count | `v1..vN` → `[v1..vN]` | build a list; a list cannot hold a list |
| `ConcatList` | — | `a b` → `a++b` | join two lists, or add one value to either end of a list |
| `EqualList`, `NotEqualList` | — | `a b` → `i` | whether the lengths are equal; the difference of the lengths |
| `CastListToStr` | — | `l` → `s` | each element's string, joined |
| `CallBuiltin` | `a`=built-in idx, `b`=argc | `arg1..argN` → result | call a function of Falcon's own library by its index |
| `Call` | `a`=function idx | `arg1..argN` → | open a frame for `functions[a]`; the arguments become its first locals |
| `Return` | — | — | close the frame and go on after the call, the result (if any) left on the stack; with no frame open, end the event |
| `Swap` | — | `a b` → `b a` | exchange the top two values |
| `CastVecToStr` | — | `v` → `str(v)` | `(string)` of a vector or rotation: five decimals |

Integer `<=` and `>=` compile to the opposite test and `NotInt`; float `!=` to
`EqualFloat` and `NotInt`. The table above leaves out the string, key, bitwise
and logical operators, which are listed in `bytecode.h`.

LSL evaluates the right operand of a binary operator before the left, and a
script can tell once an operand calls a function. The compiler therefore emits
the right operand first. Where the order of the operands matters to the
operator it emits `Swap` before it, and for an ordering it emits the mirror
image instead (`a < b` as `b > a`). Arguments and list elements run left to
right.

A call keeps no state on the native stack. `Call` appends the function's local
slots to the one `locals` vector and records a frame (where to return, and
where the caller's locals begin); `Return` takes them off again. Calls nest to
1024 frames, past which the script stops with `Stack-Heap Collision`, the
error Second Life gives for recursion without end. The parser refuses source
nested or chained more than 200 deep (`ast::kMaxDepth`), because the parser
and compiler do recurse, and a script must not be able to exhaust their stack.

A list value shares its elements and never changes them: copying one is free,
and every operation on a list makes a new list. A key holds the text a string
does under its own type tag, because a list remembers which each element is.

The built-in library (`library.cpp`) holds the functions that need nothing
from a region. The compiler resolves a call to one into its index in the
library's table, so the call costs no lookup by name; the table's order is
therefore part of the bytecode ABI, and new functions are added at its end.

Control flow is lowered by the compiler with forward-patched jump targets. For
example a `while` loop becomes: `<cond>`, `JumpIfZero end`, `<body>`,
`Jump start`, `end:`.

## Execution model

A VM instance holds one script's live state, entirely as data — no script state
ever lives on the native C++ call stack:

- `ip` — instruction pointer (an index into `code`)
- `stack` — the operand stack (`vector<Value>`)
- `locals` — the current event's local slots (`vector<Value>`; parameters
  first), reset on each dispatch
- `globals` — persistent globals (`vector<Value>`), seeded from
  `global_defaults` and preserved **across** events
- `finished`, `error`, and a `total` executed-instruction counter

`step()` executes exactly one instruction. `run(budget)` calls `step()` until
either the handler halts or `budget` instructions have been executed:

```
RunStatus run(uint64 budget):
    while not finished and executed < budget:
        step()
    return finished ? Finished : Yielded   // or Error on a fault
```

`RunStatus` is one of:

- **`Finished`** — reached `Halt`.
- **`Yielded`** — spent its instruction budget with work remaining; call `run`
  again next tick to continue.
- **`Error`** — a runtime fault stopped the script.
- **`Sleeping`** — a host call put the script to sleep. The VM has no clock; it
  holds the seconds left, and whoever drives it counts them and calls `wake`.
- **`Waiting`** — a host call is still in progress. `resume(token, value)`
  delivers its answer and the script goes on.

### Events and dispatch

A script does not "start" on its own. The region **dispatches** an event —
`state_entry` when the script is first loaded, `touch_start` when an avatar
clicks the prim, and so on. `dispatch(name, args)`:

1. looks the handler up in the event table (unknown event → rejected);
2. checks the argument count and types against the handler's `param_types`;
3. sizes `locals` for that handler and seeds the parameters into the first
   slots (e.g. `touch_start`'s detected count);
4. clears the operand stack and points `ip` at the handler's `entry_ip`.

Globals are untouched by dispatch, so state accumulates across events — the
canonical example being a touch counter that increments a global on every
`touch_start` and reports it with `llSay`. Each handler ends with its own
`Halt`, so `run()` stops at the end of the dispatched event. A full event queue,
coalescing, and SL event-ordering/delay semantics (SCRIPTING.md) build on this
dispatch primitive.

### Faults

Runtime faults (instruction pointer out of range, operand-stack underflow, a
type mismatch on the stack, a bad constant/local/host index, host-argument
underflow, or a missing host boundary) are raised internally and caught by
`run()`, which records the message, marks the script finished, and returns
`Error`. This uses ordinary C++ error handling to *stop* a script; it is not
script state and never crosses the boundary. A faulting script stops without
destabilizing the region.

### Cooperative scheduling and resource control

Cooperation is enforced by the VM, not trusted to script authors. Because the
scheduler may yield after any instruction, an infinite LSL loop simply consumes
its instruction budget and resumes at the back of the queue on a later tick — it
cannot hang the region thread. The PoC implements the per-slice **instruction
budget**; the remaining resource controls in SCRIPTING.md (per-script and
aggregate owner/object/parcel budgets, a 64 KiB logical memory limit, bounded
call depth / stack / lists / strings / event queue, moving-average throttling,
and a wall-clock guard) attach at this same yield boundary as Falcon grows.

### Host-call boundary

Host functions are provided to the VM as a single callback:

```
HostResult host(const string& name, const vector<Value>& args)
```

A `HostResult` is one of four outcomes:

| Outcome | Meaning |
| --- | --- |
| returned | the call is done, with its value or with none |
| sleep | the call is done and the script sleeps before it goes on: `llSleep`, and Second Life's forced delays |
| suspended | the call is in progress; the host gave a numeric token and answers later with `resume` |
| failed | the call raises a runtime error that stops the script |

An optional value converts to "returned", so a function that answers at once
never names the type. The token is a number so that a script suspended in a host
call can be snapshotted, carried to another region, and resumed there.

`CallHost` pops `argc` arguments (restoring source order), invokes the boundary
with the resolved name, and pushes a result only if one is returned (void host
functions return none). The host performs bounded region work — or begins a
non-blocking region operation and, later, delivers completion as an LSL event —
but never blocks the VM or touches ambient OS capabilities. The PoC ships
`llOwnerSay` and `llSay`.

## Snapshot format (the crossing snapshot)

The defining Falcon capability: a script can be suspended after **any completed
instruction**, serialized to a compact versioned binary blob, and restored into
a fresh VM to continue identically. This is what lets a heavily scripted
attachment or vehicle cross a region border without waiting for a poorly written
handler to reach an event boundary.

All multi-byte integers are little-endian. Current layout (`abi_version` 7,
snapshot version 6):

| Bytes | Field | Notes |
| --- | --- | --- |
| 4 | magic | `HWZS` |
| 2 | snapshot version | `6` |
| 4 | bytecode ABI version | must match the `Program` on restore |
| 8 | `ip` | instruction pointer |
| 1 | finished | `0` or `1` |
| 8 | `total` | instructions executed so far |
| 4 | stack length `N` | operand-stack depth |
| … | `N` values | bottom → top |
| 4 | locals length `M` | the local slots of the handler and of every function being run |
| … | `M` values | slot order |
| 4 | globals length `G` | must equal the program's global count on restore |
| … | `G` values | slot order |
| 8 | sleep left | seconds as an IEEE double; `0` when awake |
| 1 | waiting | `1` while a host call is in progress |
| 8 | waiting token | the host's name for that call |
| 8 | locals base | where the running function's locals begin |
| 4 | frame count `F` | calls in progress |
| 16 × `F` | frames | outermost first: the `ip` to return to, and the caller's locals base |

Because the operand stack, locals, and **globals** are all captured, a crossing
snapshot taken mid-`touch_start` restores with its counter intact. Each
**value** is length-prefixed and tagged:

| Bytes | Field |
| --- | --- |
| 1 | tag: `1` = integer, `2` = string, `3` = vector, `4` = rotation, `5` = float, `6` = key, `7` = list |
| 4 | integer: the 32-bit value / string: the byte length `L` |
| `L` | string bytes (strings only) |
| 32 | vector or rotation: four IEEE doubles |
| 8 | float: one IEEE double |
| 4 + … | list: element count, then each element as a value |

The wire format is a purpose-built binary format with bounded, length-prefixed,
tagged fields — deliberately **not** JSON, XML, protobuf, or a reflection dump.
Bytecode itself is cached separately and transferred only when the destination
lacks the matching asset and ABI.

**Restore** validates the magic, the snapshot version, that the snapshot's ABI
matches the target `Program`'s ABI, and that the globals count matches — then
loads `ip`, `finished`, `total`, the operand stack, the locals, and the globals.
A mismatch is rejected rather than silently reinterpreted. (Locals vary per
handler, so their count is taken from the snapshot; globals are fixed per
program and must match.)

## Versioning and ABI

`abi_version` covers both the bytecode and the snapshot layout. A destination
region refuses to restore a snapshot whose ABI it does not implement. When the
opcode set, value representation, or snapshot layout changes in an incompatible
way, `kBytecodeAbiVersion` increments; the roadmap's "version the runtime ABI …
safe upgrade, incompatibility, and rollback" item builds on this field.

## Current scope

Falcon today confirms the pipeline end to end on a representative slice and is
deliberately narrow:

- **Language:** one `default` state; persistent globals; functions, with
  recursion; `integer`, `float`,
  `string`, `key` and `list`, with vector and rotation literals; arithmetic,
  string and list concatenation, comparisons, and casts between them;
  `while`, `if` and `return`.
- **Events:** `state_entry` and `touch_start(integer)`, dispatched with typed
  parameters. Adding more events is a table entry plus a parameter signature.
- **Host surface:** `llOwnerSay`, `llSay`.
- **Input:** LSL **source text**, compiled directly. It is not yet wired to
  inventory or a prim's Contents (creating a runnable script in a prim is not
  implemented yet — only a default-script *asset-text* generator exists), and
  `llSay` output goes to a test boundary rather than region chat.

Not yet implemented: `state`
transitions and the broader event set, `vector` / `rotation` arithmetic, the full LSL host-function
library, memory and other resource limits, and region/crossing integration.
Each is an additive step behind the same boundary and ABI.

## Implementation map

The engine name is **Falcon**; the current PoC code still uses neutral module
identifiers, to be aligned as it graduates:

| Concept | Current identifier |
| --- | --- |
| module | `falcon/` (built standalone or via the top-level CMake) |
| namespace | `homeworldz::script` |
| headers | `falcon/include/homeworldz/script/{bytecode,lexer,parser,ast,compiler,vm}.h` |
| library / test | `homeworldz-script` / `homeworldz-script-poc-tests` (CTest `script-poc`) |
| snapshot magic | `HWZS` |

## References

- [ADR 0021: Script Runtime Boundary](adr/0021-script-runtime-boundary.md)
- [ADR 0031: Sandboxed Lua Scripting and SLua Compatibility](adr/0031-lua-scripting-subset.md)
  — a planned second script runtime behind the same boundary
- [SCRIPTING.md](SCRIPTING.md) — scheduling, resource, compatibility, and
  crossing requirements
- [ROADMAP.md](ROADMAP.md) — Phase 5: LSL Scripting
