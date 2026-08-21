#include "homeworldz/llsd_xml.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>

namespace homeworldz::llsd {

namespace {

// Bounds sized for the largest legitimate body this parser will meet — a
// whole-model mesh upload — with headroom, not generosity. Exceeding any of
// them fails the parse.
constexpr std::size_t maximum_depth = 32;
constexpr std::size_t maximum_nodes = 262144;
constexpr std::size_t maximum_binary_bytes = 64u << 20;

struct Reader {
    std::string_view input;
    std::size_t at{};
    std::size_t nodes{};
    std::size_t binary_bytes{};

    void skip_space() {
        while (at < input.size() &&
               std::isspace(static_cast<unsigned char>(input[at])) != 0)
            ++at;
    }

    bool consume(std::string_view expected) {
        if (input.substr(at, expected.size()) != expected) return false;
        at += expected.size();
        return true;
    }

    // The tag name at the cursor, without consuming it. Empty when the cursor
    // is not at an opening tag.
    std::string_view peek_tag() {
        skip_space();
        if (at >= input.size() || input[at] != '<') return {};
        const auto end = input.find_first_of(" \t\r\n/>", at + 1);
        if (end == std::string_view::npos) return {};
        return input.substr(at + 1, end - (at + 1));
    }

    // Consume an opening tag with the given name, tolerating attributes
    // (only <binary> legitimately carries one). Reports whether the element
    // was self-closing, in which case it is fully consumed.
    bool open(std::string_view name, bool& self_closing) {
        skip_space();
        if (!consume("<") || !consume(name)) return false;
        self_closing = false;
        while (at < input.size()) {
            if (input[at] == '>') {
                ++at;
                return true;
            }
            if (input[at] == '/' && at + 1 < input.size() && input[at + 1] == '>') {
                at += 2;
                self_closing = true;
                return true;
            }
            ++at;
        }
        return false;
    }

    bool close(std::string_view name) {
        skip_space();
        return consume("</") && consume(name) && consume(">");
    }

