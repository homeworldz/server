#include "homeworldz/script/vm.h"

#include "homeworldz/script/lexer.h" // ScriptError

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace homeworldz::script {
namespace {

// A runtime fault (stack underflow, bad index, host error). Caught inside run()
// so a faulting script stops without unwinding into the caller. This is C++
// error handling, not script state; the VM's own state stays in data.
struct VmFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// v3 records vectors and rotations as themselves; v2 wrote them as integers.
// v4 adds floats, the sleep left, and the host call in progress.
constexpr std::uint16_t kSnapshotVersion = 4;
constexpr unsigned char kMagic[4] = {'H', 'W', 'Z', 'S'};
constexpr std::uint32_t kMaxSnapshotValues = 1'000'000; // corrupt-input guard

// (string) of a float: six decimals, as Second Life writes it.
std::string float_to_string(float value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    char buffer[64]; // the largest float has 39 digits before the point
    const auto end = std::to_chars(buffer, buffer + sizeof buffer, static_cast<double>(value),
                                   std::chars_format::fixed, 6);
    return std::string(buffer, end.ptr);
}

// (integer) of a float: toward zero, and INT_MIN for anything an integer
// cannot hold, which is what the conversion gives on Second Life's hardware.
std::int32_t float_to_integer(float value) {
    if (!(value >= -2147483648.0f && value < 2147483648.0f)) return INT32_MIN; // NaN too
    return static_cast<std::int32_t>(value);
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// (integer) of a string: the decimal or 0x hexadecimal number it starts with
// after any spaces, 0 when there is none. A number past 32 bits is all ones.
std::int32_t string_to_integer(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size() && is_space(text[i])) ++i;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
    std::uint64_t value = 0;
    const auto add = [&value](std::uint64_t base, std::uint64_t digit) {
        value = value * base + digit;
        if (value > 0xffffffffULL) value = 0xffffffffULL;
    };
    if (i + 1 < text.size() && text[i] == '0' && lower(text[i + 1]) == 'x') {
        for (i += 2; i < text.size(); ++i) {
            const char c = lower(text[i]);
            if (is_digit(c)) add(16, static_cast<std::uint64_t>(c - '0'));
            else if (c >= 'a' && c <= 'f') add(16, static_cast<std::uint64_t>(c - 'a' + 10));
            else break;
        }
    } else {
        for (; i < text.size() && is_digit(text[i]); ++i) add(10, static_cast<std::uint64_t>(text[i] - '0'));
    }
    const auto bits = static_cast<std::uint32_t>(value);
    return static_cast<std::int32_t>(negative ? 0u - bits : bits);
}

// (float) of a string: the number it starts with after any spaces, with an
// optional exponent; "inf" and "nan" in any case; 0.0 when there is none.
float string_to_float(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size() && is_space(text[i])) ++i;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
    const auto starts = [&text, i](const char* word) {
        for (std::size_t k = 0; word[k] != '\0'; ++k) {
            if (i + k >= text.size() || lower(text[i + k]) != word[k]) return false;
        }
        return true;
    };
    float value = 0.0f;
    if (starts("inf")) {
        value = std::numeric_limits<float>::infinity();
    } else if (starts("nan")) {
        value = std::numeric_limits<float>::quiet_NaN();
    } else {
        std::size_t end = i;
        while (end < text.size() && is_digit(text[end])) ++end;
        if (end < text.size() && text[end] == '.') {
            ++end;
            while (end < text.size() && is_digit(text[end])) ++end;
        }
        if (end < text.size() && lower(text[end]) == 'e') {
            std::size_t exponent = end + 1;
            if (exponent < text.size() && (text[exponent] == '-' || text[exponent] == '+')) ++exponent;
            if (exponent < text.size() && is_digit(text[exponent])) {
                end = exponent;
                while (end < text.size() && is_digit(text[end])) ++end;
            }
        }
        double parsed = 0.0;
        // from_chars reads no further than a number goes, and reads nothing
        // from a lone "." — which leaves the 0.0 that is wanted.
        std::from_chars(text.data() + i, text.data() + end, parsed);
        value = static_cast<float>(parsed);
    }
    return negative ? -value : value;
}

class ByteWriter {
public:
    void u8(std::uint8_t v) { bytes_.push_back(v); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xff));
        u8(static_cast<std::uint8_t>((v >> 8) & 0xff));
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
        }
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
        }
    }
    void f64(double v) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        u64(bits);
    }
    void values(const std::vector<Value>& vs) {
        u32(static_cast<std::uint32_t>(vs.size()));
        for (const Value& v : vs) {
            value(v);
        }
    }
    std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    void value(const Value& v) {
        if (v.type == Type::String) {
            u8(2);
            u32(static_cast<std::uint32_t>(v.str.size()));
            for (char c : v.str) {
                u8(static_cast<std::uint8_t>(c));
            }
        } else if (v.type == Type::Float) {
            u8(5);
            f64(v.number());
        } else if (v.type == Type::Vector || v.type == Type::Rotation) {
            // A vector waiting on the operand stack for a host call is as much
            // script state as an integer is; written as one, it came back 0.
            u8(v.type == Type::Vector ? 3 : 4);
            for (double component : v.components) {
                std::uint64_t bits = 0;
                std::memcpy(&bits, &component, sizeof bits);
                u64(bits);
            }
        } else {
            u8(1);
            u32(static_cast<std::uint32_t>(v.integer));
        }
    }
    std::vector<std::uint8_t> bytes_;
};

