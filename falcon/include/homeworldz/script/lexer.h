#pragma once

// Handwritten scanner for the LSL PoC subset. No parser-generator dependency,
// per ADR 0021. It recognizes the identifiers, keywords, integer and string
// literals, punctuation, and operators the recursive-descent parser needs, and
// skips line and block comments.
//
// Float literals exist for one reason so far: a vector or rotation literal is
// written with them, and those are what a seat offset is made of. There are no
// float variables or float arithmetic yet, so a float outside such a literal
// has nowhere to go and the parser says so.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace homeworldz::script {

// Raised for malformed source (unterminated string, stray character, etc.).
struct ScriptError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class TokenKind : std::uint8_t {
    End,
    Identifier,
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    KwDefault,
    KwInteger,
    KwString,
    KwWhile,
    KwIf,
    KwElse,
    LParen,
    RParen,
    LBrace,
    RBrace,
    Semicolon,
    Comma,
    Plus,
    Minus,
    Star,
    Less,
    Greater,
    Assign,     // =
    EqualEqual, // ==
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;         // identifier or string-literal contents
    std::int32_t integer = 0; // value for IntLiteral
    double number = 0.0;      // value for FloatLiteral
    int line = 1;
};

// Tokenizes the whole source. Throws ScriptError on the first lexical error.
std::vector<Token> tokenize(const std::string& source);

} // namespace homeworldz::script
