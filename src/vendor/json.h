#ifndef JIAMIAODB_JSON_H
#define JIAMIAODB_JSON_H

/* ═══════════════════════════════════════════════════════
   Minimal JSON — 轻量 JSON 解析/序列化

   无需外部依赖，支持 JiamiaoDB 所需的 JSON 操作。
   ═══════════════════════════════════════════════════════ */

#include <string>
#include <vector>
#include <map>
#include <variant>
#include <sstream>
#include <stdexcept>
#include <cstdint>
#include <cmath>

class Json {
public:
    using null_t = std::nullptr_t;
    using value_t = std::variant<null_t, bool, int64_t, double, std::string, std::vector<Json>, std::map<std::string, Json>>;

    enum Type { Null, Bool, Int, Float, String, Array, Object };

    Json() : value_(nullptr) {}
    Json(null_t) : value_(nullptr) {}
    Json(bool v) : value_(v) {}
    Json(int v) : value_((int64_t)v) {}
    Json(int64_t v) : value_(v) {}
    Json(double v) : value_(v) {}
    Json(const char* v) : value_(std::string(v)) {}
    Json(const std::string& v) : value_(v) {}
    Json(std::vector<Json> v) : value_(std::move(v)) {}
    Json(std::map<std::string, Json> v) : value_(std::move(v)) {}

    Type type() const { return (Type)value_.index(); }

    // Accessors
    bool is_null() const { return type() == Null; }
    bool is_bool() const { return type() == Bool; }
    bool is_int() const { return type() == Int; }
    bool is_float() const { return type() == Float; }
    bool is_string() const { return type() == String; }
    bool is_array() const { return type() == Array; }
    bool is_object() const { return type() == Object; }

    bool get_bool() const { return std::get<bool>(value_); }
    int64_t get_int() const { return std::get<int64_t>(value_); }
    double get_float() const { return std::get<double>(value_); }
    const std::string& get_string() const { return std::get<std::string>(value_); }

    // Array access
    const Json& operator[](size_t i) const {
        if (!is_array()) { static Json null_json; return null_json; }
        return std::get<std::vector<Json>>(value_)[i];
    }
    Json& operator[](size_t i) {
        if (!is_array()) value_ = std::vector<Json>();
        return std::get<std::vector<Json>>(value_)[i];
    }
    size_t size() const {
        if (is_array()) return std::get<std::vector<Json>>(value_).size();
        if (is_object()) return std::get<std::map<std::string, Json>>(value_).size();
        return 0;
    }
    bool empty() const {
        if (is_array()) return std::get<std::vector<Json>>(value_).empty();
        if (is_object()) return std::get<std::map<std::string, Json>>(value_).empty();
        return true;
    }

    // Object access
    bool contains(const std::string& k) const {
        if (!is_object()) return false;
        auto& m = std::get<std::map<std::string, Json>>(value_);
        return m.find(k) != m.end();
    }

    const Json& operator[](const std::string& k) const {
        if (!is_object()) { static Json null_json; return null_json; }
        auto& m = std::get<std::map<std::string, Json>>(value_);
        auto it = m.find(k);
        if (it == m.end()) { static Json null_json2; return null_json2; }
        return it->second;
    }

    Json& operator[](const std::string& k) {
        if (!is_object()) value_ = std::map<std::string, Json>();
        return std::get<std::map<std::string, Json>>(value_)[k];
    }

    // Value extraction with defaults
    std::string value(const std::string& k, const std::string& def) const {
        if (!contains(k)) return def;
        auto& v = (*this)[k];
        if (v.is_string()) return v.get_string();
        return def;
    }

    int64_t value(const std::string& k, int64_t def) const {
        if (!contains(k) || !(*this)[k].is_int()) return def;
        return (*this)[k].get_int();
    }

    double value(const std::string& k, double def) const {
        if (!contains(k)) return def;
        auto& v = (*this)[k];
        if (v.is_float()) return v.get_float();
        if (v.is_int()) return (double)v.get_int();
        return def;
    }

