#pragma once

// Recursive-descent statement parser with a Pratt (precedence-climbing)
// expression parser, per ADR 0021. Parses `default { state_entry() { ... } }`
// for the PoC subset and returns the state_entry body as an AST.

#include "homeworldz/script/ast.h"
#include "homeworldz/script/lexer.h"

namespace homeworldz::script {

// Parses the source into an AST. Throws ScriptError with a message and line on
// the first syntax error.
ast::Script parse(const std::string& source);

} // namespace homeworldz::script
