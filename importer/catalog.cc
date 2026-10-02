#include "catalog.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>

#include <map>

#include "importer.h"
#include "loader/image.hh"
#include "loader/ne.hh"
#include "loader/pe.hh"
#include "md5.h"
#include "winutil.h"

namespace adw::import {

namespace fs = std::filesystem;
using loader::ResId;

// ---- text ------------------------------------------------------------------------

namespace {

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Windows-1252 0x80..0x9F; 0 marks the five bytes the code page leaves
// undefined (0x81, 0x8D, 0x8F, 0x90, 0x9D).
constexpr uint16_t kCp1252High[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

// Python's str.isspace() over what a Windows-1252 byte can decode to: the
// ASCII controls \t..\r and \x1c..\x1f, space, and NO-BREAK SPACE (0xA0).
// The strip steps work on the bytes before decoding, so this is the set of
// bytes whose character str.strip() removes.
bool is_space_byte(uint8_t c) { return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0xA0; }

std::string_view strip_space(std::string_view s) {
  while (!s.empty() && is_space_byte(uint8_t(s.front()))) s.remove_prefix(1);
  while (!s.empty() && is_space_byte(uint8_t(s.back()))) s.remove_suffix(1);
  return s;
}

bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int hex_value(char c) { return is_digit(c) ? c - '0' : (c | 0x20) - 'a' + 10; }

std::string ascii_upper(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return s;
}

std::string ascii_lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

// `name` ends with `ext` (upper case), ASCII case aside.
bool has_ext(std::string_view name, std::string_view ext) {
  const std::string u = ascii_upper(std::string(name));
  return u.size() >= ext.size() && std::string_view(u).substr(u.size() - ext.size()) == ext;
}

// A Classic About/credits string: the C string, CRLF made LF, trimmed.
std::string plain_text(std::string_view data) {
  std::string_view s = c_string(data);
  std::string t;
  t.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
    t += s[i];
  }
  return cp1252_to_utf8(strip_space(t));
}

}  // namespace

std::string cp1252_to_utf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    uint8_t b = uint8_t(ch);
    if (b >= 0x80 && b < 0xA0) append_utf8(out, kCp1252High[b - 0x80] ? kCp1252High[b - 0x80] : 0xFFFD);
    else append_utf8(out, b);
  }
  return out;
}

std::string_view c_string(std::string_view s) {
  size_t nul = s.find('\0');
  return nul == std::string_view::npos ? s : s.substr(0, nul);
}

// The same reader as the prototype (make_catalog.py rtf_to_text), which
// matches control words with \\([a-z]+)(-?\d+)? ?|\\'([0-9a-fA-F]{2})|\\(.) at
// each backslash. It works on the Windows-1252 bytes and decodes at the end:
// every rule below looks only at ASCII, so that is the same as decoding
// first.
std::string rtf_to_text(std::string_view rtf) {
  // The resources are NUL-terminated after the closing brace; stop there so
  // the terminator does not leak into the text.
  std::string_view s = c_string(rtf);
  std::string out;
  std::vector<bool> stack;
  bool skip = false;
  size_t i = 0, n = s.size();
  while (i < n) {
    char ch = s[i];
    if (ch == '{') {
      stack.push_back(skip);
      i++;
    } else if (ch == '}') {
      if (!stack.empty()) {
        skip = stack.back();
        stack.pop_back();
      } else {
        skip = false;
      }
      i++;
      if (stack.empty()) break;  // end of the outermost {\rtf …} group
    } else if (ch == '\\') {
      size_t j = i + 1;
      if (j < n && is_lower(s[j])) {
        // Control word: letters, an optional signed number, one optional space.
        size_t w0 = j;
        while (j < n && is_lower(s[j])) j++;
        std::string_view w = s.substr(w0, j - w0);
        size_t k = j;
        if (k < n && s[k] == '-') k++;
        if (k < n && is_digit(s[k])) {
          while (k < n && is_digit(s[k])) k++;
          j = k;
        }
        if (j < n && s[j] == ' ') j++;
        i = j;
        // Destinations whose text is not part of the document.
        if (w == "fonttbl" || w == "colortbl" || w == "stylesheet" || w == "info" || w == "pict" || w == "object") {
          skip = true;
        } else if (skip) {
        } else if (w == "par" || w == "line") {
          out += '\n';
        } else if (w == "tab") {
          out += '\t';
        }
      } else if (j + 2 < n && s[j] == '\'' && is_hex(s[j + 1]) && is_hex(s[j + 2])) {
        if (!skip) out += char(hex_value(s[j + 1]) << 4 | hex_value(s[j + 2]));
        i = j + 3;
      } else if (j < n && s[j] != '\n') {
        // Control symbol.
        char c = s[j];
        if (c == '*') skip = true;
        else if ((c == '\\' || c == '{' || c == '}') && !skip) out += c;
        else if (c == '~' && !skip) out += ' ';
        i = j + 1;
      } else {
        i++;  // a lone backslash (end of text, or before a newline)
      }
    } else if (ch == '\r' || ch == '\n') {
      i++;
    } else {
      if (!skip) out += ch;
      i++;
    }
  }
  // Blanks before a line break go; then runs of 3+ line breaks become 2. Two
  // passes, as the prototype's two re.sub calls: dropping the blanks in
  // "\n \n\n" is what makes it a run of three.
  std::string t;
  t.reserve(out.size());
  for (size_t p = 0; p < out.size();) {
    if (out[p] == ' ' || out[p] == '\t') {
      size_t q = p;
      while (q < out.size() && (out[q] == ' ' || out[q] == '\t')) q++;
      if (q >= out.size() || out[q] != '\n') t.append(out, p, q - p);
      p = q;
    } else {
      t += out[p++];
    }
  }
  std::string u;
  u.reserve(t.size());
  for (size_t p = 0; p < t.size();) {
    if (t[p] != '\n') {
      u += t[p++];
      continue;
    }
    size_t q = p;
    while (q < t.size() && t[q] == '\n') q++;
    u.append(q - p >= 3 ? 2 : q - p, '\n');
    p = q;
  }
  return cp1252_to_utf8(strip_space(u));
}

