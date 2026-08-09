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
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLiteral : Expr {
    std::int32_t value = 0;
};

struct StringLiteral : Expr {
    std::string value;
};

struct VarRef : Expr {
    std::string name;
};

enum class BinaryOp { Add, Sub, Mul, Less, Greater, Equal };

struct Binary : Expr {
    BinaryOp op = BinaryOp::Add;
    ExprPtr lhs;
    ExprPtr rhs;
};

// A C-style cast such as (string)i. The PoC supports (string) and (integer).
struct Cast : Expr {
    Type target = Type::String;
    ExprPtr operand;
};

struct Call : Expr {
    std::string callee;
    std::vector<ExprPtr> args;
};

struct Stmt {
    virtual ~Stmt() = default;
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

struct Assign : Stmt {
    std::string name;
    ExprPtr value;
};

struct ExprStmt : Stmt {
    ExprPtr expr;
};

struct While : Stmt {
    ExprPtr condition;
    std::unique_ptr<Block> body;
};

struct If : Stmt {
    ExprPtr condition;
    std::unique_ptr<Block> then_branch;
    std::unique_ptr<Block> else_branch; // may be null
};

// A named, typed parameter of an event handler (e.g. touch_start's detected
// count). Parameters become the first locals of the handler.
struct Param {
    Type type = Type::Integer;
    std::string name;
};

// One event handler in the default state.
struct EventHandler {
    std::string name; // "state_entry", "touch_start", ...
    std::vector<Param> params;
    std::unique_ptr<Block> body;
};

// A script-level global variable, persistent across events. The PoC allows a
// literal initializer only; absent, the global takes its type default.
struct GlobalVar {
    Type type = Type::Integer;
    std::string name;
    bool has_init = false;
    std::int32_t int_init = 0;
    std::string string_init;
};

// The compiled script: globals plus the default state's event handlers. Named
// states are future work.
struct Script {
    std::vector<GlobalVar> globals;
    std::vector<EventHandler> events;
};

} // namespace homeworldz::script::ast