    bool value(const std::string& k, bool def) const {
        if (!contains(k) || !(*this)[k].is_bool()) return def;
        return (*this)[k].get_bool();
    }

    // Iteration (array)
    using iterator = std::vector<Json>::iterator;
    using const_iterator = std::vector<Json>::const_iterator;
    iterator begin() { if (!is_array()) value_ = std::vector<Json>(); return std::get<std::vector<Json>>(value_).begin(); }
    iterator end() { if (!is_array()) value_ = std::vector<Json>(); return std::get<std::vector<Json>>(value_).end(); }
    const_iterator begin() const { if (!is_array()) { static const std::vector<Json> empty; return empty.begin(); } return std::get<std::vector<Json>>(value_).begin(); }
    const_iterator end() const { if (!is_array()) { static const std::vector<Json> empty; return empty.end(); } return std::get<std::vector<Json>>(value_).end(); }

    // Object iteration
    using object_iterator = std::map<std::string, Json>::iterator;
    using const_object_iterator = std::map<std::string, Json>::const_iterator;
    object_iterator obj_begin() { if (!is_object()) value_ = std::map<std::string, Json>(); return std::get<std::map<std::string, Json>>(value_).begin(); }
    object_iterator obj_end() { if (!is_object()) value_ = std::map<std::string, Json>(); return std::get<std::map<std::string, Json>>(value_).end(); }
    const_object_iterator obj_begin() const { if (!is_object()) { static const std::map<std::string, Json> empty; return empty.begin(); } return std::get<std::map<std::string, Json>>(value_).begin(); }
    const_object_iterator obj_end() const { if (!is_object()) { static const std::map<std::string, Json> empty; return empty.end(); } return std::get<std::map<std::string, Json>>(value_).end(); }

    // Serialization
    std::string dump(int indent = 0) const {
        std::stringstream ss;
        serialize(ss, indent, 0);
        return ss.str();
    }

    // Parsing
    static Json parse(const std::string& s) {
        size_t pos = 0;
        return parse_value(s, pos);
    }

    // Array factory
    static Json array() { return Json(std::vector<Json>{}); }
    static Json object() { return Json(std::map<std::string, Json>{}); }

    void push_back(const Json& v) {
        if (!is_array()) value_ = std::vector<Json>();
        std::get<std::vector<Json>>(value_).push_back(v);
    }
    void push_back(Json&& v) {
        if (!is_array()) value_ = std::vector<Json>();
        std::get<std::vector<Json>>(value_).push_back(std::move(v));
    }

private:
    value_t value_;

    void serialize(std::stringstream& ss, int indent, int level) const {
        switch (type()) {
            case Null: ss << "null"; break;
            case Bool: ss << (get_bool() ? "true" : "false"); break;
            case Int: ss << get_int(); break;
            case Float: {
                double d = get_float();
                if (d == std::floor(d) && std::isfinite(d)) {
                    ss << (int64_t)d << ".0";
                } else {
                    ss << d;
                }
                break;
            }
            case String: {
                const auto& s = get_string();
                ss << '"';
                for (char c : s) {
                    if (c == '"') ss << "\\\"";
                    else if (c == '\\') ss << "\\\\";
                    else if (c == '\n') ss << "\\n";
                    else if (c == '\t') ss << "\\t";
                    else ss << c;
                }
                ss << '"';
                break;
            }
            case Array: {
                const auto& arr = std::get<std::vector<Json>>(value_);
                ss << '[';
                for (size_t i = 0; i < arr.size(); i++) {
                    if (i > 0) ss << ',';
                    if (indent) ss << '\n' << std::string((level + 1) * indent, ' ');
                    arr[i].serialize(ss, indent, level + 1);
                }
                if (indent && !arr.empty()) ss << '\n' << std::string(level * indent, ' ');
                ss << ']';
                break;
            }
            case Object: {
                const auto& obj = std::get<std::map<std::string, Json>>(value_);
                ss << '{';
                bool first = true;
                for (const auto& [k, v] : obj) {
                    if (!first) ss << ',';
                    if (indent) ss << '\n' << std::string((level + 1) * indent, ' ');
                    first = false;
                    ss << '"' << k << '"' << ':';
                    if (indent) ss << ' ';
                    v.serialize(ss, indent, level + 1);
                }
                if (indent && !obj.empty()) ss << '\n' << std::string(level * indent, ' ');
                ss << '}';
                break;
            }
        }
    }