// ---- control records ----------------------------------------------------------------

namespace {

// Little-endian fields of a record that may be shorter than its layout; the
// callers check lengths the way the prototype does.
uint16_t u16_at(std::string_view d, size_t off) { return uint16_t(uint8_t(d[off]) | uint8_t(d[off + 1]) << 8); }
int16_t i16_at(std::string_view d, size_t off) { return int16_t(u16_at(d, off)); }

// Bytes [a, b) of the record, clipped to its end (Python slicing).
std::string_view slice(std::string_view d, size_t a, size_t b) {
  if (a >= d.size()) return {};
  return d.substr(a, std::min(b, d.size()) - a);
}

std::string record_string(std::string_view d, size_t a, size_t b) { return cp1252_to_utf8(c_string(slice(d, a, b))); }

// AFTERDAR.SCR 0x404f7b: the stops of a string slider. When the first stored
// value is nonzero the host prepends a 0 value and repeats the last label,
// flagged (ADPAGE draws that label bold), so the lowest stop always sends 0 —
// values are the LOWER bound of each stop. The default stop is the last one
// whose value does not exceed the record's default.
void string_slider(std::string_view d, int n, int dflt, CatalogControl& c) {
  std::vector<std::string> labels;
  for (int i = 0; i < n; i++) labels.push_back(record_string(d, 0x20 + 16 * size_t(i), 0x30 + 16 * size_t(i)));
  size_t vo = 0x20 + 16 * size_t(n);
  std::vector<int> vals(size_t(n), 0);
  if (d.size() >= vo + 2 * size_t(n))
    for (int i = 0; i < n; i++) vals[i] = u16_at(d, vo + 2 * size_t(i));
  std::optional<int> bold;
  if (n && vals[0] >= 1) {
    labels.push_back(labels.back());
    vals.insert(vals.begin(), 0);
    bold = int(labels.size()) - 1;
  }
  int cnt = std::min<int>(int(labels.size()), 101);
  labels.resize(size_t(cnt));
  vals.resize(size_t(cnt));
  if (bold && *bold >= cnt) bold.reset();
  int idx = cnt - 1;
  for (int i = 1; i < cnt; i++) {
    if (vals[i] > dflt) {
      idx = i - 1;
      break;
    }
  }
  c.type = "slider";
  c.items = std::move(labels);
  c.values = vals;
  c.def = vals.empty() ? 0 : vals[size_t(idx)];
  c.default_stop = idx;  // -1 for a record with no stops, as the prototype has it
  c.bold_stop = bold;
}

}  // namespace

std::optional<CatalogControl> parse_control_record(std::string_view d, int slot) {
  if (d.size() < 2) return std::nullopt;
  uint16_t kind = u16_at(d, 0);
  CatalogControl c;
  c.index = slot;
  c.name = record_string(d, 2, 0x16);
  int cnt = 0, dflt = 0;
  if (d.size() >= 0x1A) {
    cnt = u16_at(d, 0x16);
    dflt = i16_at(d, 0x18);
  }
  switch (kind) {
    case 1:
      c.kind = "stringslider";
      string_slider(d, cnt, dflt, c);
      break;
    case 2: {
      c.kind = "numslider";
      c.type = "slider";
      std::string unit = record_string(d, 0x20, 0x30);
      // Missing min/max bytes read as 0 (the prototype would stop on them;
      // no real record is that short).
      int mn = d.size() >= 0x32 ? i16_at(d, 0x30) : 0;
      int mx = d.size() >= 0x34 ? i16_at(d, 0x32) : 0;
      int pos = d.size() >= 0x38 ? u16_at(d, 0x36) : 0;
      // AFTERDAR.SCR 0x4050f4 clamps the default; many Classic records keep
      // one outside [min,max], so the raw value stays in the catalog too.
      c.min = mn;
      c.max = mx;
      c.def = dflt < mn ? mn : (dflt > mx ? mx : dflt);
      c.raw_default = dflt;
      if (!unit.empty()) {
        c.unit = unit;
        c.unit_pos = pos == 1 ? "prefix" : (pos ? "suffix" : "none");
      }
      break;
    }
    case 3: {
      c.kind = "popup";
      c.type = "popup";
      int n = std::min(cnt, 101);
      for (int i = 0; i < n; i++) c.items.push_back(record_string(d, 0x20 + 16 * size_t(i), 0x30 + 16 * size_t(i)));
      c.def = dflt < 0 ? 0 : (dflt >= n ? n - 1 : dflt);
      break;
    }
    case 4:
      c.kind = "button";
      c.type = "button";
      break;
    case 5:
      c.kind = "checkbox";
      c.type = "checkbox";
      c.def = dflt ? 1 : 0;
      break;
    default:
      return std::nullopt;
  }
  return c;
}

