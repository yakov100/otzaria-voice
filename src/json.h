// A small JSON reader (the pack's config files and request bodies) and string
// escaping for responses.
#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace json {

class Value {
 public:
  enum Kind { Null, Bool, Number, String, Array, Object };

  Kind kind = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Value> a;
  std::map<std::string, Value> o;

  bool isNull() const { return kind == Null; }
  double num() const {
    if (kind != Number) throw std::runtime_error("json: expected a number");
    return n;
  }
  const std::string& str() const {
    if (kind != String) throw std::runtime_error("json: expected a string");
    return s;
  }
  const std::vector<Value>& arr() const {
    if (kind != Array) throw std::runtime_error("json: expected an array");
    return a;
  }
  bool has(const std::string& k) const { return kind == Object && o.count(k); }
  const Value& operator[](const std::string& k) const {
    static const Value none;
    if (kind != Object) return none;
    auto it = o.find(k);
    return it == o.end() ? none : it->second;
  }
};

namespace detail {
struct Parser {
  const std::string& t;
  size_t i = 0;

  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++;
  }
  [[noreturn]] void fail() { throw std::runtime_error("json: syntax error"); }
  void expect(char c) {
    ws();
    if (i >= t.size() || t[i] != c) fail();
    i++;
  }
  static void put(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  unsigned hex4() {
    if (i + 4 > t.size()) fail();
    unsigned v = std::stoul(t.substr(i, 4), nullptr, 16);
    i += 4;
    return v;
  }
  std::string string() {
    expect('"');
    std::string out;
    while (true) {
      if (i >= t.size()) fail();
      char c = t[i++];
      if (c == '"') return out;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= t.size()) fail();
      char e = t[i++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          unsigned cp = hex4();
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
            i += 2;
            unsigned lo = hex4();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          put(out, cp);
          break;
        }
        default: out += e;
      }
    }
  }
  Value value(int depth = 0) {
    if (depth > 64) fail();
    ws();
    if (i >= t.size()) fail();
    Value v;
    char c = t[i];
    if (c == '{') {
      v.kind = Value::Object;
      i++;
      ws();
      if (i < t.size() && t[i] == '}') {
        i++;
        return v;
      }
      while (true) {
        std::string k = string();
        expect(':');
        v.o[k] = value(depth + 1);
        ws();
        if (i < t.size() && t[i] == ',') {
          i++;
          continue;
        }
        expect('}');
        return v;
      }
    }
    if (c == '[') {
      v.kind = Value::Array;
      i++;
      ws();
      if (i < t.size() && t[i] == ']') {
        i++;
        return v;
      }
      while (true) {
        v.a.push_back(value(depth + 1));
        ws();
        if (i < t.size() && t[i] == ',') {
          i++;
          continue;
        }
        expect(']');
        return v;
      }
    }
    if (c == '"') {
      v.kind = Value::String;
      v.s = string();
      return v;
    }
    if (t.compare(i, 4, "true") == 0) {
      v.kind = Value::Bool;
      v.b = true;
      i += 4;
      return v;
    }
    if (t.compare(i, 5, "false") == 0) {
      v.kind = Value::Bool;
      i += 5;
      return v;
    }
    if (t.compare(i, 4, "null") == 0) {
      i += 4;
      return v;
    }
    size_t used = 0;
    v.kind = Value::Number;
    try {
      v.n = std::stod(t.substr(i, 32), &used);
    } catch (...) {
      fail();
    }
    i += used;
    return v;
  }
};
}  // namespace detail

inline Value parse(const std::string& text) {
  detail::Parser p{text};
  Value v = p.value();
  p.ws();
  if (p.i != text.size()) p.fail();
  return v;
}

inline std::string quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  return out + "\"";
}

}  // namespace json
