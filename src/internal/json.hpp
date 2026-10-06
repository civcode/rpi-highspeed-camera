#pragma once

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hscam::internal::json {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double,
                                 std::string, Array, Object>;

    Value() : value_(nullptr) {}
    Value(std::nullptr_t) : value_(nullptr) {}
    Value(bool v) : value_(v) {}
    Value(double v) : value_(v) {}
    Value(std::int64_t v) : value_(v) {}
    Value(std::uint64_t v) : value_(v) {}
    Value(int v) : value_(static_cast<std::int64_t>(v)) {}
    Value(unsigned v) : value_(static_cast<std::uint64_t>(v)) {}
    Value(std::string v) : value_(std::move(v)) {}
    Value(const char *v) : value_(std::string(v)) {}
    Value(Array v) : value_(std::move(v)) {}
    Value(Object v) : value_(std::move(v)) {}

    [[nodiscard]] bool isNull() const { return std::holds_alternative<std::nullptr_t>(value_); }
    [[nodiscard]] bool isBool() const { return std::holds_alternative<bool>(value_); }
    [[nodiscard]] bool isNumber() const {
        return std::holds_alternative<std::int64_t>(value_) ||
               std::holds_alternative<std::uint64_t>(value_) ||
               std::holds_alternative<double>(value_);
    }
    [[nodiscard]] bool isString() const { return std::holds_alternative<std::string>(value_); }
    [[nodiscard]] bool isArray() const { return std::holds_alternative<Array>(value_); }
    [[nodiscard]] bool isObject() const { return std::holds_alternative<Object>(value_); }

    [[nodiscard]] bool asBool() const { return std::get<bool>(value_); }

    [[nodiscard]] double asNumber() const
    {
        if (auto p = std::get_if<double>(&value_)) return *p;
        if (auto p = std::get_if<std::int64_t>(&value_)) return static_cast<double>(*p);
        if (auto p = std::get_if<std::uint64_t>(&value_)) return static_cast<double>(*p);
        throw std::bad_variant_access();
    }

    [[nodiscard]] std::int64_t asInt64() const
    {
        if (auto p = std::get_if<std::int64_t>(&value_)) return *p;
        if (auto p = std::get_if<std::uint64_t>(&value_)) {
            if (*p > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw std::out_of_range("JSON unsigned integer does not fit int64");
            return static_cast<std::int64_t>(*p);
        }
        if (auto p = std::get_if<double>(&value_)) {
            if (!std::isfinite(*p) || std::trunc(*p) != *p ||
                *p < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
                *p > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                throw std::out_of_range("JSON number does not fit int64 exactly");
            return static_cast<std::int64_t>(*p);
        }
        throw std::bad_variant_access();
    }

    [[nodiscard]] std::uint64_t asUInt64() const
    {
        if (auto p = std::get_if<std::uint64_t>(&value_)) return *p;
        if (auto p = std::get_if<std::int64_t>(&value_)) {
            if (*p < 0) throw std::out_of_range("negative JSON integer does not fit uint64");
            return static_cast<std::uint64_t>(*p);
        }
        if (auto p = std::get_if<double>(&value_)) {
            if (!std::isfinite(*p) || std::trunc(*p) != *p || *p < 0.0 ||
                *p > static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
                throw std::out_of_range("JSON number does not fit uint64 exactly");
            return static_cast<std::uint64_t>(*p);
        }
        throw std::bad_variant_access();
    }

    [[nodiscard]] const std::string &asString() const { return std::get<std::string>(value_); }
    [[nodiscard]] const Array &asArray() const { return std::get<Array>(value_); }
    [[nodiscard]] const Object &asObject() const { return std::get<Object>(value_); }
    [[nodiscard]] Array &asArray() { return std::get<Array>(value_); }
    [[nodiscard]] Object &asObject() { return std::get<Object>(value_); }

    [[nodiscard]] const Value &at(std::string_view key) const
    {
        const auto &obj = asObject();
        auto it = obj.find(std::string(key));
        if (it == obj.end())
            throw std::runtime_error("missing JSON key: " + std::string(key));
        return it->second;
    }

    [[nodiscard]] const Value *find(std::string_view key) const
    {
        if (!isObject()) return nullptr;
        const auto &obj = asObject();
        auto it = obj.find(std::string(key));
        return it == obj.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const Storage &storage() const noexcept { return value_; }

private:
    Storage value_;
};

inline std::string escape(std::string_view input)
{
    std::ostringstream out;
    for (unsigned char ch : input) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(ch) << std::dec;
            else
                out << static_cast<char>(ch);
        }
    }
    return out.str();
}

inline void stringifyInto(const Value &value, std::ostream &out, int indent, int depth)
{
    const auto pad = [&](int extra = 0) {
        if (indent > 0) out << std::string((depth + extra) * indent, ' ');
    };

    if (value.isNull()) out << "null";
    else if (value.isBool()) out << (value.asBool() ? "true" : "false");
    else if (auto p = std::get_if<std::int64_t>(&value.storage())) out << *p;
    else if (auto p = std::get_if<std::uint64_t>(&value.storage())) out << *p;
    else if (auto p = std::get_if<double>(&value.storage())) out << std::setprecision(17) << *p;
    else if (value.isString()) out << '"' << escape(value.asString()) << '"';
    else if (value.isArray()) {
        const auto &array = value.asArray();
        out << '[';
        if (!array.empty() && indent > 0) out << '\n';
        for (std::size_t i = 0; i < array.size(); ++i) {
            if (indent > 0) pad(1);
            stringifyInto(array[i], out, indent, depth + 1);
            if (i + 1 != array.size()) out << ',';
            if (indent > 0) out << '\n';
        }
        if (!array.empty() && indent > 0) pad();
        out << ']';
    } else {
        const auto &object = value.asObject();
        out << '{';
        if (!object.empty() && indent > 0) out << '\n';
        std::size_t i = 0;
        for (const auto &[key, item] : object) {
            if (indent > 0) pad(1);
            out << '"' << escape(key) << "\":";
            if (indent > 0) out << ' ';
            stringifyInto(item, out, indent, depth + 1);
            if (++i != object.size()) out << ',';
            if (indent > 0) out << '\n';
        }
        if (!object.empty() && indent > 0) pad();
        out << '}';
    }
}

inline std::string stringify(const Value &value, int indent = 0)
{
    std::ostringstream out;
    stringifyInto(value, out, indent, 0);
    return out.str();
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value parse()
    {
        skip();
        Value result = parseValue();
        skip();
        if (pos_ != text_.size()) fail("trailing characters");
        return result;
    }

private:
    [[noreturn]] void fail(const std::string &message) const
    {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(pos_) + ": " + message);
    }

    void skip()
    {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    bool consume(char ch)
    {
        skip();
        if (pos_ < text_.size() && text_[pos_] == ch) { ++pos_; return true; }
        return false;
    }

    Value parseValue()
    {
        skip();
        if (pos_ >= text_.size()) fail("unexpected end of input");
        const char ch = text_[pos_];
        if (ch == '"') return Value(parseString());
        if (ch == '{') return parseObject();
        if (ch == '[') return parseArray();
        if (ch == 't') return parseLiteral("true", Value(true));
        if (ch == 'f') return parseLiteral("false", Value(false));
        if (ch == 'n') return parseLiteral("null", Value(nullptr));
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) return parseNumber();
        fail("unexpected token");
    }

    Value parseLiteral(std::string_view literal, Value value)
    {
        if (text_.substr(pos_, literal.size()) != literal) fail("invalid literal");
        pos_ += literal.size();
        return value;
    }

    std::string parseString()
    {
        if (!consume('"')) fail("expected string");
        std::string out;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') return out;
            if (ch != '\\') { out.push_back(ch); continue; }
            if (pos_ >= text_.size()) fail("bad escape");
            const char esc = text_[pos_++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: fail("unsupported escape");
            }
        }
        fail("unterminated string");
    }

    Value parseNumber()
    {
        const std::size_t begin = pos_;
        bool floating = false;
        if (text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_])))
            fail("invalid number");
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            floating = true;
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            floating = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }

        const auto token = text_.substr(begin, pos_ - begin);
        if (floating) {
            try { return Value(std::stod(std::string(token))); }
            catch (...) { fail("invalid floating point number"); }
        }

        if (!token.empty() && token.front() == '-') {
            std::int64_t value{};
            const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (ec != std::errc{} || ptr != token.data() + token.size())
                fail("signed integer out of range");
            return Value(value);
        }

        std::uint64_t value{};
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (ec != std::errc{} || ptr != token.data() + token.size())
            fail("unsigned integer out of range");
        return Value(value);
    }

    Value parseArray()
    {
        consume('[');
        Value::Array array;
        if (consume(']')) return Value(std::move(array));
        for (;;) {
            array.push_back(parseValue());
            if (consume(']')) break;
            if (!consume(',')) fail("expected ',' in array");
        }
        return Value(std::move(array));
    }

    Value parseObject()
    {
        consume('{');
        Value::Object object;
        if (consume('}')) return Value(std::move(object));
        for (;;) {
            skip();
            if (pos_ >= text_.size() || text_[pos_] != '"') fail("expected object key");
            std::string key = parseString();
            if (!consume(':')) fail("expected ':'");
            object.emplace(std::move(key), parseValue());
            if (consume('}')) break;
            if (!consume(',')) fail("expected ',' in object");
        }
        return Value(std::move(object));
    }

    std::string_view text_;
    std::size_t pos_{};
};

inline Value parse(std::string_view text) { return Parser(text).parse(); }

} // namespace hscam::internal::json