std::string ad20_about(std::string_view about) {
  static constexpr std::string_view kOwner = "Berkeley Systems Authorized User.";
  auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  std::string s(about);
  // The last line stood for the registered owner (After Dark 2.0 wrote the
  // name in its place); the disks hold only the stand-in.
  const size_t nl = s.rfind('\n');
  std::string_view last = nl == std::string::npos ? std::string_view(s) : std::string_view(s).substr(nl + 1);
  while (!last.empty() && blank(last.front())) last.remove_prefix(1);
  while (!last.empty() && blank(last.back())) last.remove_suffix(1);
  if (last == kOwner) {
    s.resize(nl == std::string::npos ? 0 : nl);
    while (!s.empty() && blank(s.back())) s.pop_back();
  }
  // Sentences wrapped by hand ("turn off your \ncomputer"): the break goes,
  // the space before it stays.
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\n' && i > 0 && s[i - 1] == ' ' && i + 1 < s.size() && is_lower(s[i + 1])) continue;
    out += s[i];
  }
  return out;
}

bool is_system_dll(std::string_view name) {
  static const std::set<std::string, std::less<>> kSystem = {
      "KERNEL32.DLL", "USER32.DLL", "GDI32.DLL", "WINMM.DLL", "MSACM32.DLL", "SHELL32.DLL", "COMDLG32.DLL", "KERNEL",
      "USER",         "GDI",        "MMSYSTEM",  "COMMDLG",   "SHELL",       "KEYBOARD",    "WIN87EM"};
  return kSystem.count(name) != 0;
}

std::vector<std::string> parse_stringlist(std::string_view data) {
  std::vector<std::string> out;
  if (data.size() < 2) return out;
  size_t n = u16_at(data, 0), p = 2;
  for (size_t k = 0; k < n; k++) {
    size_t e = data.find('\0', p);
    if (e == std::string_view::npos) {
      out.push_back(loader::latin1_to_utf8(data.substr(std::min(p, data.size()))));
      break;
    }
    out.push_back(loader::latin1_to_utf8(data.substr(p, e - p)));
    p = e + 1;
  }
  return out;
}

// ---- one module ------------------------------------------------------------------

namespace {

// Resource ids compared the way the prototype's reader keys them: integers by
// value, strings case-insensitively, and an all-digit string type as the
// integer it spells.
bool id_is(const ResId& id, uint16_t num) {
  if (!id.is_string) return id.num == num;
  return !id.str.empty() && id.str.size() <= 5 && std::all_of(id.str.begin(), id.str.end(), is_digit) &&
         std::stoul(id.str) == num;
}

bool id_is(const ResId& id, std::string_view name) {
  return id.is_string && ascii_upper(id.str) == ascii_upper(std::string(name));
}

// US English first, then language-neutral, then the rest in id order: what
// an English Windows' FindResource returns.
int language_rank(uint16_t lang) { return lang == 0x409 ? 0 : lang == 0 ? 1 : 2; }

template <typename Type>
std::optional<std::string_view> pe_find(const loader::pe::Image& img, const Type& type, uint16_t name) {
  const loader::pe::Resource* best = nullptr;
  for (const auto& r : img.resources()) {
    if (!id_is(r.type, type) || !id_is(r.name, name)) continue;
    if (!best || std::pair(language_rank(r.language), r.language) <
                     std::pair(language_rank(best->language), best->language))
      best = &r;
  }
  if (!best) return std::nullopt;
  return img.resource_data(*best);
}

template <typename Type>
std::optional<std::string_view> ne_find(const loader::ne::Image& img, const Type& type, uint16_t name) {
  for (const auto& r : img.resources())
    if (id_is(r.type, type) && id_is(r.name, name)) return img.resource_data(r);
  return std::nullopt;
}

// VERSIONINFO FileDescription: the first non-empty one, taking the version
// resources in the language order above. A damaged block counts as none.
std::string pe_file_description(const loader::pe::Image& img) {
  std::vector<const loader::pe::Resource*> versions;
  for (const auto& r : img.resources())
    if (id_is(r.type, loader::rt::version)) versions.push_back(&r);
  std::stable_sort(versions.begin(), versions.end(), [](auto* a, auto* b) {
    return std::pair(language_rank(a->language), a->language) < std::pair(language_rank(b->language), b->language);
  });
  for (auto* r : versions) {
    try {
      loader::VersionInfo vi = loader::parse_version_info(img.resource_data(*r), true);
      for (const auto& table : vi.string_tables)
        for (const auto& [k, v] : table.strings)
          if (k == "FileDescription" && !v.empty()) return v;
    } catch (const loader::LoaderError&) {
    }
  }
  return {};
}

std::string read_whole_file(const fs::path& p) {
  Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                       nullptr));
  if (!h.valid())
    throw ImportError(Status::error, "cannot read " + to_utf8(p.wstring()) + ": " + win_error_string(GetLastError()));
  LARGE_INTEGER size{};
  // Modules are a few hundred KB; anything past this is not one.
  if (!GetFileSizeEx(h.get(), &size) || size.QuadPart > (64ll << 20))
    throw ImportError(Status::source_invalid, to_utf8(p.wstring()) + " is too large to be a module");
  std::string data(size_t(size.QuadPart), '\0');
  DWORD got = 0;
  if (!data.empty() && (!ReadFile(h.get(), data.data(), DWORD(data.size()), &got, nullptr) || got != data.size()))
    throw ImportError(Status::error, "read error on " + to_utf8(p.wstring()) + ": " + win_error_string(GetLastError()));
  return data;
}

