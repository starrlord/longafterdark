// A small JSON reader (internal): enough to read back the importer's own
// import.json records (both versions) when it lists, catalogues or recovers
// installed packages. The library does not depend on phosg (the tests use
// phosg to check the importer's output independently), so this is its own.
// It also holds json_escape, the string escaping those records are written
// with. Standard C++ only (no Win32): the Linux player (scr/linux) compiles
// minijson.cc with g++ to read the catalog.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adw::import {

struct JsonValue {
  enum class Kind { null, boolean, number, string, array, object } kind = Kind::null;
  bool boolean = false;
  double number = 0;
  std::string string;  // UTF-8
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;

  // Object member by key (nullptr when absent or not an object).
  const JsonValue* get(std::string_view key) const;
  // Convenience: a string member, or `dflt`.
  std::string str(std::string_view key, std::string dflt = {}) const;
  // An integral number member, or `dflt`. A number that is not finite or
  // lies outside int64_t's range (a hand-edited 1e300) is `dflt` too: the
  // conversion would be undefined. A fraction is truncated toward zero.
  int64_t integer(std::string_view key, int64_t dflt = 0) const;
  // The same for this value itself, and for fields narrowed to int: `dflt`
  // unless the number fits in int.
  int64_t as_int64(int64_t dflt = 0) const;
  int as_int(int dflt = 0) const;
  int int_in(std::string_view key, int dflt = 0) const;
};

// nullopt when `text` is not one complete JSON value (trailing whitespace
// allowed). Nesting deeper than 64 levels is refused.
std::optional<JsonValue> parse_json(std::string_view text);

// The inside of a JSON string for `s` (import.json's): '"', '\\' and the C0
// controls escaped, everything else as it is. The result is UTF-8 whatever
// `s` holds: a byte that starts no well-formed UTF-8 sequence is written as
// U+FFFD (utf8.h to_valid_utf8), so a record always reads as UTF-8 JSON.
std::string json_escape(std::string_view s);

}  // namespace adw::import
