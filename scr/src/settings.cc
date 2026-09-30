#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "paths.h"

namespace adw::scr {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

bool parse_int(std::string_view s, long long& out) {
  s = trim(s);
  if (s.empty()) return false;
  std::string tmp(s);
  char* end = nullptr;
  long long v = strtoll(tmp.c_str(), &end, 10);
  if (end != tmp.c_str() + tmp.size()) return false;
  out = v;
  return true;
}

constexpr std::string_view kSaver = "Saver";
constexpr std::string_view kModulePrefix = "Module.";

bool is_module_section(std::string_view name, std::string_view* id) {
  if (name.size() <= kModulePrefix.size() || !iequals(name.substr(0, kModulePrefix.size()), kModulePrefix)) return false;
  if (id) *id = name.substr(kModulePrefix.size());
  return true;
}

// Sound=1|0, as written or as a hand might write it: on/off, yes/no,
// true/false. Anything else is not an answer (the default stays).
bool parse_switch(std::string_view s, bool& out) {
  s = trim(s);
  long long n;
  if (parse_int(s, n)) {
    out = n != 0;
    return true;
  }
  for (std::string_view on : {"on", "yes", "true"}) {
    if (iequals(s, on)) return out = true;
  }
  for (std::string_view off : {"off", "no", "false"}) {
    if (iequals(s, off)) {
      out = false;
      return true;
    }
  }
  return false;
}

std::string format_scale(double v) {
  char buf[32];
  // "1.0"/"1.5" as DESIGN.md spells them; anything hand-edited keeps its digits.
  if (v * 10.0 == (double)(long long)(v * 10.0)) snprintf(buf, sizeof(buf), "%.1f", v);
  else snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

} // namespace

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return true;
}

// ---- IniFile -------------------------------------------------------------------

void IniFile::parse(std::string_view text) {
  sections_.clear();
  sections_.push_back({});
  if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) {
    text.remove_prefix(3);
  }
  while (!text.empty()) {
    size_t nl = text.find('\n');
    std::string_view raw = text.substr(0, nl);
    text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    std::string_view t = trim(raw);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
      sections_.push_back({std::string(trim(t.substr(1, t.size() - 2))), {}});
      continue;
    }
    Line line;
    line.raw = std::string(raw);
    size_t eq = t.find('=');
    if (!t.empty() && t.front() != ';' && t.front() != '#' && eq != std::string_view::npos && eq > 0) {
      line.entry = true;
      line.key = std::string(trim(t.substr(0, eq)));
      line.value = std::string(trim(t.substr(eq + 1)));
    }
    sections_.back().lines.push_back(std::move(line));
  }
}

std::string IniFile::serialize() const {
  std::string out;
  for (const auto& sec : sections_) {
    // Only the first section is the header-less preamble. A hand-written
    // "[]" is a real (nameless) section, as GetPrivateProfileString reads it;
    // dropping its header would hand its entries to the section above.
    if (&sec != &sections_.front()) out += "[" + sec.name + "]\r\n";
    for (const auto& l : sec.lines) {
      out += l.entry ? l.key + "=" + l.value : l.raw;
      out += "\r\n";
    }
  }
  return out;
}

IniFile::Section* IniFile::find(std::string_view name) {
  for (auto& s : sections_) if (!s.name.empty() && iequals(s.name, name)) return &s;
  return nullptr;
}

const IniFile::Section* IniFile::find(std::string_view name) const {
  for (auto& s : sections_) if (!s.name.empty() && iequals(s.name, name)) return &s;
  return nullptr;
}

const std::string* IniFile::get(std::string_view section, std::string_view key) const {
  const Section* s = find(section);
  if (!s) return nullptr;
  // Last one wins, as with GetPrivateProfileString on a duplicated key.
  const std::string* r = nullptr;
  for (auto& l : s->lines) if (l.entry && iequals(l.key, key)) r = &l.value;
  return r;
}