// What an NE module is to the ne16 lane, rule for rule as its detect_kind
// decides (host/ne16/package.cc), so the catalog lists a file exactly as the
// lane will run it: MODULE makes an After Dark module, whatever else it
// exports; SAVERINIT and SAVERDRAW make an Intermission IMX module, unless it
// exports SETCURRSAVER (another product's) or its file is named IMXX_* (an
// extension) — IMIMXPLY.IMQ's own refusals (its message 10); anything else is
// no module the lane runs, and `why` says so in the lane's words. Exports
// are found by name, without case, as the lane looks them up (find_ordinal:
// no entry-table entry needed).
// SAVERMAIN alone makes an Intermission reader, which the lane runs as an
// IMQ module, its own reader, when that reader's QUERY says it is a saver;
// without running anything, the catalog takes an .IMQ (the file type
// Intermission gives readers) not named as Intermission's own readers are
// for one (imq), and leaves every other reader out. SCREENSAVERPROC, none of
// those, makes a Windows 3.1 screen-saver program (scrnsave: SCRNSAVE.LIB's
// convention, which a program, never a library, follows).
enum class NeKind { after_dark, intermission, imq, scrnsave, none };

NeKind ne_kind(const loader::ne::Image& img, const std::string& file_name, std::string* why) {
  auto exports = [&](const char* name) { return img.find_ordinal(name).has_value(); };
  if (exports("MODULE")) return NeKind::after_dark;
  const bool init = exports("SAVERINIT"), draw = exports("SAVERDRAW");
  if (init && draw) {
    if (exports("SETCURRSAVER")) {
      *why = "an Intermission module that exports SETCURRSAVER, which the IMX reader refuses";
    } else if (ascii_upper(file_name.substr(0, 5)) == "IMXX_") {
      *why = "an Intermission module named IMXX_*, which the IMX reader refuses";
    } else {
      return NeKind::intermission;
    }
    return NeKind::none;
  }
  if (exports("SAVERMAIN")) {
    if (has_ext(file_name, ".IMQ") && !is_intermission_reader(file_name)) return NeKind::imq;
    *why = "an Intermission reader (it exports SAVERMAIN), not a module";
  } else if (init || draw) {
    *why = std::string("not an Intermission module: it exports ") + (init ? "SAVERINIT" : "SAVERDRAW") + " without " +
           (init ? "SAVERDRAW" : "SAVERINIT");
  } else if (exports("SCREENSAVERPROC")) {
    if (!img.header().is_dll()) return NeKind::scrnsave;
    *why = "a library that exports SCREENSAVERPROC, not a screen-saver program";
  } else {
    *why = "not an After Dark or Intermission module (no MODULE, SAVERINIT or SAVERDRAW export)";
  }
  return NeKind::none;
}

// A Windows 3.1 screen-saver program's own name: its module description
// after "SCRNSAVE" and a colon, as Windows 3.1's Control Panel read it
// ("SCRNSAVE :Screen Antics" -> "Screen Antics"), as Windows-1252; "" when
// the description is not of that form.
std::string scrnsave_name(const loader::ne::Image& img) {
  const std::string d = img.description();
  if (d.size() < 8 || ascii_upper(d.substr(0, 8)) != "SCRNSAVE") return "";
  size_t i = 8;
  while (i < d.size() && d[i] == ' ') i++;
  if (i == d.size() || d[i] != ':') return "";
  return cp1252_to_utf8(d.substr(i + 1));
}

void split_dlls(std::set<std::string> dlls, CatalogModule& m) {
  // std::set orders by bytes, as Python's sorted() orders these ASCII names.
  for (const auto& d : dlls) (is_system_dll(d) ? m.system : m.needs).push_back(d);
}

// What an Intermission entry says beside its lane and id: no resource holds
// its name or text (the registry's name overrides give moduleName), its
// settings are a dialog (`dialog`: the Configure... button), and `entry`.
void intermission_entry(CatalogModule& m, const std::string& base, const char* entry, bool dialog) {
  m.abi = "intermission";
  m.entry = entry;
  m.display_name = base;
  if (dialog) {
    CatalogControl b;
    b.index = 0;
    b.name = kIntermissionConfigure;
    b.kind = b.type = "button";
    m.controls.push_back(std::move(b));
  }
  m.module_name = base;
}

// An Intermission ASA animation's header (the ne16 lane's rule: data that
// starts "AniN", or "AniM" as The Far Side's EGGFIGHT.ASA does).
bool asa_header(std::string_view data) {
  return data.size() >= 4 && (data.substr(0, 4) == "AniN" || data.substr(0, 4) == "AniM");
}

void add_controls(CatalogModule& m, const std::function<std::optional<std::string_view>(uint16_t)>& find) {
  for (int slot = 0; slot < 4; slot++) {
    auto d = find(uint16_t(slot + 1));
    if (!d || d->empty()) continue;
    if (auto c = parse_control_record(*d, slot)) m.controls.push_back(std::move(*c));
  }
}

