#include "homeworldz/script/parser.h"

#include <string>

namespace homeworldz::script {
namespace {

// Binding powers for the Pratt expression parser. Higher binds tighter, in C's
// order, which is the order LSL documents.
int binary_precedence(TokenKind kind) {
    switch (kind) {
    case TokenKind::PipePipe: return 1;
    case TokenKind::AmpAmp: return 2;
    case TokenKind::Pipe: return 3;
    case TokenKind::Amp: return 4;
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual: return 5;
    case TokenKind::Less:
    case TokenKind::Greater:
    case TokenKind::LessEqual:
    case TokenKind::GreaterEqual: return 6;
    case TokenKind::Plus:
    case TokenKind::Minus: return 7;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent: return 8;
    default: return -1;
    }
}

ast::BinaryOp binary_op_of(TokenKind kind) {
    switch (kind) {
    case TokenKind::Plus: return ast::BinaryOp::Add;
    case TokenKind::Minus: return ast::BinaryOp::Sub;
    case TokenKind::Star: return ast::BinaryOp::Mul;
    case TokenKind::Slash: return ast::BinaryOp::Div;
    case TokenKind::Percent: return ast::BinaryOp::Mod;
    case TokenKind::Less: return ast::BinaryOp::Less;
    case TokenKind::Greater: return ast::BinaryOp::Greater;
    case TokenKind::LessEqual: return ast::BinaryOp::LessEqual;
    case TokenKind::GreaterEqual: return ast::BinaryOp::GreaterEqual;
    case TokenKind::EqualEqual: return ast::BinaryOp::Equal;
    case TokenKind::BangEqual: return ast::BinaryOp::NotEqual;
    case TokenKind::Amp: return ast::BinaryOp::BitAnd;
    case TokenKind::Pipe: return ast::BinaryOp::BitOr;
    case TokenKind::AmpAmp: return ast::BinaryOp::And;
    case TokenKind::PipePipe: return ast::BinaryOp::Or;
    default: throw ScriptError("internal: not a binary operator");
    }
}

// The operator an assignment token applies, for the compound ones.
bool is_assignment(TokenKind kind) {
    return kind == TokenKind::Assign || kind == TokenKind::PlusAssign || kind == TokenKind::MinusAssign ||
           kind == TokenKind::StarAssign || kind == TokenKind::SlashAssign || kind == TokenKind::PercentAssign;
}

ast::BinaryOp assignment_op(TokenKind kind) {
    switch (kind) {
    case TokenKind::MinusAssign: return ast::BinaryOp::Sub;
    case TokenKind::StarAssign: return ast::BinaryOp::Mul;
    case TokenKind::SlashAssign: return ast::BinaryOp::Div;
    case TokenKind::PercentAssign: return ast::BinaryOp::Mod;
    default: return ast::BinaryOp::Add;
    }
}

bool is_type_keyword(TokenKind kind) {
    return kind == TokenKind::KwInteger || kind == TokenKind::KwFloat ||
           kind == TokenKind::KwString ||
           kind == TokenKind::KwKey || kind == TokenKind::KwVector ||
           kind == TokenKind::KwRotation || kind == TokenKind::KwList;
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    ast::Script parse_script() {
        ast::Script script;
        // Global variables and functions precede the state. A function is a
        // name followed by '(', with or without a type before it.
        for (;;) {
            const bool typed = is_type_keyword(peek().kind);
            const std::size_t name = typed ? 1 : 0;
            if (peek(name).kind == TokenKind::Identifier && peek(name + 1).kind == TokenKind::LParen) {
                script.functions.push_back(parse_function());
            } else if (typed) {
                script.globals.push_back(parse_global());
            } else {
                break;
            }
        }
        expect(TokenKind::KwDefault, "expected a global, a function or 'default'");
        expect(TokenKind::LBrace, "expected '{' after 'default'");
        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            script.events.push_back(parse_event_handler());
        }
        expect(TokenKind::RBrace, "expected '}' to close default state");
        expect(TokenKind::End, "unexpected trailing tokens after default state");
        if (script.events.empty()) {
            fail("the default state has no event handlers");
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

    // A syntax error at the token the parser is looking at.
    [[noreturn]] void fail(const std::string& message) const {
        throw ScriptError(message, peek().line, peek().column);
    }

    // A node that starts at the given token.
    template <typename Node>
    static std::unique_ptr<Node> make(const Token& at) {
        auto node = std::make_unique<Node>();
        node->line = at.line;
        node->column = at.column;
        return node;
    }

    const Token& expect(TokenKind kind, const char* message) {
        if (!check(kind)) {
            fail(message);
        }
        return advance();
    }

    Type type_keyword() {
        if (match(TokenKind::KwInteger)) return Type::Integer;
        if (match(TokenKind::KwFloat)) return Type::Float;
        if (match(TokenKind::KwString)) return Type::String;
        if (match(TokenKind::KwKey)) return Type::Key;
        if (match(TokenKind::KwVector)) return Type::Vector;
        if (match(TokenKind::KwRotation)) return Type::Rotation;
        if (match(TokenKind::KwList)) return Type::List;
        fail("expected a type");
    }

    ast::GlobalVar parse_global() {
        ast::GlobalVar global;
        global.line = peek().line;
        global.column = peek().column;
        global.type = type_keyword();
        global.name = expect(TokenKind::Identifier, "expected global variable name").text;
        if (match(TokenKind::Assign)) {
            global.init = parse_expression(0);
        }
        expect(TokenKind::Semicolon, "expected ';' after global declaration");
        return global;
    }

    // The parameters of an event handler or a function, after its '('.
    std::vector<ast::Param> parse_params() {
        std::vector<ast::Param> params;
        if (!check(TokenKind::RParen)) {
            for (;;) {
                ast::Param param;
                param.type = type_keyword();
                param.name = expect(TokenKind::Identifier, "expected parameter name").text;
                params.push_back(std::move(param));
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
        }
        expect(TokenKind::RParen, "expected ')' after parameters");
        return params;
    }

    ast::EventHandler parse_event_handler() {
        ast::EventHandler handler;
        handler.line = peek().line;
        handler.column = peek().column;
        handler.name = expect(TokenKind::Identifier, "expected an event name").text;
        expect(TokenKind::LParen, "expected '(' after event name");
        handler.params = parse_params();
        handler.body = parse_block();
        return handler;
    }

    ast::Function parse_function() {
        ast::Function function;
        function.line = peek().line;
        function.column = peek().column;
        if (is_type_keyword(peek().kind)) function.result = type_keyword();
        function.name = advance().text;
        advance(); // (
        function.params = parse_params();
        function.body = parse_block();
        return function;
    }

    // The body of an `if`, `else` or `while`: a block, or one statement, which
    // is a block of one. LSL gives a declaration no scope of its own there.
    std::unique_ptr<ast::Block> parse_body() {
        if (check(TokenKind::LBrace)) return parse_block();
        if (is_type_keyword(peek().kind)) fail("a declaration here needs braces around it");
        auto block = make<ast::Block>(peek());
        block->statements.push_back(parse_statement());
        return block;
    }

    std::unique_ptr<ast::Block> parse_block() {
        auto block = make<ast::Block>(peek());
        expect(TokenKind::LBrace, "expected '{'");
        while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
            block->statements.push_back(parse_statement());
        }
        expect(TokenKind::RBrace, "expected '}'");
        return block;
    }

    // One level of recursion in the parser, counted so that source nested
    // past kMaxDepth is refused instead of overflowing the native stack.
    class Nest {
    public:
        explicit Nest(Parser& parser) : parser_(parser) {
            if (++parser_.nesting_ > ast::kMaxDepth) parser_.fail("nested too deeply");
        }
        ~Nest() { --parser_.nesting_; }
        Nest(const Nest&) = delete;
        Nest& operator=(const Nest&) = delete;

    private:
        Parser& parser_;
    };

    // Records that `node` holds `child`, and refuses a tree grown too deep. A
    // long chain of operators is as deep as it is long.
    static void holds(ast::Expr& node, const ast::Expr& child) {
        if (child.depth + 1 > node.depth) node.depth = child.depth + 1;
        if (node.depth > ast::kMaxDepth) {
            throw ScriptError("expression too long or nested too deeply", node.line, node.column);
        }
    }

    ast::StmtPtr parse_statement() {
        const Nest nest(*this);
        if (is_type_keyword(peek().kind)) {
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
        if (check(TokenKind::Semicolon)) {
            return make<ast::Block>(advance()); // an empty statement
        }
        if (check(TokenKind::KwReturn)) {
            auto node = make<ast::Return>(advance());
            if (!check(TokenKind::Semicolon)) node->value = parse_expression(0);
            expect(TokenKind::Semicolon, "expected ';' after return");
            return node;
        }
        if (check(TokenKind::KwFor)) {
            return parse_for();
        }
        if (check(TokenKind::KwDo)) {
            auto node = make<ast::DoWhile>(advance()); // do
            node->body = parse_body();
            expect(TokenKind::KwWhile, "expected 'while' after the body of 'do'");
            expect(TokenKind::LParen, "expected '(' after 'while'");
            node->condition = parse_expression(0);
            expect(TokenKind::RParen, "expected ')' after condition");
            expect(TokenKind::Semicolon, "expected ';' after do ... while");
            return node;
        }
        // An expression statement: an assignment, a call, an increment.
        auto stmt = make<ast::ExprStmt>(peek());
        stmt->expr = parse_expression(0);
        expect(TokenKind::Semicolon, "expected ';' after expression");
        return stmt;
    }

    ast::StmtPtr parse_var_decl() {
        auto decl = make<ast::VarDecl>(peek());
        decl->type = type_keyword();
        decl->name = expect(TokenKind::Identifier, "expected variable name").text;
        if (match(TokenKind::Assign)) {
            decl->init = parse_expression(0);
        }
        expect(TokenKind::Semicolon, "expected ';' after declaration");
        return decl;
    }

    // The expressions of a for loop's first or last part, up to `end`.
    std::vector<ast::ExprPtr> parse_for_list(TokenKind end) {
        std::vector<ast::ExprPtr> list;
        if (!check(end)) {
            list.push_back(parse_expression(0));
            while (match(TokenKind::Comma)) list.push_back(parse_expression(0));
        }
        return list;
    }

    ast::StmtPtr parse_for() {
        auto node = make<ast::For>(advance()); // for
        expect(TokenKind::LParen, "expected '(' after 'for'");
        node->init = parse_for_list(TokenKind::Semicolon);
        expect(TokenKind::Semicolon, "expected ';' after the first part of 'for'");
        node->condition = parse_expression(0);
        expect(TokenKind::Semicolon, "expected ';' after the condition of 'for'");
        node->step = parse_for_list(TokenKind::RParen);
        expect(TokenKind::RParen, "expected ')' to close 'for'");
        node->body = parse_body();
        return node;
    }

    ast::StmtPtr parse_while() {
        auto node = make<ast::While>(advance()); // while
        expect(TokenKind::LParen, "expected '(' after 'while'");
        node->condition = parse_expression(0);
        expect(TokenKind::RParen, "expected ')' after condition");
        node->body = parse_body();
        return node;
    }

    ast::StmtPtr parse_if() {
        auto node = make<ast::If>(advance()); // if
        expect(TokenKind::LParen, "expected '(' after 'if'");
        node->condition = parse_expression(0);
        expect(TokenKind::RParen, "expected ')' after condition");
        node->then_branch = parse_body();
        if (match(TokenKind::KwElse)) {
            node->else_branch = parse_body(); // `else if` is a body of one `if`
        }
        return node;
    }

    ast::ExprPtr parse_expression(int min_precedence) {
        ast::ExprPtr left = parse_unary();
        // An assignment binds loosest and to the right: a = b = c. It is taken
        // only where a whole expression is being read, so a + b = c is refused.
        if (min_precedence == 0 && is_assignment(peek().kind)) {
            const auto* target = dynamic_cast<const ast::VarRef*>(left.get());
            if (target == nullptr) fail("only a variable can be assigned to");
            const Nest nest(*this);
            auto assign = std::make_unique<ast::Assign>();
            assign->line = target->line;
            assign->column = target->column;
            assign->name = target->name;
            const TokenKind op = advance().kind;
            assign->compound = op != TokenKind::Assign;
            assign->op = assignment_op(op);
            assign->value = parse_expression(0);
            holds(*assign, *assign->value);
            return assign;
        }
        for (;;) {
            const int prec = binary_precedence(peek().kind);
            if (prec < 0 || prec < min_precedence) {
                break;
            }
            const Token& op = advance();
            auto binary = make<ast::Binary>(op);
            binary->op = binary_op_of(op.kind);
            binary->lhs = std::move(left);
            binary->rhs = parse_expression(prec + 1); // left-associative
            holds(*binary, *binary->lhs);
            holds(*binary, *binary->rhs);
            left = std::move(binary);
        }
        return left;
    }

    ast::ExprPtr parse_unary() {
        const Nest nest(*this);
        // A cast: '(' type ')' operand, distinguished from a parenthesized group.
        if (check(TokenKind::LParen) &&
            (peek(1).kind == TokenKind::KwInteger || peek(1).kind == TokenKind::KwFloat ||
             peek(1).kind == TokenKind::KwString || peek(1).kind == TokenKind::KwKey ||
             peek(1).kind == TokenKind::KwList) &&
            peek(2).kind == TokenKind::RParen) {
            const Token& open = advance(); // (
            const Type target = type_keyword();
            advance(); // )
            auto cast = make<ast::Cast>(open);
            cast->target = target;
            cast->operand = parse_unary();
            holds(*cast, *cast->operand);
            return cast;
        }
        if (check(TokenKind::PlusPlus) || check(TokenKind::MinusMinus)) {
            auto node = make<ast::IncDec>(peek());
            node->increment = advance().kind == TokenKind::PlusPlus;
            node->name = expect(TokenKind::Identifier, "expected a variable after ++ or --").text;
            return node;
        }
        if (check(TokenKind::Minus) || check(TokenKind::Bang)) {
            auto unary = make<ast::Unary>(peek());
            unary->op = advance().kind == TokenKind::Minus ? ast::UnaryOp::Negate
                                                           : ast::UnaryOp::Not;
            unary->operand = parse_unary();
            holds(*unary, *unary->operand);
            return unary;
        }
        return parse_primary();
    }

    // A numeric component of a vector or rotation literal, with an optional
    // sign. Integers are accepted and widened, because <0, 0, 1> is how anyone
    // writes an axis and refusing it would be pedantry.
    double parse_vector_component() {
        double sign = 1.0;
        if (match(TokenKind::Minus)) sign = -1.0;
        else static_cast<void>(match(TokenKind::Plus));
        if (check(TokenKind::FloatLiteral)) return sign * advance().number;
        if (check(TokenKind::IntLiteral))
            return sign * static_cast<double>(advance().integer);
        fail("expected a number inside a vector or rotation literal");
    }

    ast::ExprPtr parse_primary() {
        // '<' in a position where a value belongs opens a vector or rotation
        // literal; everywhere else it is still less-than. This is LSL's own
        // resolution of the ambiguity, and it is why the literal is only
        // recognized here rather than anywhere an operator could appear.
        if (check(TokenKind::Less)) {
            auto node = make<ast::VectorLiteral>(advance());
            node->components.push_back(parse_vector_component());
            while (match(TokenKind::Comma))
                node->components.push_back(parse_vector_component());
            expect(TokenKind::Greater, "expected '>' to close a vector or rotation literal");
            if (node->components.size() != 3 && node->components.size() != 4)
                throw ScriptError("a vector takes 3 components and a rotation 4", node->line,
                                  node->column);
            return node;
        }
        if (check(TokenKind::LBracket)) {
            auto node = make<ast::ListLiteral>(advance());
            if (!check(TokenKind::RBracket)) {
                node->elements.push_back(parse_expression(0));
                while (match(TokenKind::Comma)) {
                    node->elements.push_back(parse_expression(0));
                }
            }
            expect(TokenKind::RBracket, "expected ']' to close a list");
            for (const auto& element : node->elements) holds(*node, *element);
            return node;
        }
        if (check(TokenKind::FloatLiteral)) {
            auto node = make<ast::FloatLiteral>(peek());
            node->value = advance().number;
            return node;
        }
        if (check(TokenKind::IntLiteral)) {
            auto node = make<ast::IntLiteral>(peek());
            node->value = advance().integer;
            return node;
        }
        if (check(TokenKind::StringLiteral)) {
            auto node = make<ast::StringLiteral>(peek());
            node->value = advance().text;
            return node;
        }
        if (check(TokenKind::Identifier)) {
            const Token& identifier = advance();
            std::string name = identifier.text;
            if (match(TokenKind::LParen)) {
                auto call = make<ast::Call>(identifier);
                call->callee = std::move(name);
                if (!check(TokenKind::RParen)) {
                    call->args.push_back(parse_expression(0));
                    while (match(TokenKind::Comma)) {
                        call->args.push_back(parse_expression(0));
                    }
                }
                expect(TokenKind::RParen, "expected ')' to close call arguments");
                for (const auto& arg : call->args) holds(*call, *arg);
                return call;
            }
            if (check(TokenKind::PlusPlus) || check(TokenKind::MinusMinus)) {
                auto node = make<ast::IncDec>(identifier);
                node->name = std::move(name);
                node->increment = advance().kind == TokenKind::PlusPlus;
                node->prefix = false;
                return node;
            }
            auto ref = make<ast::VarRef>(identifier);
            ref->name = std::move(name);
            return ref;
        }
        if (match(TokenKind::LParen)) {
            ast::ExprPtr inner = parse_expression(0);
            expect(TokenKind::RParen, "expected ')'");
            return inner;
        }
        fail("expected an expression");
    }

    std::vector<Token> tokens_;
    int nesting_ = 0;
    std::size_t pos_ = 0;
};

} // namespace

ast::Script parse(const std::string& source) {
    Parser parser(tokenize(source));
    return parser.parse_script();
}

} // namespace homeworldz::script