class ByteReader {
public:
    explicit ByteReader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    std::uint8_t u8() {
        if (pos_ >= bytes_.size()) {
            throw ScriptError("snapshot truncated");
        }
        return bytes_[pos_++];
    }
    std::uint16_t u16() {
        std::uint16_t lo = u8();
        std::uint16_t hi = u8();
        return static_cast<std::uint16_t>(lo | (hi << 8));
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        }
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        return v;
    }
    double f64() {
        const std::uint64_t bits = u64();
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    std::vector<Value> values() {
        const std::uint32_t count = u32();
        if (count > kMaxSnapshotValues) {
            throw ScriptError("snapshot value count is implausibly large");
        }
        std::vector<Value> result;
        result.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            result.push_back(value());
        }
        return result;
    }

private:
    Value value() {
        const std::uint8_t tag = u8();
        if (tag == 2) {
            const std::uint32_t len = u32();
            if (len > kMaxSnapshotValues) {
                throw ScriptError("snapshot string is implausibly large");
            }
            std::string s;
            s.reserve(len);
            for (std::uint32_t i = 0; i < len; ++i) {
                s.push_back(static_cast<char>(u8()));
            }
            return Value::make_string(std::move(s));
        }
        if (tag == 1) {
            return Value::make_integer(static_cast<std::int32_t>(u32()));
        }
        if (tag == 3 || tag == 4) {
            std::array<double, 4> components{};
            for (double& component : components) {
                const std::uint64_t bits = u64();
                std::memcpy(&component, &bits, sizeof component);
            }
            Value result = tag == 3
                ? Value::make_vector(components[0], components[1], components[2])
                : Value::make_rotation(components[0], components[1], components[2], components[3]);
            return result;
        }
        if (tag == 5) {
            return Value::make_float(f64());
        }
        throw ScriptError("snapshot has an unknown value tag");
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t pos_ = 0;
};

} // namespace

VM::VM(const Program& program) : program_(program), globals_(program.global_defaults) {}

void VM::dispatch(const std::string& event, const std::vector<Value>& args) {
    const EventEntry* entry = program_.find_event(event);
    if (entry == nullptr) {
        throw ScriptError("no such event handler '" + event + "'");
    }
    if (args.size() != entry->param_types.size()) {
        throw ScriptError("event '" + event + "' expects " +
                          std::to_string(entry->param_types.size()) + " argument(s)");
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].type != entry->param_types[i]) {
            throw ScriptError("argument " + std::to_string(i + 1) + " to event '" + event +
                              "' has the wrong type");
        }
    }
    locals_.assign(static_cast<std::size_t>(entry->local_count), Value::make_integer(0));
    for (std::size_t i = 0; i < args.size(); ++i) {
        locals_[i] = args[i];
    }
    stack_.clear();
    ip_ = static_cast<std::size_t>(entry->entry_ip);
    finished_ = false;
    error_.clear();
}

