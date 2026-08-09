#include "homeworldz/script/parser.h"

#include <string>

namespace homeworldz::script {
namespace {

// Binding powers for the Pratt expression parser. Higher binds tighter.
int binary_precedence(TokenKind kind) {
    switch (kind) {
    case TokenKind::EqualEqual: return 1;
    case TokenKind::Less:
    case TokenKind::Greater: return 2;
    case TokenKind::Plus:
    case TokenKind::Minus: return 3;
    case TokenKind::Star: return 4;
    default: return -1;
    }
}

ast::BinaryOp binary_op_of(TokenKind kind) {
    switch (kind) {
    case TokenKind::Plus: return ast::BinaryOp::Add;
    case TokenKind::Minus: return ast::BinaryOp::Sub;
    case TokenKind::Star: return ast::BinaryOp::Mul;
    case TokenKind::Less: return ast::BinaryOp::Less;
    case TokenKind::Greater: return ast::BinaryOp::Greater;
    case TokenKind::EqualEqual: return ast::BinaryOp::Equal;
    default: throw ScriptError("internal: not a binary operator");
    }
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    ast::Script parse_script() {
        ast::Script script;
        // Global variables precede the state, each starting with a type keyword.
        while (check(TokenKind::KwInteger) || check(TokenKind::KwString)) {
            script.globals.push_back(parse_global());
        }
        expect(TokenKind::KwDefault, "expected global declarations or 'default'");
        expect(TokenKind::LBrace, "expected '{' after 'default'");
        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            script.events.push_back(parse_event_handler());
        }
        expect(TokenKind::RBrace, "expected '}' to close default state");
        expect(TokenKind::End, "unexpected trailing tokens after default state");
        if (script.events.empty()) {
            throw ScriptError("the default state has no event handlers");
        }
        return script;
    }

private:
    const Token& peek(std::size_t ahead = 0) const {
        std::size_t idx = pos_ + ahead;
        if (idx >= tokens_.size()) {
            idx = tokens_.size() - 1; // the End token
        }
        return tokens_[idx];
    }

    const Token& advance() { return tokens_[pos_++]; }

    bool check(TokenKind kind) const { return peek().kind == kind; }

    bool match(TokenKind kind) {
        if (check(kind)) {
            ++pos_;
            return true;
        }
        return false;
    }

    const Token& expect(TokenKind kind, const char* message) {
        if (!check(kind)) {
            throw ScriptError(std::string(message) + " (line " +
                              std::to_string(peek().line) + ")");
        }
        return advance();
    }

    Type type_keyword() {
        if (match(TokenKind::KwInteger)) {
            return Type::Integer;
        }
        if (match(TokenKind::KwString)) {
            return Type::String;
        }
        throw ScriptError("expected a type (line " + std::to_string(peek().line) + ")");
    }

    ast::GlobalVar parse_global() {
        ast::GlobalVar global;
        global.type = type_keyword();
        global.name = expect(TokenKind::Identifier, "expected global variable name").text;
        if (match(TokenKind::Assign)) {
            // The PoC allows a literal initializer only.
            if (check(TokenKind::IntLiteral)) {
                if (global.type != Type::Integer) {
                    throw ScriptError("cannot initialize a string global with an integer literal");
                }
                global.int_init = advance().integer;
            } else if (check(TokenKind::StringLiteral)) {
                if (global.type != Type::String) {
                    throw ScriptError("cannot initialize an integer global with a string literal");
                }
                global.string_init = advance().text;
            } else {
                throw ScriptError("global initializers must be a literal in this PoC");
            }
            global.has_init = true;
        }
        expect(TokenKind::Semicolon, "expected ';' after global declaration");
        return global;
    }

    ast::EventHandler parse_event_handler() {
        ast::EventHandler handler;
        handler.name = expect(TokenKind::Identifier, "expected an event name").text;
        expect(TokenKind::LParen, "expected '(' after event name");
        if (!check(TokenKind::RParen)) {
            for (;;) {
                ast::Param param;
                param.type = type_keyword();
                param.name = expect(TokenKind::Identifier, "expected parameter name").text;
                handler.params.push_back(std::move(param));
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
        }
        expect(TokenKind::RParen, "expected ')' after event parameters");
        handler.body = parse_block();
        return handler;
    }

    std::unique_ptr<ast::Block> parse_block() {
        expect(TokenKind::LBrace, "expected '{'");
        auto block = std::make_unique<ast::Block>();
        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            block->statements.push_back(parse_statement());
        }
        expect(TokenKind::RBrace, "expected '}'");
        return block;
    }