    static Json parse_value(const std::string& s, size_t& pos) {
        skip_ws(s, pos);
        if (pos >= s.size()) throw std::runtime_error("Unexpected end of JSON");

        char c = s[pos];
        if (c == '"') return parse_string(s, pos);
        if (c == '{') return parse_object(s, pos);
        if (c == '[') return parse_array(s, pos);
        if (c == 't' || c == 'f') return parse_bool(s, pos);
        if (c == 'n') return parse_null(s, pos);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(s, pos);

        throw std::runtime_error(std::string("Unexpected char: ") + c);
    }

    static void skip_ws(const std::string& s, size_t& pos) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) pos++;
    }

    static Json parse_string(const std::string& s, size_t& pos) {
        pos++; // skip "
        std::string result;
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\') {
                pos++;
                if (pos >= s.size()) break;
                switch (s[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case 'r': result += '\r'; break;
                    default: result += s[pos];
                }
            } else {
                result += s[pos];
            }
            pos++;
        }
        if (pos < s.size()) pos++; // skip closing "
        return Json(result);
    }

    static Json parse_number(const std::string& s, size_t& pos) {
        size_t start = pos;
        bool is_float = false;
        if (s[pos] == '-') pos++;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        if (pos < s.size() && s[pos] == '.') {
            is_float = true;
            pos++;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            is_float = true;
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        }

        std::string num = s.substr(start, pos - start);
        if (is_float) return Json(std::stod(num));
        return Json((int64_t)std::stoll(num));
    }

    static Json parse_bool(const std::string& s, size_t& pos) {
        if (s.substr(pos, 4) == "true") { pos += 4; return Json(true); }
        if (s.substr(pos, 5) == "false") { pos += 5; return Json(false); }
        throw std::runtime_error("Invalid bool");
    }

    static Json parse_null(const std::string& s, size_t& pos) {
        if (s.substr(pos, 4) == "null") { pos += 4; return Json(nullptr); }
        throw std::runtime_error("Invalid null");
    }

    static Json parse_object(const std::string& s, size_t& pos) {
        pos++; // skip {
        Json obj = Json::object();
        skip_ws(s, pos);

        if (pos < s.size() && s[pos] == '}') { pos++; return obj; }

        while (true) {
            skip_ws(s, pos);
            if (pos >= s.size()) throw std::runtime_error("Unterminated object");
            if (s[pos] == '}') { pos++; break; }
            if (s[pos] != '"') throw std::runtime_error("Expected string key");

            std::string key = parse_string(s, pos).get_string();
            skip_ws(s, pos);
            if (pos >= s.size() || s[pos] != ':') throw std::runtime_error("Expected :");
            pos++; // skip :
            skip_ws(s, pos);

            obj[key] = parse_value(s, pos);
            skip_ws(s, pos);

            if (pos < s.size() && s[pos] == ',') { pos++; continue; }
            if (pos < s.size() && s[pos] == '}') { pos++; break; }
            break;
        }
        return obj;
    }

    static Json parse_array(const std::string& s, size_t& pos) {
        pos++; // skip [
        Json arr = Json::array();
        skip_ws(s, pos);

        if (pos < s.size() && s[pos] == ']') { pos++; return arr; }

        while (true) {
            arr.push_back(parse_value(s, pos));
            skip_ws(s, pos);
            if (pos < s.size() && s[pos] == ',') { pos++; continue; }
            if (pos < s.size() && s[pos] == ']') { pos++; break; }
            break;
        }
        return arr;
    }
};

#endif // JIAMIAODB_JSON_H
