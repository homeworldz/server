#pragma once

// Lowers the AST to Homeworldz bytecode with light semantic analysis: local
// slot allocation, static typing to select integer vs. string operators and
// casts, and host-call arity/type checking against a small built-in table.

#include "homeworldz/script/ast.h"
#include "homeworldz/script/bytecode.h"

namespace homeworldz::script {

// Compiles a parsed script (globals + default-state event handlers) into an
// immutable Program with an event dispatch table. Throws ScriptError on a
// semantic error (unknown variable, type mismatch, bad host-call arity,
// unsupported event, etc.).
Program compile(const ast::Script& script);

// Convenience: source -> tokens -> AST -> bytecode.
Program compile_source(const std::string& source);

} // namespace homeworldz::script