    // The character content up to the element's closing tag, entities decoded.
    std::optional<std::string> content(std::string_view name) {
        const auto closing = "</" + std::string(name) + ">";
        const auto end = input.find(closing, at);
        if (end == std::string_view::npos) return std::nullopt;
        const auto raw = input.substr(at, end - at);
        at = end + closing.size();
        std::string decoded;
        decoded.reserve(raw.size());
        for (std::size_t index = 0; index < raw.size(); ++index) {
            if (raw[index] != '&') {
                decoded.push_back(raw[index]);
                continue;
            }
            const auto rest = raw.substr(index);
            if (rest.starts_with("&lt;")) { decoded.push_back('<'); index += 3; }
            else if (rest.starts_with("&gt;")) { decoded.push_back('>'); index += 3; }
            else if (rest.starts_with("&amp;")) { decoded.push_back('&'); index += 4; }
            else if (rest.starts_with("&quot;")) { decoded.push_back('"'); index += 5; }
            else if (rest.starts_with("&apos;")) { decoded.push_back('\''); index += 5; }
            else return std::nullopt;
        }
        return decoded;
    }
};


std::optional<Value> parse_value(Reader& reader, std::size_t depth);

std::optional<Value> parse_map(Reader& reader, std::size_t depth) {
    Value value;
    value.type = Value::Type::map;
    while (true) {
        const auto tag = reader.peek_tag();
        if (tag != "key") {
            if (!reader.close("map")) return std::nullopt;
            return value;
        }
        bool self_closing = false;
        if (!reader.open("key", self_closing)) return std::nullopt;
        std::string key;
        if (!self_closing) {
            auto text = reader.content("key");
            if (!text) return std::nullopt;
            key = std::move(*text);
        }
        auto member = parse_value(reader, depth);
        if (!member) return std::nullopt;
        value.members.emplace_back(std::move(key), std::move(*member));
    }
}

std::optional<Value> parse_array(Reader& reader, std::size_t depth) {
    Value value;
    value.type = Value::Type::array;
    while (true) {
        const auto tag = reader.peek_tag();
        if (tag.empty() || tag == "/array" || tag.starts_with("/")) {
            if (!reader.close("array")) return std::nullopt;
            return value;
        }
        auto element = parse_value(reader, depth);
        if (!element) return std::nullopt;
        value.elements.push_back(std::move(*element));
    }
}

std::optional<Value> parse_value(Reader& reader, std::size_t depth) {
    if (depth >= maximum_depth || ++reader.nodes > maximum_nodes) return std::nullopt;
    const auto tag = reader.peek_tag();
    if (tag.empty()) return std::nullopt;
    bool self_closing = false;
    if (tag == "map") {
        if (!reader.open("map", self_closing)) return std::nullopt;
        if (self_closing) {
            Value value;
            value.type = Value::Type::map;
            return value;
        }
        return parse_map(reader, depth + 1);
    }
    if (tag == "array") {
        if (!reader.open("array", self_closing)) return std::nullopt;
        if (self_closing) {
            Value value;
            value.type = Value::Type::array;
            return value;
        }
        return parse_array(reader, depth + 1);
    }
    if (tag != "undef" && tag != "boolean" && tag != "integer" && tag != "real" &&
        tag != "string" && tag != "uuid" && tag != "uri" && tag != "binary")
        return std::nullopt;
    if (!reader.open(tag, self_closing)) return std::nullopt;
    Value value;
    std::string text;
    if (!self_closing) {
        auto raw = reader.content(tag);
        if (!raw) return std::nullopt;
        text = std::move(*raw);
    }
    if (tag == "undef") {
        value.type = Value::Type::undefined;
    } else if (tag == "boolean") {
        value.type = Value::Type::boolean;
        value.boolean = text == "true" || text == "1";
    } else if (tag == "integer") {
        value.type = Value::Type::integer;
        if (!text.empty())
            std::from_chars(text.data(), text.data() + text.size(), value.integer);
    } else if (tag == "real") {
        value.type = Value::Type::real;
        if (!text.empty()) {
            // LLSD writes nan for unset reals; from_chars refuses it, and an
            // unset real is zero to every consumer here.
            if (text != "nan" && text != "-nan")
                std::from_chars(text.data(), text.data() + text.size(), value.real);
        }
    } else if (tag == "string") {
        value.type = Value::Type::string;
        value.text = std::move(text);
    } else if (tag == "uuid") {
        value.type = Value::Type::uuid;
        value.text = std::move(text);
    } else if (tag == "uri") {
        value.type = Value::Type::uri;
        value.text = std::move(text);
    } else { // binary; base64 is both the default and the only encoding here
        value.type = Value::Type::binary;
        auto decoded = decode_base64(text);
        if (!decoded) return std::nullopt;
        reader.binary_bytes += decoded->size();
        if (reader.binary_bytes > maximum_binary_bytes) return std::nullopt;
        value.binary = std::move(*decoded);
    }
    return value;
}

} // namespace

std::optional<std::vector<std::byte>> decode_base64(std::string_view text) {
    static constexpr auto table = [] {
        std::array<std::int8_t, 256> values{};
        values.fill(-1);
        const std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t index = 0; index < alphabet.size(); ++index)
            values[static_cast<unsigned char>(alphabet[index])] =
                static_cast<std::int8_t>(index);
        return values;
    }();
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char character : text) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) continue;
        if (character == '=') break;
        const auto value = table[static_cast<unsigned char>(character)];
        if (value < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xffu));
        }
    }
    return out;
}

const Value* Value::find(std::string_view key) const {
    if (type != Type::map) return nullptr;
    const auto found = std::find_if(members.begin(), members.end(),
        [&](const auto& member) { return member.first == key; });
    return found == members.end() ? nullptr : &found->second;
}

std::int64_t Value::as_integer(std::int64_t fallback) const {
    switch (type) {
    case Type::integer: return integer;
    case Type::real: return static_cast<std::int64_t>(real);
    case Type::boolean: return boolean ? 1 : 0;
    default: return fallback;
    }
}

double Value::as_real(double fallback) const {
    switch (type) {
    case Type::real: return real;
    case Type::integer: return static_cast<double>(integer);
    case Type::boolean: return boolean ? 1.0 : 0.0;
    default: return fallback;
    }
}

std::optional<Value> parse_xml(std::string_view xml) {
    Reader reader{xml};
    reader.skip_space();
    if (reader.consume("<?xml")) {
        const auto end = reader.input.find("?>", reader.at);
        if (end == std::string_view::npos) return std::nullopt;
        reader.at = end + 2;
    }
    bool self_closing = false;
    if (!reader.open("llsd", self_closing)) return std::nullopt;
    if (self_closing) return std::nullopt;
    auto value = parse_value(reader, 0);
    if (!value || !reader.close("llsd")) return std::nullopt;
    return value;
}

} // namespace homeworldz::llsd