void IniFile::set(std::string_view section, std::string_view key, std::string_view value) {
  Section* s = find(section);
  if (!s) {
    if (sections_.empty()) sections_.push_back({});
    auto& last = sections_.back().lines;
    auto blank = [](const Line& l) { return !l.entry && trim(l.raw).empty(); };
    bool had_content = !sections_.back().name.empty() || !last.empty();
    if (had_content && (last.empty() || !blank(last.back()))) {
      last.push_back({});   // blank separator line before the new section
    }
    sections_.push_back({std::string(section), {}});
    s = &sections_.back();
  }
  Line* hit = nullptr;
  for (auto& l : s->lines) if (l.entry && iequals(l.key, key)) hit = &l;
  if (hit) {
    hit->value = std::string(value);
    return;
  }
  Line l;
  l.entry = true;
  l.key = std::string(key);
  l.value = std::string(value);
  // Insert after the last entry so trailing blank lines keep separating sections.
  auto pos = s->lines.end();
  while (pos != s->lines.begin() && !(pos - 1)->entry && trim((pos - 1)->raw).empty()) --pos;
  s->lines.insert(pos, std::move(l));
}

void IniFile::remove_key(std::string_view section, std::string_view key) {
  if (Section* s = find(section)) {
    std::erase_if(s->lines, [&](const Line& l) { return l.entry && iequals(l.key, key); });
  }
}

void IniFile::clear_entries(std::string_view section) {
  for (auto& s : sections_) {
    if (!s.name.empty() && iequals(s.name, section)) std::erase_if(s.lines, [](const Line& l) { return l.entry; });
  }
}

void IniFile::remove_section(std::string_view section) {
  std::erase_if(sections_, [&](const Section& s) { return !s.name.empty() && iequals(s.name, section); });
}

std::vector<std::string> IniFile::section_names() const {
  std::vector<std::string> r;
  for (auto& s : sections_) if (!s.name.empty()) r.push_back(s.name);
  return r;
}

std::vector<std::pair<std::string, std::string>> IniFile::entries(std::string_view section) const {
  std::vector<std::pair<std::string, std::string>> r;
  // Merge duplicate sections (a hand-edited file may repeat one).
  for (auto& s : sections_) {
    if (s.name.empty() || !iequals(s.name, section)) continue;
    for (auto& l : s.lines) if (l.entry) r.emplace_back(l.key, l.value);
  }
  return r;
}

// ---- Settings ------------------------------------------------------------------

bool Settings::is_random() const { return module.empty() || iequals(module, "random"); }

const std::vector<std::string>& dialog_checklist(const Settings& s) {
  return s.rotates() ? s.randomize : s.randomize_saved;
}

bool dialog_checklist_none(const Settings& s) { return !s.rotates() && s.randomize_saved_none; }

bool random_allowed(const DialogChoice& c) { return !c.random || c.total == 0 || c.shown_checked > 0; }

std::vector<std::string> normalize_collections(const std::vector<std::string>& selected, size_t releases) {
  std::vector<std::string> ids;
  for (const auto& id : selected) {
    if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  }
  if (releases > 0 && ids.size() >= releases) ids.clear();
  return ids;
}

Settings apply_dialog_choice(const Settings& loaded, const DialogChoice& c) {
  Settings s = loaded;
  if (c.total == 0) return s;
  if (c.strip) s.collections = normalize_collections(c.collections, c.releases);
  const bool all = c.checked.size() >= c.total;
  if (c.random) {
    s.randomize_saved.clear();   // the checklist is the rotation list now
    s.randomize_saved_none = false;
    if (loaded.has_lead()) {
      s.module = loaded.module;
      s.randomize = c.checked;
    } else {
      s.module = "random";
      s.randomize = all ? std::vector<std::string>{} : c.checked;
    }
  } else {
    s.randomize.clear();
    // All checked keeps nothing (= every module, so modules imported later
    // join in); nothing checked is said outright, or it would read as "all".
    s.randomize_saved = all ? std::vector<std::string>{} : c.checked;
    s.randomize_saved_none = c.checked.empty();
    if (!c.selected.empty()) s.module = c.selected;
  }
  return s;
}

namespace {

std::vector<std::string> parse_id_list(std::string_view rest) {
  std::vector<std::string> ids;
  while (!rest.empty()) {
    size_t c = rest.find(',');
    std::string_view id = trim(rest.substr(0, c));
    rest.remove_prefix(c == std::string_view::npos ? rest.size() : c + 1);
    if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.emplace_back(id);
  }
  return ids;
}

std::string join_ids(const std::vector<std::string>& ids) {
  std::string list;
  for (const auto& id : ids) {
    if (!list.empty()) list += ",";
    list += id;
  }
  return list;
}

} // namespace

