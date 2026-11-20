#pragma once

// Handwritten scanner for the LSL PoC subset. No parser-generator dependency,
// per ADR 0021. It recognizes the identifiers, keywords, integer and string
// and float literals, punctuation, and operators the recursive-descent parser
// needs, and skips line and block comments.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace homeworldz::script {

// Raised for malformed source (unterminated string, stray character, etc.).
struct ScriptError : std::runtime_error {
    // An error with no place in the source, or whose place is not known yet.
    explicit ScriptError(const std::string& what) : std::runtime_error(what), message(what) {}
    // An error at a place in the source. Lines and columns count from 1.
    ScriptError(const std::string& what, int at_line, int at_column)
        : std::runtime_error(what + " (line " + std::to_string(at_line) + ", column " +
                             std::to_string(at_column) + ")"),
          message(what), line(at_line), column(at_column) {}

    std::string message; // what() without the position
    int line = 0;        // 0 when the position is not known
    int column = 0;
};

enum class TokenKind : std::uint8_t {
    End,
    Identifier,
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    KwDefault,
    KwInteger,
    KwFloat,
    KwString,
    KwKey,
    KwVector,
    KwRotation,
    KwList,
    KwWhile,
    KwIf,
    KwElse,
    KwPrint, // reserved in LSL's grammar, so it cannot name a variable
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Semicolon,
    Comma,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Less,
    Greater,
    LessEqual,    // <=
    GreaterEqual, // >=
    Assign,     // =
    EqualEqual, // ==
    BangEqual,  // !=
    Bang,       // !
    Amp,        // &
    AmpAmp,     // &&
    Pipe,       // |
    PipePipe,   // ||
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;         // identifier or string-literal contents
    std::int32_t integer = 0; // value for IntLiteral
    double number = 0.0;      // value for FloatLiteral
    int line = 1;
    int column = 1;
};

// Tokenizes the whole source. Throws ScriptError on the first lexical error.
std::vector<Token> tokenize(const std::string& source);

} // namespace homeworldz::script
