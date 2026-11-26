#include "homeworldz/script/lexer.h"

#include <cstdlib>

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

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// Where a float's exponent ends when one starts at `at` (e3, E-3, e+10), or
// `at` itself when what is there is not an exponent.
std::size_t exponent_end(const std::string& source, std::size_t at) {
    const std::size_t n = source.size();
    if (at >= n || (source[at] != 'e' && source[at] != 'E')) return at;
    std::size_t i = at + 1;
    if (i < n && (source[i] == '+' || source[i] == '-')) ++i;
    if (i >= n || !is_digit(source[i])) return at;
    while (i < n && is_digit(source[i])) ++i;
    return i;
}

TokenKind keyword_kind(const std::string& word) {
    static const std::unordered_map<std::string, TokenKind> keywords = {
        {"default", TokenKind::KwDefault},
        {"integer", TokenKind::KwInteger},
        {"float", TokenKind::KwFloat},
        {"string", TokenKind::KwString},
        {"key", TokenKind::KwKey},
        {"vector", TokenKind::KwVector},
        {"rotation", TokenKind::KwRotation},
        {"quaternion", TokenKind::KwRotation}, // LSL's other spelling
        {"list", TokenKind::KwList},
        {"while", TokenKind::KwWhile},
        {"if", TokenKind::KwIf},
        {"else", TokenKind::KwElse},
        {"return", TokenKind::KwReturn},
        {"for", TokenKind::KwFor},
        {"do", TokenKind::KwDo},
        {"print", TokenKind::KwPrint},
    };
    const auto it = keywords.find(word);
    return it == keywords.end() ? TokenKind::Identifier : it->second;
}

ScriptError located(const std::string& message, int line, std::size_t column) {
    return ScriptError(message, line, static_cast<int>(column));
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
                throw located(
                    "unterminated block comment", comment_line, comment_column);
            }
            i += 2;
            continue;
        }

        Token token;
        token.line = line;
        token.column = static_cast<int>(i - line_start + 1);

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
            const auto start = i;
            while (i < n && is_digit(source[i])) ++i;
            // A dot, with or without digits after it, or an exponent makes
            // this a float: LSL writes 1.0, 1., 1e3 and 1.5e-3.
            bool is_float = false;
            if (i < n && source[i] == '.') {
                is_float = true;
                ++i;
                while (i < n && is_digit(source[i])) ++i;
            }
            if (const auto end = exponent_end(source, i); end != i) {
                is_float = true;
                i = end;
            }
            if (is_float) {
                token.kind = TokenKind::FloatLiteral;
                token.number = std::strtod(source.substr(start, i - start).c_str(), nullptr);
                if (i < n && (source[i] == 'f' || source[i] == 'F')) ++i; // 1.0f
                tokens.push_back(std::move(token));
                continue;
            }
            std::int64_t value = 0;
            for (auto digit = start; digit < i; ++digit) {
                value = value * 10 + (source[digit] - '0');
                if (value > 0x7fffffffLL) {
                    throw located(
                        "integer literal out of range", line, literal_column);
                }
            }
            token.kind = TokenKind::IntLiteral;
            token.integer = static_cast<std::int32_t>(value);
            tokens.push_back(std::move(token));
            continue;
        }
        // A leading dot, as in .5, which LSL also accepts.
        if (c == '.' && i + 1 < n && is_digit(source[i + 1])) {
            const auto start = i;
            ++i;
            while (i < n && is_digit(source[i])) ++i;
            i = exponent_end(source, i);
            token.kind = TokenKind::FloatLiteral;
            token.number = std::strtod(source.substr(start, i - start).c_str(), nullptr);
            if (i < n && (source[i] == 'f' || source[i] == 'F')) ++i;
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
                    throw located(
                        "unterminated string literal", string_line, string_column);
                }
                text.push_back(ch);
                ++i;
            }
            if (i >= n) {
                throw located(
                    "unterminated string literal", string_line, string_column);
            }
            ++i; // closing quote
            token.kind = TokenKind::StringLiteral;
            token.text = std::move(text);
            tokens.push_back(std::move(token));
            continue;
        }

        // Punctuation and operators. `next` takes the second character of a
        // two-character operator when it is the one asked for.
        const auto next = [&](char second) {
            if (i + 1 < n && source[i + 1] == second) {
                ++i;
                return true;
            }
            return false;
        };
        switch (c) {
        case '(': token.kind = TokenKind::LParen; break;
        case ')': token.kind = TokenKind::RParen; break;
        case '{': token.kind = TokenKind::LBrace; break;
        case '}': token.kind = TokenKind::RBrace; break;
        case '[': token.kind = TokenKind::LBracket; break;
        case ']': token.kind = TokenKind::RBracket; break;
        case ';': token.kind = TokenKind::Semicolon; break;
        case ',': token.kind = TokenKind::Comma; break;
        case '+':
            token.kind = next('+') ? TokenKind::PlusPlus : next('=') ? TokenKind::PlusAssign : TokenKind::Plus;
            break;
        case '-':
            token.kind = next('-') ? TokenKind::MinusMinus : next('=') ? TokenKind::MinusAssign : TokenKind::Minus;
            break;
        case '*': token.kind = next('=') ? TokenKind::StarAssign : TokenKind::Star; break;
        case '/': token.kind = next('=') ? TokenKind::SlashAssign : TokenKind::Slash; break;
        case '%': token.kind = next('=') ? TokenKind::PercentAssign : TokenKind::Percent; break;
        case '<':
            if (i + 1 < n && source[i + 1] == '=') {
                token.kind = TokenKind::LessEqual;
                ++i;
            } else {
                token.kind = TokenKind::Less;
            }
            break;
        case '>':
            if (i + 1 < n && source[i + 1] == '=') {
                token.kind = TokenKind::GreaterEqual;
                ++i;
            } else {
                token.kind = TokenKind::Greater;
            }
            break;
        case '=':
            if (i + 1 < n && source[i + 1] == '=') {
                token.kind = TokenKind::EqualEqual;
                ++i;
            } else {
                token.kind = TokenKind::Assign;
            }
            break;
        case '!':
            if (i + 1 < n && source[i + 1] == '=') {
                token.kind = TokenKind::BangEqual;
                ++i;
            } else {
                token.kind = TokenKind::Bang;
            }
            break;
        case '&':
            if (i + 1 < n && source[i + 1] == '&') {
                token.kind = TokenKind::AmpAmp;
                ++i;
            } else {
                token.kind = TokenKind::Amp;
            }
            break;
        case '|':
            if (i + 1 < n && source[i + 1] == '|') {
                token.kind = TokenKind::PipePipe;
                ++i;
            } else {
                token.kind = TokenKind::Pipe;
            }
            break;
        default:
            throw located(
                std::string("unexpected character '") + c + "'",
                line, i - line_start + 1);
        }
        ++i;
        tokens.push_back(std::move(token));
    }

    tokens.push_back(Token{TokenKind::End, {}, 0, 0.0, line, static_cast<int>(n - line_start + 1)});
    return tokens;
}

} // namespace homeworldz::script
