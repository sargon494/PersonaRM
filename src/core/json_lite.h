#pragma once

// Minimal dependency-free JSON value, parser and writer (RFC 8259 subset).
// Used for personarm_config.json (settings, input profiles, texture overrides).
// Limitations: numbers are stored as double; \u escapes are decoded to UTF-8
// (surrogate pairs supported); nesting depth is capped to avoid stack abuse.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace personarm {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>; // sorted keys -> stable output

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool b) : m_type(Type::Bool), m_bool(b) {}
    JsonValue(int v) : m_type(Type::Number), m_number(v) {}
    JsonValue(uint32_t v) : m_type(Type::Number), m_number(v) {}
    JsonValue(double v) : m_type(Type::Number), m_number(v) {}
    JsonValue(const char* s) : m_type(Type::String), m_string(s) {}
    JsonValue(std::string s) : m_type(Type::String), m_string(std::move(s)) {}

    static JsonValue MakeArray() { JsonValue v; v.m_type = Type::Array; return v; }
    static JsonValue MakeObject() { JsonValue v; v.m_type = Type::Object; return v; }

    Type GetType() const { return m_type; }
    bool IsNull() const { return m_type == Type::Null; }
    bool IsBool() const { return m_type == Type::Bool; }
    bool IsNumber() const { return m_type == Type::Number; }
    bool IsString() const { return m_type == Type::String; }
    bool IsArray() const { return m_type == Type::Array; }
    bool IsObject() const { return m_type == Type::Object; }

    bool AsBool(bool fallback = false) const { return IsBool() ? m_bool : fallback; }
    double AsNumber(double fallback = 0.0) const { return IsNumber() ? m_number : fallback; }
    const std::string& AsString() const { return m_string; }
    const Array& Items() const { return m_array; }
    const Object& Members() const { return m_object; }

    // Object helpers
    JsonValue& Set(const std::string& key, JsonValue value);
    const JsonValue* Find(const std::string& key) const;
    bool Has(const std::string& key) const { return Find(key) != nullptr; }
    double NumberOr(const std::string& key, double fallback) const;
    bool BoolOr(const std::string& key, bool fallback) const;
    std::string StringOr(const std::string& key, const std::string& fallback) const;

    // Array helper
    JsonValue& Push(JsonValue value);

    /// Serialize. indent < 0 => compact output.
    std::string Dump(int indent = 2) const;

    /// Parse a complete document. Returns nullopt (and fills error) on malformed input.
    static std::optional<JsonValue> Parse(const std::string& text, std::string* error = nullptr);

private:
    void DumpImpl(std::string& out, int indent, int depth) const;

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    Array m_array;
    Object m_object;
};

} // namespace personarm
