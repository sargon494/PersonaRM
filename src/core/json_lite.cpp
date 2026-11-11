#include "json_lite.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace personarm {

namespace {

constexpr int kMaxDepth = 64;

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void EscapeString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

class Parser {
public:
    Parser(const std::string& text, std::string* error) : m_s(text), m_error(error) {}

    std::optional<JsonValue> ParseDocument() {
        SkipWs();
        auto v = ParseValue(0);
        if (!v) return std::nullopt;
        SkipWs();
        if (m_pos != m_s.size()) return Fail("trailing characters after document");
        return v;
    }

private:
    std::optional<JsonValue> Fail(const char* msg) {
        if (m_error && m_error->empty()) {
            *m_error = std::string(msg) + " at offset " + std::to_string(m_pos);
        }
        return std::nullopt;
    }

    void SkipWs() {
        while (m_pos < m_s.size()) {
            char c = m_s[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++m_pos; else break;
        }
    }

    bool Match(const char* lit) {
        size_t n = std::char_traits<char>::length(lit);
        if (m_s.compare(m_pos, n, lit) == 0) { m_pos += n; return true; }
        return false;
    }

    std::optional<JsonValue> ParseValue(int depth) {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        if (m_pos >= m_s.size()) return Fail("unexpected end of input");
        char c = m_s[m_pos];
        if (c == '{') return ParseObject(depth);
        if (c == '[') return ParseArray(depth);
        if (c == '"') {
            std::string str;
            if (!ParseString(str)) return std::nullopt;
            return JsonValue(std::move(str));
        }
        if (Match("true")) return JsonValue(true);
        if (Match("false")) return JsonValue(false);
        if (Match("null")) return JsonValue(nullptr);
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
        return Fail("unexpected character");
    }

    std::optional<JsonValue> ParseNumber() {
        size_t start = m_pos;
        if (m_s[m_pos] == '-') ++m_pos;
        while (m_pos < m_s.size() && std::string("0123456789.eE+-").find(m_s[m_pos]) != std::string::npos) ++m_pos;
        std::string tok = m_s.substr(start, m_pos - start);
        char* end = nullptr;
        double v = std::strtod(tok.c_str(), &end);
        if (tok.empty() || end != tok.c_str() + tok.size() || !std::isfinite(v)) {
            m_pos = start;
            return Fail("invalid number");
        }
        return JsonValue(v);
    }

    bool ParseHex4(uint32_t& out) {
        if (m_pos + 4 > m_s.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = m_s[m_pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool ParseString(std::string& out) {
        ++m_pos; // opening quote
        while (m_pos < m_s.size()) {
            unsigned char c = static_cast<unsigned char>(m_s[m_pos++]);
            if (c == '"') return true;
            if (c < 0x20) { Fail("control character in string"); return false; }
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (m_pos >= m_s.size()) break;
            char e = m_s[m_pos++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ParseHex4(cp)) { Fail("bad \\u escape"); return false; }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo = 0;
                        if (m_pos + 2 <= m_s.size() && m_s[m_pos] == '\\' && m_s[m_pos + 1] == 'u') {
                            m_pos += 2;
                            if (!ParseHex4(lo) || lo < 0xDC00 || lo > 0xDFFF) { Fail("bad surrogate pair"); return false; }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else { Fail("unpaired surrogate"); return false; }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: Fail("invalid escape"); return false;
            }
        }
        Fail("unterminated string");
        return false;
    }

    std::optional<JsonValue> ParseArray(int depth) {
        ++m_pos;
        JsonValue arr = JsonValue::MakeArray();
        SkipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == ']') { ++m_pos; return arr; }
        while (true) {
            SkipWs();
            auto v = ParseValue(depth + 1);
            if (!v) return std::nullopt;
            arr.Push(std::move(*v));
            SkipWs();
            if (m_pos >= m_s.size()) return Fail("unterminated array");
            char c = m_s[m_pos++];
            if (c == ']') return arr;
            if (c != ',') { --m_pos; return Fail("expected ',' or ']'"); }
        }
    }

    std::optional<JsonValue> ParseObject(int depth) {
        ++m_pos;
        JsonValue obj = JsonValue::MakeObject();
        SkipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == '}') { ++m_pos; return obj; }
        while (true) {
            SkipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != '"') return Fail("expected object key");
            std::string key;
            if (!ParseString(key)) return std::nullopt;
            SkipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != ':') return Fail("expected ':'");
            ++m_pos;
            SkipWs();
            auto v = ParseValue(depth + 1);
            if (!v) return std::nullopt;
            obj.Set(key, std::move(*v));
            SkipWs();
            if (m_pos >= m_s.size()) return Fail("unterminated object");
            char c = m_s[m_pos++];
            if (c == '}') return obj;
            if (c != ',') { --m_pos; return Fail("expected ',' or '}'"); }
        }
    }

    const std::string& m_s;
    std::string* m_error;
    size_t m_pos = 0;
};

} // namespace

JsonValue& JsonValue::Set(const std::string& key, JsonValue value) {
    if (m_type != Type::Object) { *this = MakeObject(); }
    return m_object[key] = std::move(value);
}

const JsonValue* JsonValue::Find(const std::string& key) const {
    if (m_type != Type::Object) return nullptr;
    auto it = m_object.find(key);
    return it == m_object.end() ? nullptr : &it->second;
}

double JsonValue::NumberOr(const std::string& key, double fallback) const {
    const JsonValue* v = Find(key);
    return v && v->IsNumber() ? v->m_number : fallback;
}

bool JsonValue::BoolOr(const std::string& key, bool fallback) const {
    const JsonValue* v = Find(key);
    return v && v->IsBool() ? v->m_bool : fallback;
}

std::string JsonValue::StringOr(const std::string& key, const std::string& fallback) const {
    const JsonValue* v = Find(key);
    return v && v->IsString() ? v->m_string : fallback;
}

JsonValue& JsonValue::Push(JsonValue value) {
    if (m_type != Type::Array) { *this = MakeArray(); }
    m_array.push_back(std::move(value));
    return m_array.back();
}

void JsonValue::DumpImpl(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(indent) * static_cast<size_t>(d), ' ');
    };
    switch (m_type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += m_bool ? "true" : "false"; break;
        case Type::Number: {
            char buf[40];
            if (m_number == std::floor(m_number) && std::fabs(m_number) < 1e15) {
                std::snprintf(buf, sizeof(buf), "%.0f", m_number);
            } else {
                std::snprintf(buf, sizeof(buf), "%.9g", m_number);
            }
            out += buf;
            break;
        }
        case Type::String: EscapeString(out, m_string); break;
        case Type::Array:
            if (m_array.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t i = 0; i < m_array.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                m_array[i].DumpImpl(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back(']');
            break;
        case Type::Object:
            if (m_object.empty()) { out += "{}"; break; }
            out.push_back('{');
            {
                bool first = true;
                for (const auto& kv : m_object) {
                    if (!first) out.push_back(',');
                    first = false;
                    newline(depth + 1);
                    EscapeString(out, kv.first);
                    out += indent < 0 ? ":" : ": ";
                    kv.second.DumpImpl(out, indent, depth + 1);
                }
            }
            newline(depth);
            out.push_back('}');
            break;
    }
}

std::string JsonValue::Dump(int indent) const {
    std::string out;
    DumpImpl(out, indent, 0);
    return out;
}

std::optional<JsonValue> JsonValue::Parse(const std::string& text, std::string* error) {
    if (error) error->clear();
    Parser p(text, error);
    return p.ParseDocument();
}

} // namespace personarm