// A name trimmed at both ends: the whitespace str.strip() removes from a
// Windows-1252 name (see is_space_byte), NO-BREAK SPACE as UTF-8 included.
std::string trim_name(std::string s) {
  auto space_at_end = [&](bool front) -> size_t {
    if (s.empty()) return 0;
    if (front) {
      if (is_space_byte(uint8_t(s[0])) && uint8_t(s[0]) < 0x80) return 1;
      if (s.size() >= 2 && uint8_t(s[0]) == 0xC2 && uint8_t(s[1]) == 0xA0) return 2;
    } else {
      if (is_space_byte(uint8_t(s.back())) && uint8_t(s.back()) < 0x80) return 1;
      if (s.size() >= 2 && uint8_t(s[s.size() - 2]) == 0xC2 && uint8_t(s.back()) == 0xA0) return 2;
    }
    return 0;
  };
  while (size_t n = space_at_end(true)) s.erase(0, n);
  while (size_t n = space_at_end(false)) s.resize(s.size() - n);
  return s;
}

}  // namespace

bool is_intermission_reader(std::string_view file_name) {
  const std::string u = ascii_upper(std::string(file_name));
  return u.size() == 12 && u.compare(0, 2, "IM") == 0 && u.compare(5, 7, "PLY.IMQ") == 0;
}

CatalogModule catalog_module(const fs::path& file, const std::string& rel_path, const Package* package) {
  std::string data = read_whole_file(file);
  loader::Format fmt = loader::detect_format(data);
  CatalogModule m;
  std::string base = ascii_lower(to_utf8(file.stem().wstring()));
  m.path = rel_path;
  m.md5 = md5_hex(data.data(), data.size());
  const bool legacy_ids = !package || package->is_deluxe();
  if (package) {
    m.package = package->id;
    m.package_title = package->title;
    if (package->screen) m.screen = package->screen;
  }
  if (asa_header(data)) {
    // Played by Intermission's ASA reader, whose dialog its Configure...
    // button opens; it imports nothing itself.
    m.lane = "ne16";
    m.id = (legacy_ids ? "classic." : std::string(package->id) + ".") + base;
    intermission_entry(m, base, "SAVERMAIN", true);
    return m;
  }
  if (fmt == loader::Format::pe32) {
    loader::pe::Image img(std::move(data));
    m.lane = "pe32";
    m.id = (legacy_ids ? "ad40." : std::string(package->id) + ".") + base;
    m.display_name = pe_file_description(img);
    auto rtf = pe_find(img, uint16_t(2000), 40);
    m.about = rtf && !rtf->empty() ? rtf_to_text(*rtf) : std::string();
    add_controls(m, [&](uint16_t name) { return pe_find(img, uint16_t(1000), name); });
    std::set<std::string> dlls;
    for (const auto& imp : img.imports()) dlls.insert(ascii_upper(imp.dll));
    split_dlls(std::move(dlls), m);
    m.entry = img.find_export("_Module@4") ? "_Module@4" : "Module";
    if (m.display_name.empty()) {
      auto sl = pe_find(img, std::string_view("STRINGLIST"), 128);
      auto list = sl ? parse_stringlist(*sl) : std::vector<std::string>{};
      m.display_name = !list.empty() && !list[0].empty() ? list[0] : base;
    }
  } else if (fmt == loader::Format::ne) {
    loader::ne::Image img(std::move(data));
    std::string why;
    const NeKind kind = ne_kind(img, to_utf8(file.filename().wstring()), &why);
    // A file the lane would refuse is not offered at all (the catalog logs
    // why and leaves it out, as it does a module it cannot read).
    if (kind == NeKind::none) throw ImportError(Status::source_invalid, why);
    m.lane = "ne16";
    m.id = (legacy_ids ? "classic." : std::string(package->id) + ".") + base;
    std::set<std::string> dlls;
    for (const auto& ref : img.module_refs()) dlls.insert(ascii_upper(loader::latin1_to_utf8(ref)));
    if (kind == NeKind::intermission || kind == NeKind::imq) {
      // No resource holds an IMX or IMQ module's name or text: the
      // registry's name overrides give moduleName; its settings are its own
      // dialog. The IMX reader calls an IMX module's SAVERDRAW; an IMQ
      // module is a reader, called at its SAVERMAIN.
      intermission_entry(m, base, kind == NeKind::imq ? "SAVERMAIN" : "SAVERDRAW",
                         img.find_export("SAVERDLGPROC") != nullptr);
      split_dlls(std::move(dlls), m);
      return m;
    }
    if (kind == NeKind::scrnsave) {
      // A Windows 3.1 screen-saver program: named by its description (the
      // registry's name overrides may say otherwise), no text resource; its
      // settings are its own dialog, behind Control Panel's Setup... button
      // (SCREENSAVERCONFIGUREDIALOG, which SCRNSAVE.LIB has every program
      // export); run at its SCREENSAVERPROC.
      m.abi = "scrnsave";
      m.entry = "SCREENSAVERPROC";
      m.display_name = trim_name(scrnsave_name(img));
      if (m.display_name.empty()) m.display_name = base;
      if (img.find_export("SCREENSAVERCONFIGUREDIALOG")) {
        CatalogControl b;
        b.index = 0;
        b.name = kScrnsaveSetup;
        b.kind = b.type = "button";
        m.controls.push_back(std::move(b));
      }
      split_dlls(std::move(dlls), m);
      m.module_name = m.display_name;
      return m;
    }
    if (auto nm = ne_find(img, uint16_t(2000), 20); nm && !nm->empty()) m.display_name = cp1252_to_utf8(c_string(*nm));
    auto about = ne_find(img, uint16_t(2000), 30);
    m.about = about && !about->empty() ? plain_text(*about) : std::string();
    if (package && package->about == Package::About::ad20) m.about = ad20_about(m.about);
    if (auto credits = ne_find(img, uint16_t(2000), 10); credits && !credits->empty()) m.credits = plain_text(*credits);
    add_controls(m, [&](uint16_t name) { return ne_find(img, uint16_t(1000), name); });
    split_dlls(std::move(dlls), m);
    m.entry = "MODULE";
    if (m.display_name.empty()) {
      auto sl = ne_find(img, std::string_view("STRINGLIST"), 128);
      auto list = sl ? parse_stringlist(*sl) : std::vector<std::string>{};
      m.display_name = !list.empty() && !list[0].empty() ? list[0] : base;
    }
  } else {
    throw ImportError(Status::source_invalid, "not a PE32 or NE image (" + std::string(loader::format_name(fmt)) + ")");
  }
  m.module_name = trim_name(m.display_name);
  if (m.module_name.empty()) m.module_name = base;
  return m;
}