Settings parse_settings(std::string_view text) {
  IniFile ini;
  ini.parse(text);
  Settings s;
  if (auto* v = ini.get(kSaver, "Module")) s.module = v->empty() ? "random" : *v;
  if (auto* v = ini.get(kSaver, "Randomize")) s.randomize = parse_id_list(*v);
  if (auto* v = ini.get(kSaver, "RandomizeSaved")) {
    if (trim(*v) == kRandomizeSavedNone) s.randomize_saved_none = true;
    else s.randomize_saved = parse_id_list(*v);
  }
  if (auto* v = ini.get(kSaver, "Collections")) s.collections = parse_id_list(*v);
  long long n;
  if (auto* v = ini.get(kSaver, "DurationMin"); v && parse_int(*v, n)) {
    s.duration_min = (int)std::clamp<long long>(n, 0, 24 * 60 * 7);
  }
  if (auto* v = ini.get(kSaver, "Scale")) {
    std::string tmp(trim(*v));
    char* end = nullptr;
    double d = strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() && d >= 0.5 && d <= 4.0) s.scale = d;
  }
  if (auto* v = ini.get(kSaver, "Monitors")) s.all_monitors = !iequals(trim(*v), "primary");
  if (auto* v = ini.get(kSaver, "DifferentPerMonitor")) parse_switch(*v, s.different_per_monitor);
  if (auto* v = ini.get(kSaver, "StartFromDesktop"); v && parse_int(*v, n)) s.start_from_desktop = n != 0;
  if (auto* v = ini.get(kSaver, "Sound")) parse_switch(*v, s.sound);
  if (auto* v = ini.get(kSaver, "Volume"); v && parse_int(*v, n)) s.volume = (int)std::clamp<long long>(n, 0, 100);
  if (auto* v = ini.get(kSaver, "SoundMonitor"); v && !trim(*v).empty()) s.sound_monitor = std::string(trim(*v));
  for (const auto& name : ini.section_names()) {
    std::string_view id;
    if (!is_module_section(name, &id) || id.empty()) continue;
    auto& values = s.controls[std::string(id)];
    for (auto& [k, v] : ini.entries(name)) {
      long long idx, val;
      if (parse_int(k, idx) && parse_int(v, val) && idx >= 0 && idx < 4096) {
        values[(int)idx] = (int)std::clamp<long long>(val, INT32_MIN, INT32_MAX);
      }
    }
    if (values.empty()) s.controls.erase(std::string(id));
  }
  return s;
}

std::string serialize_settings(const Settings& s, std::string_view base) {
  IniFile ini;
  ini.parse(base);
  ini.set(kSaver, "Module", s.is_random() ? "random" : s.module);
  ini.set(kSaver, "Randomize", join_ids(s.randomize));
  // Only written while it says something: a partial checklist kept aside,
  // or "-" for one with nothing checked (no key means every module).
  if (s.randomize_saved_none) ini.set(kSaver, "RandomizeSaved", kRandomizeSavedNone);
  else if (s.randomize_saved.empty()) ini.remove_key(kSaver, "RandomizeSaved");
  else ini.set(kSaver, "RandomizeSaved", join_ids(s.randomize_saved));
  // The strip's filter. A key that already says this list (a missing key says
  // "all") is left exactly as written: an OK while the strip is hidden, or
  // one that changed no tile, doesn't touch it.
  {
    const std::string* cur = ini.get(kSaver, "Collections");
    if (s.collections != (cur ? parse_id_list(*cur) : std::vector<std::string>{})) {
      ini.set(kSaver, "Collections", join_ids(s.collections));
    }
  }
  ini.set(kSaver, "DurationMin", std::to_string(std::max(0, s.duration_min)));
  ini.set(kSaver, "Scale", format_scale(s.scale));
  ini.set(kSaver, "Monitors", s.all_monitors ? "all" : "primary");
  // A value that already says this ("on", "yes", "false") is left as written.
  {
    bool cur = !s.different_per_monitor;
    const std::string* v = ini.get(kSaver, "DifferentPerMonitor");
    if (!v || !parse_switch(*v, cur) || cur != s.different_per_monitor)
      ini.set(kSaver, "DifferentPerMonitor", s.different_per_monitor ? "1" : "0");
  }
  // No UI sets it: written only when it differs from the default, or when
  // the file already says something else.
  if (!s.start_from_desktop || ini.get(kSaver, "StartFromDesktop"))
    ini.set(kSaver, "StartFromDesktop", s.start_from_desktop ? "1" : "0");
  // Sound (AUDIO.md §9). A value that already says this ("on", "075") is
  // left as written; SoundMonitor keeps whatever it said (it is reserved).
  {
    bool cur = !s.sound;
    const std::string* v = ini.get(kSaver, "Sound");
    if (!v || !parse_switch(*v, cur) || cur != s.sound) ini.set(kSaver, "Sound", s.sound ? "1" : "0");
    long long n = -1;
    const int volume = std::clamp(s.volume, 0, 100);
    v = ini.get(kSaver, "Volume");
    if (!v || !parse_int(*v, n) || n != volume) ini.set(kSaver, "Volume", std::to_string(volume));
    const std::string monitor = trim(s.sound_monitor).empty() ? std::string("primary") : s.sound_monitor;
    v = ini.get(kSaver, "SoundMonitor");
    if (!v || std::string(trim(*v)) != monitor) ini.set(kSaver, "SoundMonitor", monitor);
  }

  // Rewrite every [Module.*] section from `s`: the in-memory map is the whole
  // truth for control values (it was loaded from this same file). Sections
  // that stay are rewritten in place; the rest go.
  for (const auto& name : ini.section_names()) {
    std::string_view id;
    if (!is_module_section(name, &id)) continue;
    auto it = s.controls.find(std::string(id));
    if (it != s.controls.end() && !it->second.empty()) ini.clear_entries(name);
    else ini.remove_section(name);
  }
  for (const auto& [id, values] : s.controls) {
    if (values.empty() || id.empty()) continue;
    std::string sec = std::string(kModulePrefix) + id;
    for (const auto& [idx, val] : values) ini.set(sec, std::to_string(idx), std::to_string(val));
  }
  return ini.serialize();
}