    ast::StmtPtr parse_statement() {
        if (check(TokenKind::KwInteger) || check(TokenKind::KwString)) {
            return parse_var_decl();
        }
        if (check(TokenKind::KwWhile)) {
            return parse_while();
        }
        if (check(TokenKind::KwIf)) {
            return parse_if();
        }
        if (check(TokenKind::LBrace)) {
            return parse_block();
        }
        // Assignment (ident = expr;) or an expression statement (e.g. a call).
        if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Assign) {
            auto assign = std::make_unique<ast::Assign>();
            assign->name = advance().text;
            expect(TokenKind::Assign, "expected '='");
            assign->value = parse_expression(0);
            expect(TokenKind::Semicolon, "expected ';' after assignment");
            return assign;
        }
        auto stmt = std::make_unique<ast::ExprStmt>();
        stmt->expr = parse_expression(0);
        expect(TokenKind::Semicolon, "expected ';' after expression");
        return stmt;
    }

    ast::StmtPtr parse_var_decl() {
        auto decl = std::make_unique<ast::VarDecl>();
        decl->type = type_keyword();
        decl->name = expect(TokenKind::Identifier, "expected variable name").text;
        if (match(TokenKind::Assign)) {
            decl->init = parse_expression(0);
        }
        expect(TokenKind::Semicolon, "expected ';' after declaration");
        return decl;
    }

    ast::StmtPtr parse_while() {
        advance(); // while
        expect(TokenKind::LParen, "expected '(' after 'while'");
        auto node = std::make_unique<ast::While>();
        node->condition = parse_expression(0);
        expect(TokenKind::RParen, "expected ')' after condition");
        node->body = parse_block();
        return node;
    }

    ast::StmtPtr parse_if() {
        advance(); // if
        expect(TokenKind::LParen, "expected '(' after 'if'");
        auto node = std::make_unique<ast::If>();
        node->condition = parse_expression(0);
        expect(TokenKind::RParen, "expected ')' after condition");
        node->then_branch = parse_block();
        if (match(TokenKind::KwElse)) {
            node->else_branch = parse_block();
        }
        return node;
    }

    ast::ExprPtr parse_expression(int min_precedence) {
        ast::ExprPtr left = parse_unary();
        for (;;) {
            const int prec = binary_precedence(peek().kind);
            if (prec < 0 || prec < min_precedence) {
                break;
            }
            const TokenKind op_kind = advance().kind;
            auto binary = std::make_unique<ast::Binary>();
            binary->op = binary_op_of(op_kind);
            binary->lhs = std::move(left);
            binary->rhs = parse_expression(prec + 1); // left-associative
            left = std::move(binary);
        }
        return left;
    }

    ast::ExprPtr parse_unary() {
        // A cast: '(' type ')' operand, distinguished from a parenthesized group.
        if (check(TokenKind::LParen) &&
            (peek(1).kind == TokenKind::KwInteger || peek(1).kind == TokenKind::KwString) &&
            peek(2).kind == TokenKind::RParen) {
            advance(); // (
            const Type target = check(TokenKind::KwString) ? Type::String : Type::Integer;
            advance(); // type
            advance(); // )
            auto cast = std::make_unique<ast::Cast>();
            cast->target = target;
            cast->operand = parse_unary();
            return cast;
        }
        return parse_primary();
    }

    ast::ExprPtr parse_primary() {
        if (check(TokenKind::IntLiteral)) {
            auto node = std::make_unique<ast::IntLiteral>();
            node->value = advance().integer;
            return node;
        }
        if (check(TokenKind::StringLiteral)) {
            auto node = std::make_unique<ast::StringLiteral>();
            node->value = advance().text;
            return node;
        }
        if (check(TokenKind::Identifier)) {
            std::string name = advance().text;
            if (match(TokenKind::LParen)) {
                auto call = std::make_unique<ast::Call>();
                call->callee = std::move(name);
                if (!check(TokenKind::RParen)) {
                    call->args.push_back(parse_expression(0));
                    while (match(TokenKind::Comma)) {
                        call->args.push_back(parse_expression(0));
                    }
                }
                expect(TokenKind::RParen, "expected ')' to close call arguments");
                return call;
            }
            auto ref = std::make_unique<ast::VarRef>();
            ref->name = std::move(name);
            return ref;
        }
        if (match(TokenKind::LParen)) {
            ast::ExprPtr inner = parse_expression(0);
            expect(TokenKind::RParen, "expected ')'");
            return inner;
        }
        throw ScriptError("expected an expression (line " +
                          std::to_string(peek().line) + ")");
    }

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

} // namespace

ast::Script parse(const std::string& source) {
    Parser parser(tokenize(source));
    return parser.parse_script();
}

} // namespace homeworldz::script
