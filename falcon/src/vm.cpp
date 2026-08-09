#include "homeworldz/script/vm.h"

#include "homeworldz/script/lexer.h" // ScriptError

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

constexpr std::uint16_t kSnapshotVersion = 2;
constexpr unsigned char kMagic[4] = {'H', 'W', 'Z', 'S'};
constexpr std::uint32_t kMaxSnapshotValues = 1'000'000; // corrupt-input guard

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
        while (!finished_ && used < instruction_budget) {
            step();
            ++used;
            ++total_;
        }
    } catch (const VmFault& fault) {
        error_ = fault.what();
        finished_ = true;
        return RunStatus::Error;
    }
    return finished_ ? RunStatus::Finished : RunStatus::Yielded;
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
        std::optional<Value> result =
            host_(program_.host_names[static_cast<std::size_t>(instr.a)], args);
        if (result) {
            stack_.push_back(std::move(*result));
        }
        break;
    }
    case Op::Pop:
        (void)pop();
        break;
    case Op::Halt:
        finished_ = true;
        break;
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
}

} // namespace homeworldz::script
