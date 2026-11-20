#include "homeworldz/script/compiler.h"

#include "homeworldz/script/lexer.h"
#include "homeworldz/script/library.h"
#include "homeworldz/script/parser.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace homeworldz::script {
namespace {

const char* type_name(Type type) {
    switch (type) {
    case Type::Integer: return "integer";
    case Type::String: return "string";
    case Type::Void: return "void";
    case Type::Vector: return "vector";
    case Type::Rotation: return "rotation";
    case Type::Key: return "key";
    case Type::Float: return "float";
    case Type::List: return "list";
    }
    return "?";
}

// LSL converts between string and key implicitly wherever one is given for the
// other, and widens an integer given where a float is wanted; every other pair
// of types must match exactly. Both conversions need code (Compiler::convert).
bool assignable(Type to, Type from) {
    if (to == from) return true;
    return (to == Type::Key && from == Type::String) ||
           (to == Type::String && from == Type::Key) ||
           (to == Type::Float && from == Type::Integer);
}

// The whole compatibility surface, generated from falcon/index: every
// signature and every constant value comes from there, not from this file.
#include "lsl_index.inc"

// The functions the region answers today; Falcon's own library answers more
// (library.cpp). A script calling a function in neither does not compile,
// even though the index knows its signature: compiling
// a call that then does nothing would hide the gap from the script's author.
// That is why the sitting unit (ADR 0040) brought the handful every chair and
// pose ball calls, not only the sit functions themselves.
const std::unordered_set<std::string_view> implemented_functions = {
    "llOwnerSay",
    "llSay",
    "llSitTarget",
    "llLinkSitTarget",
    "llAvatarOnSitTarget",
    "llAvatarOnLinkSitTarget",
    "llUnSit",
    "llSetSitText",
    "llSetClickAction",
    "llSetCameraEyeOffset",
    "llSetCameraAtOffset",
    "llRequestPermissions",
    "llGetPermissions",
    "llGetPermissionsKey",
    "llStartAnimation",
    "llStopAnimation",
    "llGetInventoryName",
    "llSleep",
    "llSetTimerEvent",
};

// The events the region raises today.
const std::unordered_set<std::string_view> implemented_events = {
    "state_entry",
    "touch_start",
    "changed",
    "run_time_permissions",
    "timer",
};

// An index type as a Falcon type.
std::optional<Type> falcon_type(char code) {
    switch (code) {
    case 'v': return Type::Void;
    case 'i': return Type::Integer;
    case 'f': return Type::Float;
    case 's': return Type::String;
    case 'k': return Type::Key;
    case 'V': return Type::Vector;
    case 'r': return Type::Rotation;
    case 'l': return Type::List;
    default: return std::nullopt;
    }
}

std::optional<std::vector<Type>> falcon_types(std::string_view codes) {
    std::vector<Type> types;
    for (const char code : codes) {
        const auto type = falcon_type(code);
        if (!type) return std::nullopt;
        types.push_back(*type);
    }
    return types;
}

struct HostSignature {
    std::vector<Type> params;
    Type result;
};

const HostSignature* lookup_host(const std::string& name) {
    static const auto table = [] {
        std::unordered_map<std::string, HostSignature> built;
        for (const auto& function : index_functions) {
            if (!implemented_functions.contains(function.name) && find_builtin(function.name) < 0) continue;
            const auto result = falcon_type(function.result);
            auto params = falcon_types(function.params);
            if (result && params) built.emplace(function.name, HostSignature{std::move(*params), *result});
        }
        return built;
    }();
    const auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

const std::vector<Type>* lookup_event(const std::string& name) {
    static const auto table = [] {
        std::unordered_map<std::string, std::vector<Type>> built;
        for (const auto& event : index_events) {
            if (!implemented_events.contains(event.name)) continue;
            if (auto params = falcon_types(event.params)) built.emplace(event.name, std::move(*params));
        }
        return built;
    }();
    const auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

// LSL's named constants, with Second Life's values. A name here that a script
// also declares as a variable is the variable's; LSL forbids that anyway.
const Value* lookup_constant(const std::string& name) {
    static const auto table = [] {
        std::unordered_map<std::string, Value> built;
        for (const auto& constant : index_integers)
            built.emplace(constant.name, Value::make_integer(constant.value));
        for (const auto& constant : index_floats)
            built.emplace(constant.name, Value::make_float(constant.value));
        for (const auto& constant : index_strings)
            built.emplace(constant.name, Value::make_string(std::string(constant.value)));
        // The one constant that is a key.
        built.at("NULL_KEY").type = Type::Key;
        for (const auto& constant : index_vectors) {
            const auto& c = constant.components;
            built.emplace(constant.name, constant.size == 4 ? Value::make_rotation(c[0], c[1], c[2], c[3])
                                                            : Value::make_vector(c[0], c[1], c[2]));
        }
        return built;
    }();
    const auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

// Whether two constants are the same value, for pooling. Every field counts:
// comparing only type, integer and string made every vector literal in a
// script the first one written, since vectors carry neither.
bool same_constant(const Value& a, const Value& b) {
    return a.type == b.type && a.integer == b.integer && a.str == b.str &&
           a.components == b.components &&
           std::equal(a.items().begin(), a.items().end(), b.items().begin(), b.items().end(),
                      same_constant);
}

enum class Scope { Local, Global };

class Compiler {
public:
    Program compile(const ast::Script& script) {
        try {
            for (const auto& global : script.globals) {
                declare_global(global);
            }
            for (const auto& handler : script.events) {
                compile_event(handler);
            }
        } catch (const ScriptError& error) {
            // A semantic error is raised without a place; it belongs to the
            // node being compiled when it was raised.
            if (error.line != 0 || line_ == 0) throw;
            throw ScriptError(error.message, line_, column_);
        }
        return std::move(program_);
    }

private:
    // The node being compiled, for placing errors. Entering a node moves here;
    // leaving it normally moves back to its parent, so a check the parent makes
    // after compiling its operands is reported at the parent. An error leaves
    // the position where it was raised.
    class Here {
    public:
        template <typename Node>
        Here(Compiler& compiler, const Node& node)
            : compiler_(compiler), line_(compiler.line_), column_(compiler.column_),
              exceptions_(std::uncaught_exceptions()) {
            if (node.line != 0) {
                compiler.line_ = node.line;
                compiler.column_ = node.column;
            }
        }
        ~Here() {
            if (std::uncaught_exceptions() != exceptions_) return;
            compiler_.line_ = line_;
            compiler_.column_ = column_;
        }
        Here(const Here&) = delete;
        Here& operator=(const Here&) = delete;

    private:
        Compiler& compiler_;
        int line_;
        int column_;
        int exceptions_;
    };

    int line_ = 0;
    int column_ = 0;

    struct Symbol {
        Scope scope;
        std::int32_t slot;
        Type type;
    };

    void declare_global(const ast::GlobalVar& global) {
        const Here position(*this, global);
        if (globals_.count(global.name) != 0) {
            throw ScriptError("global '" + global.name + "' already declared");
        }
        const std::int32_t slot = static_cast<std::int32_t>(program_.global_defaults.size());
        Value initial = default_value(global.type);
        if (global.init) {
            Type folded_type = Type::Void;
            initial = fold_constant(*global.init, folded_type);
            if (!assignable(global.type, folded_type)) {
                throw ScriptError("cannot initialize " + std::string(type_name(global.type)) +
                                  " global '" + global.name + "' with " + type_name(folded_type));
            }
            if (global.type == Type::Float && folded_type == Type::Integer) {
                initial = Value::make_float(static_cast<double>(initial.integer));
            }
            if (is_textual(global.type)) initial.type = global.type;
        }
        program_.global_defaults.push_back(std::move(initial));
        globals_.emplace(global.name, Symbol{Scope::Global, slot, global.type});
    }

    // A global initializer evaluated at compile time. LSL allows only constant
    // initializers, so anything that would need code to run is refused.
    Value fold_constant(const ast::Expr& expr, Type& type) {
        if (const auto* lit = dynamic_cast<const ast::IntLiteral*>(&expr)) {
            type = Type::Integer;
            return Value::make_integer(lit->value);
        }
        if (const auto* lit = dynamic_cast<const ast::StringLiteral*>(&expr)) {
            type = Type::String;
            return Value::make_string(lit->value);
        }
        if (const auto* lit = dynamic_cast<const ast::FloatLiteral*>(&expr)) {
            type = Type::Float;
            return Value::make_float(lit->value);
        }
        if (const auto* lit = dynamic_cast<const ast::VectorLiteral*>(&expr)) {
            const auto& p = lit->components;
            if (p.size() == 4) {
                type = Type::Rotation;
                return Value::make_rotation(p[0], p[1], p[2], p[3]);
            }
            type = Type::Vector;
            return Value::make_vector(p[0], p[1], p[2]);
        }
        if (const auto* lit = dynamic_cast<const ast::ListLiteral*>(&expr)) {
            std::vector<Value> items;
            for (const auto& element : lit->elements) {
                Type element_type = Type::Void;
                items.push_back(fold_constant(*element, element_type));
                if (element_type == Type::List) throw ScriptError("a list cannot hold a list");
            }
            type = Type::List;
            return Value::make_list(std::move(items));
        }
        if (const auto* ref = dynamic_cast<const ast::VarRef*>(&expr)) {
            // An earlier global, by the value it starts with.
            if (const auto global = globals_.find(ref->name); global != globals_.end()) {
                type = global->second.type;
                return program_.global_defaults[static_cast<std::size_t>(global->second.slot)];
            }
            if (const Value* constant = lookup_constant(ref->name)) {
                type = constant->type;
                return *constant;
            }
        }
        if (const auto* unary = dynamic_cast<const ast::Unary*>(&expr)) {
            if (unary->op == ast::UnaryOp::Negate) {
                Value inner = fold_constant(*unary->operand, type);
                if (type == Type::Integer) {
                    inner.integer = -inner.integer;
                    return inner;
                }
                if (type == Type::Float) return Value::make_float(-inner.number());
            }
        }
        throw ScriptError("a global initializer must be a constant");
    }

    static Value default_value(Type type) {
        switch (type) {
        case Type::String: return Value::make_string("");
        case Type::Key: return Value::make_key("00000000-0000-0000-0000-000000000000");
        case Type::List: return Value::make_list({});
        case Type::Vector: return Value::make_vector(0.0, 0.0, 0.0);
        case Type::Rotation: return Value::make_rotation(0.0, 0.0, 0.0, 1.0);
        case Type::Float: return Value::make_float(0.0);
        default: return Value::make_integer(0);
        }
    }

    void compile_event(const ast::EventHandler& handler) {
        const Here position(*this, handler);
        const std::vector<Type>* signature = lookup_event(handler.name);
        if (signature == nullptr) {
            throw ScriptError("unsupported event '" + handler.name + "' in this PoC");
        }
        if (program_.find_event(handler.name) != nullptr) {
            throw ScriptError("duplicate event handler '" + handler.name + "'");
        }
        if (handler.params.size() != signature->size()) {
            throw ScriptError("event '" + handler.name + "' has the wrong parameter count");
        }

        locals_.clear();
        local_count_ = 0;
        EventEntry entry;
        entry.name = handler.name;
        entry.entry_ip = here();
        for (std::size_t i = 0; i < handler.params.size(); ++i) {
            const ast::Param& param = handler.params[i];
            if (param.type != (*signature)[i]) {
                throw ScriptError("parameter " + std::to_string(i + 1) + " of '" +
                                  handler.name + "' must be " + type_name((*signature)[i]));
            }
            const std::int32_t slot = local_count_++;
            locals_.emplace(param.name, Symbol{Scope::Local, slot, param.type});
            entry.param_types.push_back(param.type);
        }
        compile_block(*handler.body);
        emit(Op::Halt);
        entry.local_count = local_count_;
        program_.events.push_back(std::move(entry));
    }

    std::int32_t emit(Op op, std::int32_t a = 0, std::int32_t b = 0) {
        program_.code.push_back(Instruction{op, a, b});
        program_.lines.push_back(line_);
        return static_cast<std::int32_t>(program_.code.size()) - 1;
    }

    // Makes the value on top of the stack, of type `from`, one of type `to`.
    // False when LSL does not convert between the two without a cast.
    bool convert(Type to, Type from) {
        if (!assignable(to, from)) return false;
        if (to == Type::Float && from == Type::Integer) emit(Op::IntToFloat);
        if (to == Type::Key && from == Type::String) emit(Op::ToKey);
        if (to == Type::String && from == Type::Key) emit(Op::ToStr);
        return true;
    }

    void patch_target(std::int32_t at, std::int32_t target) {
        program_.code[static_cast<std::size_t>(at)].a = target;
    }

    std::int32_t here() const { return static_cast<std::int32_t>(program_.code.size()); }

    std::int32_t add_constant(Value value) {
        for (std::size_t i = 0; i < program_.constants.size(); ++i) {
            if (same_constant(program_.constants[i], value)) {
                return static_cast<std::int32_t>(i);
            }
        }
        program_.constants.push_back(std::move(value));
        return static_cast<std::int32_t>(program_.constants.size()) - 1;
    }

    std::int32_t add_host(const std::string& name) {
        for (std::size_t i = 0; i < program_.host_names.size(); ++i) {
            if (program_.host_names[i] == name) {
                return static_cast<std::int32_t>(i);
            }
        }
        program_.host_names.push_back(name);
        return static_cast<std::int32_t>(program_.host_names.size()) - 1;
    }

    Symbol resolve(const std::string& name) {
        const auto local = locals_.find(name);
        if (local != locals_.end()) {
            return local->second;
        }
        const auto global = globals_.find(name);
        if (global != globals_.end()) {
            return global->second;
        }
        throw ScriptError("undeclared variable '" + name + "'");
    }

    void emit_load(const Symbol& symbol) {
        emit(symbol.scope == Scope::Global ? Op::LoadGlobal : Op::LoadLocal, symbol.slot);
    }

    void emit_store(const Symbol& symbol) {
        emit(symbol.scope == Scope::Global ? Op::StoreGlobal : Op::StoreLocal, symbol.slot);
    }

    void compile_block(const ast::Block& block) {
        for (const auto& stmt : block.statements) {
            compile_statement(*stmt);
        }
    }

    void compile_statement(const ast::Stmt& stmt) {
        const Here position(*this, stmt);
        if (const auto* block = dynamic_cast<const ast::Block*>(&stmt)) {
            compile_block(*block);
        } else if (const auto* decl = dynamic_cast<const ast::VarDecl*>(&stmt)) {
            compile_var_decl(*decl);
        } else if (const auto* assign = dynamic_cast<const ast::Assign*>(&stmt)) {
            compile_assign(*assign);
        } else if (const auto* expr = dynamic_cast<const ast::ExprStmt*>(&stmt)) {
            const Type result = compile_expression(*expr->expr);
            if (result != Type::Void) {
                emit(Op::Pop); // discard an unused value
            }
        } else if (const auto* loop = dynamic_cast<const ast::While*>(&stmt)) {
            compile_while(*loop);
        } else if (const auto* branch = dynamic_cast<const ast::If*>(&stmt)) {
            compile_if(*branch);
        } else {
            throw ScriptError("internal: unknown statement");
        }
    }

    void compile_var_decl(const ast::VarDecl& decl) {
        if (locals_.count(decl.name) != 0) {
            throw ScriptError("variable '" + decl.name + "' already declared");
        }
        const std::int32_t slot = local_count_++;
        if (decl.init) {
            const Type init_type = compile_expression(*decl.init);
            if (!convert(decl.type, init_type)) {
                throw ScriptError("cannot initialize " + std::string(type_name(decl.type)) +
                                  " '" + decl.name + "' with " + type_name(init_type));
            }
        } else {
            emit(Op::PushConst, default_constant(decl.type));
        }
        emit(Op::StoreLocal, slot);
        locals_.emplace(decl.name, Symbol{Scope::Local, slot, decl.type});
    }

    void compile_assign(const ast::Assign& assign) {
        const Symbol symbol = resolve(assign.name);
        const Type value_type = compile_expression(*assign.value);
        if (!convert(symbol.type, value_type)) {
            throw ScriptError("cannot assign " + std::string(type_name(value_type)) + " to " +
                              type_name(symbol.type) + " '" + assign.name + "'");
        }
        emit_store(symbol);
    }

    void compile_while(const ast::While& loop) {
        const std::int32_t start = here();
        compile_condition(*loop.condition, "while condition");
        const std::int32_t exit_jump = emit(Op::JumpIfZero, 0);
        compile_block(*loop.body);
        emit(Op::Jump, start);
        patch_target(exit_jump, here());
    }

    // A condition leaves one integer for JumpIfZero. LSL takes a string as true
    // when it is not empty, a key as true when it names something — which is
    // how a sit script asks `if (llAvatarOnSitTarget())` — and a float as true
    // when it is not zero.
    void compile_condition(const ast::Expr& expr, const char* context) {
        const Type type = compile_expression(expr);
        if (type == Type::String) {
            emit(Op::StrTruth);
        } else if (type == Type::Key) {
            emit(Op::KeyTruth);
        } else if (type == Type::Float) {
            emit(Op::PushConst, add_constant(Value::make_float(0.0)));
            emit(Op::EqualFloat);
            emit(Op::NotInt);
        } else if (type == Type::List) {
            // True when it has elements: its length less an empty list's.
            emit(Op::PushConst, add_constant(Value::make_list({})));
            emit(Op::NotEqualList);
        } else {
            require_integer(type, context);
        }
    }

    void compile_if(const ast::If& branch) {
        compile_condition(*branch.condition, "if condition");
        const std::int32_t else_jump = emit(Op::JumpIfZero, 0);
        compile_block(*branch.then_branch);
        if (branch.else_branch) {
            const std::int32_t end_jump = emit(Op::Jump, 0);
            patch_target(else_jump, here());
            compile_block(*branch.else_branch);
            patch_target(end_jump, here());
        } else {
            patch_target(else_jump, here());
        }
    }

    // Emits code that pushes exactly one value (unless the result is Void, for a
    // void host call used as a statement) and returns the value's static type.
    Type compile_expression(const ast::Expr& expr) {
        const Here position(*this, expr);
        if (const auto* lit = dynamic_cast<const ast::IntLiteral*>(&expr)) {
            emit(Op::PushConst, add_constant(Value::make_integer(lit->value)));
            return Type::Integer;
        }
        if (const auto* lit = dynamic_cast<const ast::StringLiteral*>(&expr)) {
            emit(Op::PushConst, add_constant(Value::make_string(lit->value)));
            return Type::String;
        }
        if (const auto* lit = dynamic_cast<const ast::FloatLiteral*>(&expr)) {
            emit(Op::PushConst, add_constant(Value::make_float(lit->value)));
            return Type::Float;
        }
        if (const auto* lit = dynamic_cast<const ast::VectorLiteral*>(&expr)) {
            // Component count already checked by the parser; three is a vector
            // and four a rotation, and there is no third possibility.
            const auto& parts = lit->components;
            if (parts.size() == 4) {
                emit(Op::PushConst, add_constant(
                    Value::make_rotation(parts[0], parts[1], parts[2], parts[3])));
                return Type::Rotation;
            }
            emit(Op::PushConst, add_constant(
                Value::make_vector(parts[0], parts[1], parts[2])));
            return Type::Vector;
        }
        if (const auto* lit = dynamic_cast<const ast::ListLiteral*>(&expr)) {
            for (const auto& element : lit->elements) {
                const Type element_type = compile_expression(*element);
                if (element_type == Type::List || element_type == Type::Void) {
                    throw ScriptError(std::string("a list cannot hold a ") +
                                      (element_type == Type::List ? "list" : "call with no value"));
                }
            }
            emit(Op::MakeList, static_cast<std::int32_t>(lit->elements.size()));
            return Type::List;
        }
        if (const auto* ref = dynamic_cast<const ast::VarRef*>(&expr)) {
            if (locals_.count(ref->name) == 0 && globals_.count(ref->name) == 0) {
                if (const Value* constant = lookup_constant(ref->name)) {
                    emit(Op::PushConst, add_constant(*constant));
                    return constant->type;
                }
            }
            const Symbol symbol = resolve(ref->name);
            emit_load(symbol);
            return symbol.type;
        }
        if (const auto* unary = dynamic_cast<const ast::Unary*>(&expr)) {
            if (unary->op == ast::UnaryOp::Not) {
                compile_condition(*unary->operand, "operand of '!'");
                emit(Op::NotInt);
                return Type::Integer;
            }
            const Type operand = compile_expression(*unary->operand);
            if (operand == Type::Float) {
                emit(Op::NegFloat);
                return Type::Float;
            }
            require_integer(operand, "operand of unary '-'");
            emit(Op::NegInt);
            return Type::Integer;
        }
        if (const auto* cast = dynamic_cast<const ast::Cast*>(&expr)) {
            return compile_cast(*cast);
        }
        if (const auto* binary = dynamic_cast<const ast::Binary*>(&expr)) {
            return compile_binary(*binary);
        }
        if (const auto* call = dynamic_cast<const ast::Call*>(&expr)) {
            return compile_call(*call);
        }
        throw ScriptError("internal: unknown expression");
    }

    Type compile_cast(const ast::Cast& cast) {
        const Type operand = compile_expression(*cast.operand);
        bool known = false;
        switch (cast.target) {
        case Type::String:
            known = is_textual(operand) || operand == Type::Integer || operand == Type::Float ||
                    operand == Type::List;
            if (operand == Type::Integer) emit(Op::CastIntToStr);
            if (operand == Type::Float) emit(Op::CastFloatToStr);
            if (operand == Type::List) emit(Op::CastListToStr);
            if (operand == Type::Key) emit(Op::ToStr);
            break;
        case Type::Key:
            known = is_textual(operand);
            if (operand == Type::String) emit(Op::ToKey);
            break;
        case Type::List:
            // Any single value becomes a list of one.
            known = operand != Type::Void;
            if (known && operand != Type::List) emit(Op::MakeList, 1);
            break;
        case Type::Integer:
            known = operand == Type::Integer || operand == Type::Float || operand == Type::String;
            if (operand == Type::Float) emit(Op::CastFloatToInt);
            if (operand == Type::String) emit(Op::CastStrToInt);
            break;
        case Type::Float:
            known = operand == Type::Integer || operand == Type::Float || operand == Type::String;
            if (operand == Type::Integer) emit(Op::IntToFloat);
            if (operand == Type::String) emit(Op::CastStrToFloat);
            break;
        default:
            break;
        }
        if (!known) {
            throw ScriptError(std::string("cannot cast ") + type_name(operand) + " to " +
                              type_name(cast.target));
        }
        return cast.target;
    }

    // An arithmetic or ordering operator on two numbers: the integer form when
    // both are integers, the float form when either is a float.
    std::optional<Type> numeric_pair(Type lhs, Type rhs) {
        if (lhs == Type::Integer && rhs == Type::Integer) return Type::Integer;
        if (is_numeric(lhs) && is_numeric(rhs)) return Type::Float;
        return std::nullopt;
    }

    Type compile_arithmetic(Type lhs, Type rhs, Op integer_op, Op float_op, const char* op) {
        const auto type = numeric_pair(lhs, rhs);
        if (!type) throw ScriptError(std::string("operator '") + op + "' requires numbers");
        emit(*type == Type::Integer ? integer_op : float_op);
        return *type;
    }

    // An ordering. The integer forms of <= and >= are the opposite test
    // negated; a float needs its own, because nothing orders against NaN.
    Type compile_ordering(Type lhs, Type rhs, ast::BinaryOp op, const char* spelling) {
        const auto type = numeric_pair(lhs, rhs);
        if (!type) throw ScriptError(std::string("operator '") + spelling + "' requires numbers");
        const bool integer = *type == Type::Integer;
        switch (op) {
        case ast::BinaryOp::Less: emit(integer ? Op::LessInt : Op::LessFloat); break;
        case ast::BinaryOp::Greater: emit(integer ? Op::GreaterInt : Op::GreaterFloat); break;
        case ast::BinaryOp::LessEqual:
            emit(integer ? Op::GreaterInt : Op::LessEqFloat);
            if (integer) emit(Op::NotInt);
            break;
        default:
            emit(integer ? Op::LessInt : Op::GreaterEqFloat);
            if (integer) emit(Op::NotInt);
            break;
        }
        return Type::Integer;
    }

    Type compile_binary(const ast::Binary& binary) {
        const Type lhs = compile_expression(*binary.lhs);
        const Type rhs = compile_expression(*binary.rhs);
        switch (binary.op) {
        case ast::BinaryOp::Add:
            // A list takes anything added to either end of it.
            if ((lhs == Type::List && rhs != Type::Void) || (rhs == Type::List && lhs != Type::Void)) {
                emit(Op::ConcatList);
                return Type::List;
            }
            if (is_textual(lhs) && is_textual(rhs)) {
                emit(Op::ConcatStr);
                return Type::String;
            }
            if (!numeric_pair(lhs, rhs)) {
                throw ScriptError("operator '+' requires two numbers or two strings");
            }
            return compile_arithmetic(lhs, rhs, Op::AddInt, Op::AddFloat, "+");
        case ast::BinaryOp::Sub:
            return compile_arithmetic(lhs, rhs, Op::SubInt, Op::SubFloat, "-");
        case ast::BinaryOp::Mul:
            return compile_arithmetic(lhs, rhs, Op::MulInt, Op::MulFloat, "*");
        case ast::BinaryOp::Div:
            return compile_arithmetic(lhs, rhs, Op::DivInt, Op::DivFloat, "/");
        case ast::BinaryOp::Mod:
            require_two_integers(lhs, rhs, "%");
            emit(Op::ModInt);
            return Type::Integer;
        case ast::BinaryOp::Less: return compile_ordering(lhs, rhs, binary.op, "<");
        case ast::BinaryOp::Greater: return compile_ordering(lhs, rhs, binary.op, ">");
        case ast::BinaryOp::LessEqual: return compile_ordering(lhs, rhs, binary.op, "<=");
        case ast::BinaryOp::GreaterEqual: return compile_ordering(lhs, rhs, binary.op, ">=");
        case ast::BinaryOp::Equal:
        case ast::BinaryOp::NotEqual: {
            const bool equal = binary.op == ast::BinaryOp::Equal;
            if (lhs == Type::List && rhs == Type::List) {
                emit(equal ? Op::EqualList : Op::NotEqualList);
                return Type::Integer;
            }
            if (is_textual(lhs) && is_textual(rhs)) {
                emit(equal ? Op::EqualStr : Op::NotEqualStr);
                return Type::Integer;
            }
            if (numeric_pair(lhs, rhs) == Type::Float) {
                emit(Op::EqualFloat);
                if (!equal) emit(Op::NotInt);
                return Type::Integer;
            }
            require_two_integers(lhs, rhs, equal ? "==" : "!=");
            emit(equal ? Op::EqualInt : Op::NotEqualInt);
            return Type::Integer;
        }
        case ast::BinaryOp::BitAnd:
            require_two_integers(lhs, rhs, "&");
            emit(Op::BitAnd);
            return Type::Integer;
        case ast::BinaryOp::BitOr:
            require_two_integers(lhs, rhs, "|");
            emit(Op::BitOr);
            return Type::Integer;
        case ast::BinaryOp::And:
            require_two_integers(lhs, rhs, "&&");
            emit(Op::LogicalAnd);
            return Type::Integer;
        case ast::BinaryOp::Or:
            require_two_integers(lhs, rhs, "||");
            emit(Op::LogicalOr);
            return Type::Integer;
        }
        throw ScriptError("internal: unknown binary operator");
    }

    Type compile_call(const ast::Call& call) {
        const HostSignature* signature = lookup_host(call.callee);
        if (signature == nullptr) {
            throw ScriptError("unknown function '" + call.callee + "'");
        }
        if (call.args.size() != signature->params.size()) {
            throw ScriptError("function '" + call.callee + "' expects " +
                              std::to_string(signature->params.size()) + " argument(s)");
        }
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            const Type arg_type = compile_expression(*call.args[i]);
            if (!convert(signature->params[i], arg_type)) {
                throw ScriptError("argument " + std::to_string(i + 1) + " to '" + call.callee +
                                  "' must be " + type_name(signature->params[i]));
            }
        }
        const auto argc = static_cast<std::int32_t>(call.args.size());
        if (const int builtin = find_builtin(call.callee); builtin >= 0) {
            emit(Op::CallBuiltin, builtin, argc);
        } else {
            emit(Op::CallHost, add_host(call.callee), argc);
            // A region answers a key as text; here it becomes a key.
            if (signature->result == Type::Key) emit(Op::ToKey);
        }
        return signature->result;
    }

    std::int32_t default_constant(Type type) {
        return add_constant(default_value(type));
    }

    static bool is_textual(Type type) { return type == Type::String || type == Type::Key; }
    static bool is_numeric(Type type) { return type == Type::Integer || type == Type::Float; }

    static void require_integer(Type type, const char* context) {
        if (type != Type::Integer) {
            throw ScriptError(std::string(context) + " must be an integer");
        }
    }

    static void require_two_integers(Type lhs, Type rhs, const char* op) {
        if (lhs != Type::Integer || rhs != Type::Integer) {
            throw ScriptError(std::string("operator '") + op + "' requires integers");
        }
    }

    Program program_;
    std::unordered_map<std::string, Symbol> globals_;
    std::unordered_map<std::string, Symbol> locals_;
    std::int32_t local_count_ = 0;
};

} // namespace

Program compile(const ast::Script& script) {
    Compiler compiler;
    return compiler.compile(script);
}

Program compile_source(const std::string& source) {
    return compile(parse(source));
}

} // namespace homeworldz::script
