#include "minijson.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "utf8.h"

namespace adw::import {

const JsonValue* JsonValue::get(std::string_view key) const {
  if (kind != Kind::object) return nullptr;
  for (const auto& [k, v] : object)
    if (k == key) return &v;
  return nullptr;
}

std::string JsonValue::str(std::string_view key, std::string dflt) const {
  const JsonValue* v = get(key);
  return v && v->kind == Kind::string ? v->string : dflt;
}

int64_t JsonValue::as_int64(int64_t dflt) const {
  if (kind != Kind::number || !std::isfinite(number)) return dflt;
  // [-2^63, 2^63): every double in it converts; 2^63 itself would not.
  if (number < -9223372036854775808.0 || number >= 9223372036854775808.0) return dflt;
  return int64_t(number);
}

int JsonValue::as_int(int dflt) const {
  const int64_t v = as_int64(int64_t(INT64_MIN));
  if (v == INT64_MIN || v < INT_MIN || v > INT_MAX) return dflt;
  return int(v);
}

int64_t JsonValue::integer(std::string_view key, int64_t dflt) const {
  const JsonValue* v = get(key);
  return v ? v->as_int64(dflt) : dflt;
}

int JsonValue::int_in(std::string_view key, int dflt) const {
  const JsonValue* v = get(key);
  return v ? v->as_int(dflt) : dflt;
}

namespace {

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  bool parse(JsonValue& out) {
    ws();
    if (!value(out, 0)) return false;
    ws();
    return i_ == s_.size();
  }

 private:
  std::string_view s_;
  size_t i_ = 0;

  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++;
  }
  bool lit(std::string_view w) {
    if (s_.substr(i_, w.size()) != w) return false;
    i_ += w.size();
    return true;
  }

  static void utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) {
      o += char(cp);
    } else if (cp < 0x800) {
      o += char(0xC0 | (cp >> 6));
      o += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      o += char(0xE0 | (cp >> 12));
      o += char(0x80 | ((cp >> 6) & 0x3F));
      o += char(0x80 | (cp & 0x3F));
    } else {
      o += char(0xF0 | (cp >> 18));
      o += char(0x80 | ((cp >> 12) & 0x3F));
      o += char(0x80 | ((cp >> 6) & 0x3F));
      o += char(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(uint32_t& v) {
    if (i_ + 4 > s_.size()) return false;
    v = 0;
    for (int k = 0; k < 4; k++) {
      char c = s_[i_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool string(std::string& o) {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    i_++;
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (uint8_t(c) < 0x20) return false;
      if (c != '\\') {
        o += c;
        continue;
      }
      if (i_ >= s_.size()) return false;
      char e = s_[i_++];
      switch (e) {
        case '"': o += '"'; break;
        case '\\': o += '\\'; break;
        case '/': o += '/'; break;
        case 'b': o += '\b'; break;
        case 'f': o += '\f'; break;
        case 'n': o += '\n'; break;
        case 'r': o += '\r'; break;
        case 't': o += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && s_.substr(i_, 2) == "\\u") {
            i_ += 2;
            uint32_t lo;
            if (!hex4(lo) || lo < 0xDC00 || lo >= 0xE000) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(o, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool number(double& v) {
    size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') i_++;
    bool digits = false;
    while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' ||
                              s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
      digits = true;
      i_++;
    }
    if (!digits) return false;
    std::string t(s_.substr(start, i_ - start));
    char* end = nullptr;
    v = strtod(t.c_str(), &end);
    return end && *end == '\0';
  }

  bool value(JsonValue& out, int depth) {
    if (depth > 64 || i_ >= s_.size()) return false;
    char c = s_[i_];
    if (c == '{') {
      out.kind = JsonValue::Kind::object;
      i_++;
      ws();
      if (i_ < s_.size() && s_[i_] == '}') return i_++, true;
      for (;;) {
        ws();
        std::string key;
        if (!string(key)) return false;
        ws();
        if (i_ >= s_.size() || s_[i_] != ':') return false;
        i_++;
        ws();
        JsonValue v;
        if (!value(v, depth + 1)) return false;
        out.object.emplace_back(std::move(key), std::move(v));
        ws();
        if (i_ < s_.size() && s_[i_] == ',') {
          i_++;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == '}') return i_++, true;
        return false;
      }
    }
    if (c == '[') {
      out.kind = JsonValue::Kind::array;
      i_++;
      ws();
      if (i_ < s_.size() && s_[i_] == ']') return i_++, true;
      for (;;) {
        ws();
        JsonValue v;
        if (!value(v, depth + 1)) return false;
        out.array.push_back(std::move(v));
        ws();
        if (i_ < s_.size() && s_[i_] == ',') {
          i_++;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == ']') return i_++, true;
        return false;
      }
    }
    if (c == '"') {
      out.kind = JsonValue::Kind::string;
      return string(out.string);
    }
    if (lit("true")) return out.kind = JsonValue::Kind::boolean, out.boolean = true, true;
    if (lit("false")) return out.kind = JsonValue::Kind::boolean, out.boolean = false, true;
    if (lit("null")) return out.kind = JsonValue::Kind::null, true;
    out.kind = JsonValue::Kind::number;
    return number(out.number);
  }
};

}  // namespace

std::optional<JsonValue> parse_json(std::string_view text) {
  JsonValue v;
  Parser p(text);
  if (!p.parse(v)) return std::nullopt;
  return v;
}

std::string json_escape(std::string_view s) {
  std::string o;
  for (unsigned char c : to_valid_utf8(s)) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += char(c);
        }
    }
  }
  return o;
}

}  // namespace adw::import