bool load_settings(const std::wstring& path, Settings& out) {
  std::string text;
  if (!read_file(path, text)) {
    out = Settings{};
    return false;
  }
  out = parse_settings(text);
  return true;
}

bool save_settings(const std::wstring& path, const Settings& s) {
  std::string base;
  read_file(path, base);   // absent is fine: start from an empty document
  return write_file_atomic(path, serialize_settings(s, base));
}

std::string format_cvset(const std::map<int, int>& values) {
  std::string r;
  for (const auto& [idx, val] : values) {
    if (!r.empty()) r += ",";
    r += std::to_string(idx) + "=" + std::to_string(val);
  }
  return r;
}

// ---- Rotation ------------------------------------------------------------------

Rotation::Rotation(std::vector<std::string> ids, uint32_t seed, const std::string& first) : rng_(seed) {
  for (auto& id : ids) {
    if (std::find(order_.begin(), order_.end(), id) == order_.end()) order_.push_back(std::move(id));
  }
  shuffle();
  if (first.empty()) return;
  if (auto it = std::find(order_.begin(), order_.end(), first); it != order_.end()) std::iter_swap(order_.begin(), it);
  else lead_ = first;
}

void Rotation::shuffle() { std::shuffle(order_.begin(), order_.end(), rng_); }

const std::string& Rotation::current() const {
  static const std::string none;
  if (!lead_.empty()) return lead_;
  return order_.empty() ? none : order_[pos_];
}

const std::string& Rotation::next() {
  if (!lead_.empty() && !order_.empty()) {
    lead_.clear();          // the bag starts at its first entry, not its second
    return order_[pos_];
  }
  if (order_.size() <= 1) return current();
  if (++pos_ < order_.size()) return order_[pos_];
  std::string last = order_.back();
  shuffle();
  if (order_[0] == last) {
    std::uniform_int_distribution<size_t> d(1, order_.size() - 1);
    std::swap(order_[0], order_[d(rng_)]);
  }
  pos_ = 0;
  return order_[0];
}

// ---- SharedRotation ------------------------------------------------------------

bool SharedRotation::tick(bool owner_plays) {
  if (bag_.size() < 2) return false;
  if (owner_plays) {
    waiting_ = true;
    return false;
  }
  move_on();
  return true;
}

bool SharedRotation::give_up(size_t dead, bool owner, bool owner_plays) {
  // Nothing else to show; or every module failed on that monitor in turn.
  if (bag_.size() < 2 || dead >= bag_.size()) return false;
  // Another monitor's game plays it: never taken from under the player.
  if (!owner && owner_plays) return false;
  move_on();
  return true;
}

void SharedRotation::move_on() {
  bag_.next();
  ++step_;
  waiting_ = false;
}

} // namespace adw::scr
