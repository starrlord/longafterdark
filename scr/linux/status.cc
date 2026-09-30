#include "status.h"

#include <algorithm>

namespace lad {

namespace {

// Strict unsigned decimal (or hex when `hex`): digits only, no sign, no
// overflow past 64 bits.
bool parse_u64(std::string_view s, uint64_t& out, bool hex = false) {
  if (s.empty() || s.size() > (hex ? 16u : 20u)) return false;
  uint64_t v = 0;
  for (char c : s) {
    unsigned d;
    if (c >= '0' && c <= '9') d = unsigned(c - '0');
    else if (hex && c >= 'a' && c <= 'f') d = unsigned(c - 'a' + 10);
    else if (hex && c >= 'A' && c <= 'F') d = unsigned(c - 'A' + 10);
    else return false;
    const uint64_t base = hex ? 16 : 10;
    if (v > (UINT64_MAX - d) / base) return false;
    v = v * base + d;
  }
  out = v;
  return true;
}

std::vector<std::string_view> split_ws(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

bool field(std::string_view tok, std::string_view key, std::string_view& value) {
  if (tok.size() <= key.size() || tok.substr(0, key.size()) != key) return false;
  value = tok.substr(key.size());
  return true;
}

std::vector<std::string> split_list(const std::string& v) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= v.size()) {
    size_t c = v.find(',', p);
    std::string item = v.substr(p, c == std::string::npos ? std::string::npos : c - p);
    if (!item.empty()) out.push_back(item);
    if (c == std::string::npos) break;
    p = c + 1;
  }
  return out;
}

}  // namespace

bool parse_status_line(std::string_view line, HostStatus& out) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  const auto t = split_ws(line);
  if (t.size() != 6 || t[0] != "STATUS") return false;
  HostStatus s;
  std::string_view v;
  uint64_t flags = 0, source = 0;
  if (!parse_u64(t[1], s.frames)) return false;
  if (!field(t[2], "flags=0x", v) || !parse_u64(v, flags, true) || flags > 0xFFFFFFFFu) return false;
  if (!field(t[3], "applied=", v) || !parse_u64(v, s.input_applied)) return false;
  if (!field(t[4], "eaten=", v) || !parse_u64(v, s.input_eaten)) return false;
  if (!field(t[5], "src=", v) || !parse_u64(v, source) || source > 0xFFFFFFFFu) return false;
  s.flags = uint32_t(flags);
  s.source = uint32_t(source);
  out = s;
  return true;
}

bool HostCapabilities::has_lane(const std::string& lane) const {
  return std::find(lanes.begin(), lanes.end(), lane) != lanes.end();
}

bool HostCapabilities::has_abi(const std::string& abi) const {
  return std::find(abis.begin(), abis.end(), abi.empty() ? std::string("afterdark") : abi) != abis.end();
}

bool HostCapabilities::runs(const std::string& lane, const std::string& abi) const {
  if (!known) return true;
  return (lane.empty() || has_lane(lane)) && has_abi(abi);
}

HostCapabilities parse_capabilities(const std::string& text) {
  HostCapabilities c;
  std::string line = text.substr(0, text.find_first_of("\r\n"));
  c.line = line;
  bool any = false;
  for (std::string_view tok : split_ws(line)) {
    size_t eq = tok.find('=');
    if (eq == std::string_view::npos) continue;
    std::string k(tok.substr(0, eq)), v(tok.substr(eq + 1));
    if (k == "lanes") {
      c.lanes = split_list(v);
      any = true;
    } else if (k == "abis") {
      c.abis = split_list(v);   // as listed: "abis=" alone runs no module ABI at all
    } else if (k == "status") {
      c.status = v == "1";
    } else if (k == "numlock") {
      c.numlock = v == "1";
    }
  }
  // An answer without "lanes=" is not an answer (a host too old to know the
  // switch prints its usage instead).
  c.known = any;
  if (!c.known) c = HostCapabilities{};
  c.line = line;
  return c;
}

}  // namespace lad