// ---- the catalog ------------------------------------------------------------------

namespace {

bool iequals_w(std::wstring_view a, std::wstring_view b) {
  return CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL;
}

// <dir>\*.AD (and, with `imx`, *.IMX; with `asa_imq`, *.ASA and *.IMQ too;
// with `scr`, *.SCR), matched case-insensitively (as glob does on Windows)
// and sorted by name together.
std::vector<std::wstring> modules_in(const fs::path& dir, bool imx, bool asa_imq = false, bool scr = false) {
  std::vector<std::wstring> names;
  std::error_code ec;
  auto ends_with = [](std::wstring_view n, std::wstring_view ext) {
    return n.size() >= ext.size() && iequals_w(n.substr(n.size() - ext.size()), ext);
  };
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::wstring n = it->path().filename().wstring();
    const bool four = n.size() > 4;  // a name before a four-character extension
    if (n.size() < 3 || n[0] == L'.' ||
        !(ends_with(n, L".AD") || (imx && four && ends_with(n, L".IMX")) ||
          (asa_imq && four && (ends_with(n, L".ASA") || ends_with(n, L".IMQ"))) ||
          (scr && four && ends_with(n, L".SCR"))))
      continue;
    std::error_code fe;
    if (it->is_regular_file(fe)) names.push_back(n);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace

namespace {

struct ModuleFile {
  fs::path path;
  std::string rel;      // catalog path, relative to <win>
  std::string pkg_rel;  // relative to the package root ("AD10TH/TOAST2K.AD"), for name overrides
};

// The module files of one package tree, in catalog order.
std::vector<ModuleFile> module_files(const Package& pkg, const fs::path& dir) {
  std::vector<ModuleFile> order;
  std::error_code ec;
  const std::string root = pkg.root;
  // Deluxe's three fixed places hold *.AD only, so its order and ids stay as
  // they always were; the other packages' folders may hold IMX modules, a
  // Delrina release's module folders ASA animations and IMQ modules (never
  // its ENGINE, where the ASA reader is), and an InstallShield 1 package's a
  // screen-saver program (*.SCR: only there, never After Dark's
  // ENGINE\AFTERDAR.SCR).
  const bool imx = !pkg.is_deluxe();
  auto add_dir = [&](const std::string& d) {
    const bool asa_imq = pkg.delrina_installer() && d != "ENGINE";
    const bool scr = pkg.recipe == Recipe::is1 && d != "ENGINE";
    for (const auto& n : modules_in(dir / to_wide(d), imx, asa_imq, scr))
      order.push_back({dir / to_wide(d) / n, root + "/" + d + "/" + to_utf8(n), d + "/" + to_utf8(n)});
  };
  bool engine_listed = false;
  for (const char* d : pkg.module_dirs) {
    std::string name = d;
    if (name == "ENGINE") {
      engine_listed = true;
      if (pkg.is_deluxe()) {
        // The one MSVC-built AD4 module ships beside the engine files.
        if (fs::exists(dir / L"ENGINE" / L"STARRYNI.AD", ec))
          order.push_back({dir / L"ENGINE" / L"STARRYNI.AD", root + "/ENGINE/STARRYNI.AD", "ENGINE/STARRYNI.AD"});
        continue;
      }
    }
    add_dir(name);
  }
  // Every other package's ENGINE is scanned too (ad10's STARRYNI.AD).
  if (!pkg.is_deluxe() && !engine_listed) add_dir("ENGINE");
  return order;
}

std::string file_name_of(const std::string& rel) { return rel.substr(rel.rfind('/') + 1); }

}  // namespace

CatalogDoc build_catalog(const std::vector<CatalogTree>& trees, const std::function<void(const std::string&)>& log) {
  CatalogDoc doc;
  std::set<std::string> ids;
  std::map<std::string, const Package*> by_id;
  for (const CatalogTree& t : trees) {
    const Package& pkg = *t.package;
    by_id[pkg.id] = &pkg;
    CatalogPackage cp;
    cp.id = pkg.id;
    cp.title = pkg.title;
    cp.short_title = pkg.short_title;
    cp.root = pkg.root;
    cp.verified = t.verified;
    cp.imported_utc = t.imported_utc;
    cp.cover = t.cover;
    for (const ModuleFile& f : module_files(pkg, t.dir)) {
      try {
        CatalogModule m = catalog_module(f.path, f.rel, &pkg);
        // Ids key settings.ini; two files may not share one.
        if (!ids.insert(m.id).second) {
          if (log) log("catalog: skipped " + f.rel + ": duplicate id " + m.id);
          continue;
        }
        for (const NameOverride& o : pkg.name_overrides)
          if (CompareStringOrdinal(to_wide(o.module).c_str(), -1, to_wide(f.pkg_rel).c_str(), -1, TRUE) == CSTR_EQUAL)
            m.module_name = o.name;
        doc.modules.push_back(std::move(m));
        cp.modules++;
      } catch (const std::exception& e) {
        if (log) log("catalog: skipped " + f.rel + ": " + e.what());
      }
    }
    doc.packages.push_back(std::move(cp));
  }
  // Display names stay unique within a lane (the front-end lists by lane):
  // the first module with a name keeps it, a later one is told apart by its
  // package's short title, and then by its file name.
  std::set<std::string> taken;
  std::map<std::string, std::string> first_by_md5;
  for (CatalogModule& m : doc.modules) {
    auto key = [&](const std::string& name) { return m.lane + '\n' + ascii_lower(name); };
    std::string name = m.module_name;
    if (taken.count(key(name))) {
      const Package* p = by_id[m.package];
      std::string shortt = p ? p->short_title : m.package;
      name = m.module_name + " (" + shortt + ")";
      if (taken.count(key(name))) name = m.module_name + " (" + shortt + ", " + file_name_of(m.path) + ")";
      for (int k = 2; taken.count(key(name)); k++)
        name = m.module_name + " (" + shortt + ", " + file_name_of(m.path) + " " + std::to_string(k) + ")";
    }
    taken.insert(key(name));
    m.display_name = name;
    if (!m.md5.empty()) {
      auto [it, fresh] = first_by_md5.emplace(m.md5, m.id);
      if (!fresh) m.same_as = it->second;
    }
  }
  return doc;
}

std::vector<CatalogModule> scan_catalog(const fs::path& files_dir, const std::function<void(const std::string&)>& log) {
  std::error_code ec;
  if (!fs::is_directory(files_dir, ec))
    throw ImportError(Status::source_invalid, "no module files at " + to_utf8(files_dir.wstring()));
  CatalogTree t;
  t.package = find_package("deluxe");
  t.dir = files_dir;
  return build_catalog({t}, log).modules;
}

// ---- JSON ---------------------------------------------------------------------------

namespace {

// Just enough of a JSON value to lay the document out the way Python's
// json.dump(obj, indent=1, ensure_ascii=False) does: objects keep insertion
// order, one member per line, one space of indent per level.
struct Json {
  enum class Kind { number, string, array, object } kind = Kind::number;
  long long number = 0;
  std::string string;
  std::vector<Json> array;
  std::vector<std::pair<std::string, Json>> object;

  static Json num(long long v) { return Json{Kind::number, v, {}, {}, {}}; }
  static Json str(std::string v) { return Json{Kind::string, 0, std::move(v), {}, {}}; }
  static Json arr() { return Json{Kind::array, 0, {}, {}, {}}; }
  static Json obj() { return Json{Kind::object, 0, {}, {}, {}}; }
  Json& add(std::string key, Json v) {
    object.emplace_back(std::move(key), std::move(v));
    return *this;
  }
};

// json.encoder's escaping with ensure_ascii=False: '"', '\\' and the C0
// controls only; everything else (UTF-8) passes through.
void write_string(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
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
  o += '"';
}

void write_json(std::string& o, const Json& j, int level) {
  auto newline = [&](int l) {
    o += '\n';
    o.append(size_t(l), ' ');
  };
  switch (j.kind) {
    case Json::Kind::number: o += std::to_string(j.number); break;
    case Json::Kind::string: write_string(o, j.string); break;
    case Json::Kind::array:
      if (j.array.empty()) {
        o += "[]";
        break;
      }
      o += '[';
      for (size_t i = 0; i < j.array.size(); i++) {
        if (i) o += ',';
        newline(level + 1);
        write_json(o, j.array[i], level + 1);
      }
      newline(level);
      o += ']';
      break;
    case Json::Kind::object:
      if (j.object.empty()) {
        o += "{}";
        break;
      }
      o += '{';
      for (size_t i = 0; i < j.object.size(); i++) {
        if (i) o += ',';
        newline(level + 1);
        write_string(o, j.object[i].first);
        o += ": ";
        write_json(o, j.object[i].second, level + 1);
      }
      newline(level);
      o += '}';
      break;
  }
}

Json strings(const std::vector<std::string>& v) {
  Json a = Json::arr();
  for (const auto& s : v) a.array.push_back(Json::str(s));
  return a;
}

// Member order follows the prototype's dicts, so the files line up.
Json control_json(const CatalogControl& c) {
  Json j = Json::obj();
  j.add("index", Json::num(c.index)).add("name", Json::str(c.name)).add("kind", Json::str(c.kind));
  j.add("type", Json::str(c.type));
  if (c.kind == "stringslider") {
    Json vals = Json::arr();
    for (int v : c.values) vals.array.push_back(Json::num(v));
    j.add("items", strings(c.items)).add("values", std::move(vals));
    j.add("default", Json::num(c.def.value_or(0))).add("defaultStop", Json::num(c.default_stop.value_or(0)));
    if (c.bold_stop) j.add("boldStop", Json::num(*c.bold_stop));
  } else if (c.kind == "numslider") {
    j.add("min", Json::num(c.min.value_or(0))).add("max", Json::num(c.max.value_or(0)));
    j.add("default", Json::num(c.def.value_or(0))).add("rawDefault", Json::num(c.raw_default.value_or(0)));
    if (!c.unit.empty()) j.add("unit", Json::str(c.unit)).add("unitPos", Json::str(c.unit_pos));
  } else if (c.kind == "popup") {
    j.add("items", strings(c.items)).add("default", Json::num(c.def.value_or(0)));
  } else if (c.kind == "checkbox") {
    j.add("default", Json::num(c.def.value_or(0)));
  }
  return j;
}

Json module_json(const CatalogModule& m) {
  Json j = Json::obj();
  j.add("id", Json::str(m.id)).add("displayName", Json::str(m.display_name));
  j.add("lane", Json::str(m.lane)).add("path", Json::str(m.path)).add("about", Json::str(m.about));
  if (m.credits) j.add("credits", Json::str(*m.credits));
  Json controls = Json::arr();
  for (const auto& c : m.controls) controls.array.push_back(control_json(c));
  j.add("controls", std::move(controls)).add("entry", Json::str(m.entry));
  j.add("needs", strings(m.needs)).add("system", strings(m.system));
  // PACKAGES.md §6, appended so the prototype's fields keep their places.
  if (!m.package.empty()) {
    j.add("package", Json::str(m.package)).add("packageTitle", Json::str(m.package_title));
    j.add("moduleName", Json::str(m.module_name));
  }
  if (!m.md5.empty()) j.add("md5", Json::str(m.md5));
  if (!m.same_as.empty()) j.add("sameAs", Json::str(m.same_as));
  // Last, and only for a module whose ABI is not After Dark's: every After
  // Dark entry is laid out exactly as before it existed.
  if (!m.abi.empty()) j.add("abi", Json::str(m.abi));
  // Last too, and only for a package shown at a fixed screen: every other
  // entry is laid out as before it existed.
  if (!m.screen.empty()) j.add("screen", Json::str(m.screen));
  return j;
}

// COVERS.md §2.7: after "modules"; a generated cover is only its origin.
Json cover_json(const CatalogCover& c) {
  Json j = Json::obj();
  j.add("origin", Json::str(c.origin));
  if (c.generated()) return j;
  j.add("tile", Json::str(c.tile)).add("tileMd5", Json::str(c.tile_md5)).add("image", Json::str(c.image));
  j.add("width", Json::num(c.width)).add("height", Json::num(c.height));
  if (!c.art.empty()) j.add("art", Json::str(c.art));
  j.add("label", Json::str(c.label)).add("credit", Json::str(c.credit)).add("original", Json::str(c.original));
  return j;
}

// The release date the registry knows for a package id ("" when unknown).
std::string released_of(const std::string& id) {
  const Package* pk = find_package(id);
  return pk && pk->released ? pk->released : "";
}

Json package_json(const CatalogPackage& p) {
  Json j = Json::obj();
  j.add("id", Json::str(p.id)).add("title", Json::str(p.title)).add("shortTitle", Json::str(p.short_title));
  if (std::string r = released_of(p.id); !r.empty()) j.add("released", Json::str(r));
  j.add("root", Json::str(p.root)).add("verified", Json::str(p.verified));
  j.add("importedUtc", Json::str(p.imported_utc)).add("modules", Json::num((long long)p.modules));
  j.add("cover", cover_json(p.cover));
  return j;
}

}  // namespace

std::string render_catalog_json(const std::vector<CatalogModule>& modules, const std::vector<CatalogPackage>& packages) {
  Json root = Json::obj();
  root.add("version", Json::num(1)).add("generator", Json::str(kCatalogGenerator));
  // Oldest release first: the front-ends show the cover strip and the list groups in
  // this order. Undated packages keep their given order, after the dated ones.
  std::vector<const CatalogPackage*> by_date;
  for (const auto& p : packages) by_date.push_back(&p);
  std::stable_sort(by_date.begin(), by_date.end(), [](const CatalogPackage* a, const CatalogPackage* b) {
    std::string ra = released_of(a->id), rb = released_of(b->id);
    if (ra.empty() != rb.empty()) return rb.empty();
    return ra < rb;
  });
  Json pkgs = Json::arr();
  for (const CatalogPackage* p : by_date) pkgs.array.push_back(package_json(*p));
  root.add("packages", std::move(pkgs));
  Json mods = Json::arr();
  for (const auto& m : modules) mods.array.push_back(module_json(m));
  root.add("modules", std::move(mods));
  std::string out;
  write_json(out, root, 0);
  out += '\n';
  return out;
}

std::string render_catalog(const CatalogDoc& doc) { return render_catalog_json(doc.modules, doc.packages); }

}  // namespace adw::import
