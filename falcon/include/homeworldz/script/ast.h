#pragma once

// Abstract syntax tree for the Falcon LSL PoC subset, produced by the
// recursive-descent parser and lowered to bytecode by the compiler. Nodes are
// owned through unique_ptr; this is a small tree, so clarity is preferred over
// arena tricks.

#include <memory>
#include <string>
#include <vector>

#include "homeworldz/script/bytecode.h"

namespace homeworldz::script::ast {

struct Expr {
    virtual ~Expr() = default;
    // Where the expression starts in the source, from 1; an operator's own
    // position for a binary expression. Used to place compile errors.
    int line = 0;
    int column = 0;
    // How many expressions deep this one is, itself included. The parser
    // refuses a tree past kMaxDepth, because everything that walks one
    // recurses, and a script must not be able to exhaust the native stack.
    int depth = 1;
};
using ExprPtr = std::unique_ptr<Expr>;

// The deepest an expression may go, and the deepest statements may nest.
inline constexpr int kMaxDepth = 200;

struct IntLiteral : Expr {
    std::int32_t value = 0;
};

struct StringLiteral : Expr {
    std::string value;
};

// A float written out: 1.0, .5, 2., 1e3.
struct FloatLiteral : Expr {
    double value = 0.0;
};

// A vector or rotation written out in full: <0, 0, 0.5> or <0, 0, 0, 1>.
//
// Its components are literal numbers rather than expressions, which is the
// whole of the stopgap. Allowing an expression here would mean deciding what a
// float expression is, and that is the type-system work this deliberately
// defers. Three components make a vector, four a rotation.
struct VectorLiteral : Expr {
    std::vector<double> components;
};

// A list written out: [1, "two", <3, 3, 3>]. Each element is an expression of
// any type but list.
struct ListLiteral : Expr {
    std::vector<std::unique_ptr<Expr>> elements;
};

struct VarRef : Expr {
    std::string name;
};

enum class BinaryOp {
    Add, Sub, Mul, Div, Mod, Less, Greater, LessEqual, GreaterEqual,
    Equal, NotEqual, BitAnd, BitOr, And, Or,
};

enum class UnaryOp { Negate, Not };

struct Unary : Expr {
    UnaryOp op = UnaryOp::Negate;
    ExprPtr operand;
};

struct Binary : Expr {
    BinaryOp op = BinaryOp::Add;
    ExprPtr lhs;
    ExprPtr rhs;
};

// A C-style cast such as (string)i. Supported: (string), (integer), (float),
// (list), and (key) between strings and keys.
struct Cast : Expr {
    Type target = Type::String;
    ExprPtr operand;
};

// An assignment, which in LSL is an expression with the variable's new value:
// `x = v`, or `x += v` and its kin, for which `op` is the operator applied.
struct Assign : Expr {
    std::string name;
    bool compound = false;
    BinaryOp op = BinaryOp::Add;
    ExprPtr value;
};

// ++x, x++, --x, x--. A prefix form has the new value and a postfix form the
// old one.
struct IncDec : Expr {
    std::string name;
    bool increment = true;
    bool prefix = true;
};

struct Call : Expr {
    std::string callee;
    std::vector<ExprPtr> args;
};

struct Stmt {
    virtual ~Stmt() = default;
    int line = 0;
    int column = 0;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct Block : Stmt {
    std::vector<StmtPtr> statements;
};

struct VarDecl : Stmt {
    Type type = Type::Integer;
    std::string name;
    ExprPtr init; // may be null (default-initialized)
};

struct ExprStmt : Stmt {
    ExprPtr expr;
};

struct While : Stmt {
    ExprPtr condition;
    std::unique_ptr<Block> body;
};

// for (init; condition; step) body. `init` and `step` are lists of expressions
// run for their effects; either may be empty.
struct For : Stmt {
    std::vector<ExprPtr> init;
    ExprPtr condition;
    std::vector<ExprPtr> step;
    std::unique_ptr<Block> body;
};

// do body while (condition);
struct DoWhile : Stmt {
    std::unique_ptr<Block> body;
    ExprPtr condition;
};

struct If : Stmt {
    ExprPtr condition;
    std::unique_ptr<Block> then_branch;
    std::unique_ptr<Block> else_branch; // may be null
};

// `return;` or `return value;`. In an event handler only the first is allowed,
// and it ends the event.
struct Return : Stmt {
    ExprPtr value; // null for a bare return
};

// A named, typed parameter of an event handler (e.g. touch_start's detected
// count) or of a function. Parameters become the first locals.
struct Param {
    Type type = Type::Integer;
    std::string name;
};

// One event handler in the default state.
struct EventHandler {
    std::string name; // "state_entry", "touch_start", ...
    std::vector<Param> params;
    std::unique_ptr<Block> body;
    int line = 0;
    int column = 0;
};

// A function the script defines. `result` is Void for one that returns nothing.
struct Function {
    Type result = Type::Void;
    std::string name;
    std::vector<Param> params;
    std::unique_ptr<Block> body;
    int line = 0;
    int column = 0;
};

// A script-level global variable, persistent across events. Its initializer,
// if any, must fold to a constant at compile time — a literal, a named constant
// such as ZERO_ROTATION, or a negated integer — as LSL requires; absent, the
// global takes its type default.
struct GlobalVar {
    Type type = Type::Integer;
    std::string name;
    ExprPtr init; // may be null
    int line = 0;
    int column = 0;
};

// The compiled script: globals, functions, and the default state's event
// handlers. Named states are future work.
struct Script {
    std::vector<GlobalVar> globals;
    std::vector<Function> functions;
    std::vector<EventHandler> events;
};

} // namespace homeworldz::script::ast