RunStatus VM::run(std::uint64_t instruction_budget) {
    try {
        std::uint64_t used = 0;
        // A script asleep or waiting on a host call runs nothing until it is
        // woken or resumed, and stops the moment an instruction puts it there.
        while (!finished_ && !waiting_ && sleep_seconds_ <= 0.0 && used < instruction_budget) {
            step();
            ++used;
            ++total_;
        }
    } catch (const VmFault& fault) {
        error_ = fault.what();
        // step() has moved past the instruction that faulted.
        error_line_ = ip_ > 0 && ip_ - 1 < program_.lines.size() ? program_.lines[ip_ - 1] : 0;
        finished_ = true;
        return RunStatus::Error;
    }
    if (waiting_) return RunStatus::Waiting;
    if (sleep_seconds_ > 0.0) return RunStatus::Sleeping;
    return finished_ ? RunStatus::Finished : RunStatus::Yielded;
}

bool VM::resume(std::uint64_t token, std::optional<Value> value) {
    if (!waiting_ || token != waiting_token_) return false;
    waiting_ = false;
    waiting_token_ = 0;
    if (value) stack_.push_back(std::move(*value));
    return true;
}

void VM::step() {
    if (ip_ >= program_.code.size()) {
        throw VmFault("instruction pointer out of range");
    }
    const Instruction instr = program_.code[ip_];
    ++ip_;

    const auto pop = [this]() -> Value {
        if (stack_.empty()) {
            throw VmFault("operand stack underflow");
        }
        Value v = std::move(stack_.back());
        stack_.pop_back();
        return v;
    };
    const auto pop_int = [&pop]() -> std::int32_t {
        Value v = pop();
        if (v.type != Type::Integer) {
            throw VmFault("expected an integer on the stack");
        }
        return v.integer;
    };
    // An operand of a float operator: a float, or an integer to widen.
    const auto pop_number = [&pop]() -> float {
        Value v = pop();
        if (v.type == Type::Float) return v.real();
        if (v.type == Type::Integer) return static_cast<float>(v.integer);
        throw VmFault("expected a number on the stack");
    };
    const auto pop_float = [&pop]() -> float {
        Value v = pop();
        if (v.type != Type::Float) {
            throw VmFault("expected a float on the stack");
        }
        return v.real();
    };
    const auto pop_string = [&pop]() -> std::string {
        Value v = pop();
        if (v.type != Type::String) {
            throw VmFault("expected a string on the stack");
        }
        return std::move(v.str);
    };
    const auto slot_ref = [](std::vector<Value>& bank, std::int32_t slot,
                             const char* which) -> Value& {
        if (slot < 0 || static_cast<std::size_t>(slot) >= bank.size()) {
            throw VmFault(std::string(which) + " slot out of range");
        }
        return bank[static_cast<std::size_t>(slot)];
    };

    switch (instr.op) {
    case Op::PushConst: {
        if (instr.a < 0 || static_cast<std::size_t>(instr.a) >= program_.constants.size()) {
            throw VmFault("constant index out of range");
        }
        stack_.push_back(program_.constants[static_cast<std::size_t>(instr.a)]);
        break;
    }
    case Op::LoadLocal:
        stack_.push_back(slot_ref(locals_, instr.a, "local"));
        break;
    case Op::StoreLocal:
        slot_ref(locals_, instr.a, "local") = pop();
        break;
    case Op::LoadGlobal:
        stack_.push_back(slot_ref(globals_, instr.a, "global"));
        break;
    case Op::StoreGlobal:
        slot_ref(globals_, instr.a, "global") = pop();
        break;
    case Op::AddInt: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer(a + b));
        break;
    }
    case Op::SubInt: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer(a - b));
        break;
    }
    case Op::MulInt: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer(a * b));
        break;
    }
    case Op::ConcatStr: {
        Value b = pop();
        Value a = pop();
        if (a.type != Type::String || b.type != Type::String) {
            throw VmFault("expected strings for concatenation");
        }
        stack_.push_back(Value::make_string(a.str + b.str));
        break;
    }
    case Op::LessInt: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer(a < b ? 1 : 0));
        break;
    }
    case Op::GreaterInt: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer(a > b ? 1 : 0));
        break;
    }
    case Op::EqualInt: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer(a == b ? 1 : 0));
        break;
    }
    case Op::CastIntToStr: {
        std::int32_t v = pop_int();
        stack_.push_back(Value::make_string(std::to_string(v)));
        break;
    }
    case Op::Jump:
        ip_ = static_cast<std::size_t>(instr.a);
        break;
    case Op::JumpIfZero: {
        if (pop_int() == 0) {
            ip_ = static_cast<std::size_t>(instr.a);
        }
        break;
    }
    case Op::CallHost: {
        if (instr.a < 0 || static_cast<std::size_t>(instr.a) >= program_.host_names.size()) {
            throw VmFault("host index out of range");
        }
        const std::int32_t argc = instr.b;
        if (argc < 0 || static_cast<std::size_t>(argc) > stack_.size()) {
            throw VmFault("host call argument underflow");
        }
        std::vector<Value> args(static_cast<std::size_t>(argc));
        for (std::int32_t i = argc - 1; i >= 0; --i) {
            args[static_cast<std::size_t>(i)] = pop();
        }
        if (!host_) {
            throw VmFault("no host function boundary is installed");
        }
        HostResult result = host_(program_.host_names[static_cast<std::size_t>(instr.a)], args);
        if (result.kind == HostResult::Kind::Failed) {
            throw VmFault(result.error);
        }
        if (result.kind == HostResult::Kind::Suspended) {
            // The instruction is complete as far as the VM's own state goes:
            // the arguments are consumed, and the value arrives with resume().
            waiting_ = true;
            waiting_token_ = result.token;
            break;
        }
        if (result.value) {
            stack_.push_back(std::move(*result.value));
        }
        if (result.kind == HostResult::Kind::Sleep && result.sleep_seconds > 0.0) {
            sleep_seconds_ = result.sleep_seconds;
        }
        break;
    }
    case Op::IntToFloat:
        stack_.push_back(Value::make_float(static_cast<double>(pop_int())));
        break;
    case Op::DivInt:
    case Op::ModInt: {
        const std::int32_t b = pop_int();
        const std::int32_t a = pop_int();
        if (b == 0) throw VmFault("Math Error");
        // INT_MIN / -1 does not fit; LSL gives INT_MIN, and 0 for the remainder.
        if (b == -1) {
            stack_.push_back(Value::make_integer(
                instr.op == Op::DivInt ? static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(a)) : 0));
            break;
        }
        stack_.push_back(Value::make_integer(instr.op == Op::DivInt ? a / b : a % b));
        break;
    }
    case Op::AddFloat:
    case Op::SubFloat:
    case Op::MulFloat:
    case Op::DivFloat: {
        const float b = pop_number();
        const float a = pop_number();
        if (instr.op == Op::DivFloat && b == 0.0f) throw VmFault("Math Error");
        const float result = instr.op == Op::AddFloat   ? a + b
                             : instr.op == Op::SubFloat ? a - b
                             : instr.op == Op::MulFloat ? a * b
                                                        : a / b;
        stack_.push_back(Value::make_float(result));
        break;
    }
    case Op::NegFloat:
        stack_.push_back(Value::make_float(-pop_float()));
        break;
    case Op::LessFloat:
    case Op::GreaterFloat:
    case Op::LessEqFloat:
    case Op::GreaterEqFloat:
    case Op::EqualFloat: {
        const float b = pop_number();
        const float a = pop_number();
        const bool result = instr.op == Op::LessFloat        ? a < b
                            : instr.op == Op::GreaterFloat   ? a > b
                            : instr.op == Op::LessEqFloat    ? a <= b
                            : instr.op == Op::GreaterEqFloat ? a >= b
                                                             : a == b;
        stack_.push_back(Value::make_integer(result ? 1 : 0));
        break;
    }
    case Op::CastFloatToStr:
        stack_.push_back(Value::make_string(float_to_string(pop_float())));
        break;
    case Op::CastFloatToInt:
        stack_.push_back(Value::make_integer(float_to_integer(pop_float())));
        break;
    case Op::CastStrToInt:
        stack_.push_back(Value::make_integer(string_to_integer(pop_string())));
        break;
    case Op::CastStrToFloat:
        stack_.push_back(Value::make_float(string_to_float(pop_string())));
        break;
    case Op::Pop:
        (void)pop();
        break;
    case Op::Halt:
        finished_ = true;
        break;
    case Op::NotEqualInt: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer(a != b ? 1 : 0));
        break;
    }
    case Op::EqualStr:
    case Op::NotEqualStr: {
        Value b = pop();
        Value a = pop();
        if (a.type != Type::String || b.type != Type::String) {
            throw VmFault("string comparison requires strings");
        }
        const bool equal = a.str == b.str;
        stack_.push_back(Value::make_integer((instr.op == Op::EqualStr) == equal ? 1 : 0));
        break;
    }
    case Op::BitAnd: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer(a & b));
        break;
    }
    case Op::BitOr: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer(a | b));
        break;
    }
    case Op::LogicalAnd: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer((a != 0 && b != 0) ? 1 : 0));
        break;
    }
    case Op::LogicalOr: {
        std::int32_t b = pop_int();
        std::int32_t a = pop_int();
        stack_.push_back(Value::make_integer((a != 0 || b != 0) ? 1 : 0));
        break;
    }
    case Op::NegInt: {
        // Unsigned negation wraps as LSL's -(-2147483648) does, without the
        // undefined behaviour signed overflow would be in C++.
        const auto a = static_cast<std::uint32_t>(pop_int());
        stack_.push_back(Value::make_integer(static_cast<std::int32_t>(0u - a)));
        break;
    }
    case Op::NotInt:
        stack_.push_back(Value::make_integer(pop_int() == 0 ? 1 : 0));
        break;
    case Op::StrTruth: {
        Value a = pop();
        if (a.type != Type::String) {
            throw VmFault("expected a string on the stack");
        }
        stack_.push_back(Value::make_integer(a.str.empty() ? 0 : 1));
        break;
    }
    case Op::KeyTruth: {
        // A key is true when it is a well-formed uuid other than NULL_KEY.
        Value a = pop();
        if (a.type != Type::String) {
            throw VmFault("expected a key on the stack");
        }
        bool valid = a.str.size() == 36 && a.str != "00000000-0000-0000-0000-000000000000";
        for (std::size_t i = 0; valid && i < a.str.size(); ++i) {
            const char c = a.str[i];
            valid = (i == 8 || i == 13 || i == 18 || i == 23)
                        ? c == '-'
                        : std::isxdigit(static_cast<unsigned char>(c)) != 0;
        }
        stack_.push_back(Value::make_integer(valid ? 1 : 0));
        break;
    }
    }
}

