#include "homeworldz/script/lexer.h"

#include <cctype>
#include <unordered_map>
#include <utility>

namespace homeworldz::script {
namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

TokenKind keyword_kind(const std::string& word) {
    static const std::unordered_map<std::string, TokenKind> keywords = {
        {"default", TokenKind::KwDefault},
        {"integer", TokenKind::KwInteger},
        {"string", TokenKind::KwString},
        {"while", TokenKind::KwWhile},
        {"if", TokenKind::KwIf},
        {"else", TokenKind::KwElse},
    };
    const auto it = keywords.find(word);
    return it == keywords.end() ? TokenKind::Identifier : it->second;
}

std::string located(std::string message, int line, std::size_t column) {
    return std::move(message) + " (line " + std::to_string(line) +
           ", column " + std::to_string(column) + ")";
}

} // namespace

std::vector<Token> tokenize(const std::string& source) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    int line = 1;
    std::size_t line_start = 0;
    const std::size_t n = source.size();

    while (i < n) {
        const char c = source[i];

        if (c == '\n') {
            ++line;
            ++i;
            line_start = i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
        }
        // Comments: // to end of line, and /* ... */ blocks.
        if (c == '/' && i + 1 < n && source[i + 1] == '/') {
            i += 2;
            while (i < n && source[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && source[i + 1] == '*') {
            const auto comment_line = line;
            const auto comment_column = i - line_start + 1;
            i += 2;
            while (i + 1 < n && !(source[i] == '*' && source[i + 1] == '/')) {
                if (source[i] == '\n') {
                    ++line;
                    line_start = i + 1;
                }
                ++i;
            }
            if (i + 1 >= n) {
                throw ScriptError(located(
                    "unterminated block comment", comment_line, comment_column));
            }
            i += 2;
            continue;
        }

        Token token;
        token.line = line;

        if (is_ident_start(c)) {
            std::size_t start = i;
            while (i < n && is_ident_char(source[i])) {
                ++i;
            }
            token.text = source.substr(start, i - start);
            token.kind = keyword_kind(token.text);
            tokens.push_back(std::move(token));
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            const auto literal_column = i - line_start + 1;
            std::int64_t value = 0;
            while (i < n && std::isdigit(static_cast<unsigned char>(source[i])) != 0) {
                value = value * 10 + (source[i] - '0');
                if (value > 0x7fffffffLL) {
                    throw ScriptError(located(
                        "integer literal out of range", line, literal_column));
                }
                ++i;
            }
            token.kind = TokenKind::IntLiteral;
            token.integer = static_cast<std::int32_t>(value);
            tokens.push_back(std::move(token));
            continue;
        }
        if (c == '"') {
            const auto string_line = line;
            const auto string_column = i - line_start + 1;
            ++i;
            std::string text;
            while (i < n && source[i] != '"') {
                char ch = source[i];
                if (ch == '\\' && i + 1 < n) {
                    const char esc = source[i + 1];
                    switch (esc) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case '"': ch = '"'; break;
                    case '\\': ch = '\\'; break;
                    default: ch = esc; break;
                    }
                    i += 2;
                    text.push_back(ch);
                    continue;
                }
                if (ch == '\n') {
                    throw ScriptError(located(
                        "unterminated string literal", string_line, string_column));
                }
                text.push_back(ch);
                ++i;
            }
            if (i >= n) {
                throw ScriptError(located(
                    "unterminated string literal", string_line, string_column));
            }
            ++i; // closing quote
            token.kind = TokenKind::StringLiteral;
            token.text = std::move(text);
            tokens.push_back(std::move(token));
            continue;
        }

        // Punctuation and operators.
        switch (c) {
        case '(': token.kind = TokenKind::LParen; break;
        case ')': token.kind = TokenKind::RParen; break;
        case '{': token.kind = TokenKind::LBrace; break;
        case '}': token.kind = TokenKind::RBrace; break;
        case ';': token.kind = TokenKind::Semicolon; break;
        case ',': token.kind = TokenKind::Comma; break;
        case '+': token.kind = TokenKind::Plus; break;
        case '-': token.kind = TokenKind::Minus; break;
        case '*': token.kind = TokenKind::Star; break;
        case '<': token.kind = TokenKind::Less; break;
        case '>': token.kind = TokenKind::Greater; break;
        case '=':
            if (i + 1 < n && source[i + 1] == '=') {
                token.kind = TokenKind::EqualEqual;
                ++i;
            } else {
                token.kind = TokenKind::Assign;
            }
            break;
        default:
            throw ScriptError(located(
                std::string("unexpected character '") + c + "'",
                line, i - line_start + 1));
        }
        ++i;
        tokens.push_back(std::move(token));
    }

    tokens.push_back(Token{TokenKind::End, {}, 0, line});
    return tokens;
}

} // namespace homeworldz::script