std::vector<std::uint8_t> VM::snapshot() const {
    ByteWriter w;
    for (unsigned char c : kMagic) {
        w.u8(c);
    }
    w.u16(kSnapshotVersion);
    w.u32(program_.abi_version);
    w.u64(static_cast<std::uint64_t>(ip_));
    w.u8(finished_ ? 1 : 0);
    w.u64(total_);
    w.values(stack_);
    w.values(locals_);
    w.values(globals_);
    w.f64(sleep_seconds_);
    w.u8(waiting_ ? 1 : 0);
    w.u64(waiting_token_);
    return w.take();
}

void VM::restore(const std::vector<std::uint8_t>& bytes) {
    ByteReader r(bytes);
    for (unsigned char expected : kMagic) {
        if (r.u8() != expected) {
            throw ScriptError("snapshot has a bad magic header");
        }
    }
    if (r.u16() != kSnapshotVersion) {
        throw ScriptError("unsupported snapshot version");
    }
    if (r.u32() != program_.abi_version) {
        throw ScriptError("snapshot bytecode ABI does not match this program");
    }
    ip_ = static_cast<std::size_t>(r.u64());
    finished_ = r.u8() != 0;
    total_ = r.u64();
    stack_ = r.values();
    locals_ = r.values();
    std::vector<Value> globals = r.values();
    if (globals.size() != program_.global_defaults.size()) {
        throw ScriptError("snapshot global count does not match this program");
    }
    globals_ = std::move(globals);
    sleep_seconds_ = r.f64();
    waiting_ = r.u8() != 0;
    waiting_token_ = r.u64();
}

} // namespace homeworldz::script
