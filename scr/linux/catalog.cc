#include "catalog.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string_view>

#include "minijson.h"

namespace lad {

namespace {

using adw::import::JsonValue;
using Kind = JsonValue::Kind;

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return s;
}

std::string norm_path(std::string s) {
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return lower(s);
}

// A member as the Windows saver's parser reads an int: a number (a fraction
// cut toward zero) or a boolean (1/0); `dflt` for anything else.
int int_of(const JsonValue& o, std::string_view key, int dflt) {
  const JsonValue* v = o.get(key);
  if (!v) return dflt;
  if (v->kind == Kind::boolean) return v->boolean ? 1 : 0;
  return v->as_int(dflt);
}

// A control's default value, read and clamped exactly as the Windows saver
// reads it (scr/src/catalog.cc: parse_catalog, Control::clamp), so the
// player's host gets what a Windows saver's gets for a control never set.
// False for a control without a value: a button, an unknown kind, a popup
// without items.
bool control_default(const JsonValue& c, int* out) {
  const std::string type = c.str("type");
  const bool slider = type == "slider", checkbox = type == "checkbox", popup = type == "popup";
  int min = int_of(c, "min", 0), max = int_of(c, "max", checkbox ? 1 : 100);
  std::vector<std::string> items;
  if (const JsonValue* list = c.get("items"); list && list->kind == Kind::array) {
    for (const JsonValue& i : list->array) {
      if (i.kind == Kind::string) items.push_back(i.string);
    }
  }
  int def = int_of(c, "default", min);
  const int default_stop = int_of(c, "defaultStop", -1);
  std::vector<int> values;   // a string slider's, ascending
  if (slider && !items.empty()) {
    if (const JsonValue* list = c.get("values"); list && list->kind == Kind::array) {
      for (const JsonValue& v : list->array) {
        if (v.kind != Kind::number) break;
        values.push_back(v.as_int(0));
      }
      if (!values.empty()) {
        // As many stops as have both a label and a value; defaultStop
        // indexes the table as written, before it is put in order.
        const size_t n = std::min(values.size(), items.size());
        values.resize(n);
        items.resize(n);
        if (default_stop >= 0 && default_stop < (int)n) def = values[default_stop];
        std::stable_sort(values.begin(), values.end());
      }
    }
    min = values.empty() ? min : values.front();
    max = values.empty() ? min + (int)items.size() - 1 : values.back();
    if (values.empty() && default_stop >= 0 && default_stop < (int)items.size()) def = min + default_stop;
  }
  if (slider && !items.empty()) {
    // A string slider snaps to the last stop whose value is <= it.
    const int n = (int)items.size();
    int stop = 0;
    if (values.empty()) {
      stop = std::clamp(def - min, 0, n - 1);
    } else {
      for (int i = 0; i < n; ++i) {
        if (values[i] <= def) stop = i;
      }
    }
    *out = values.empty() ? min + stop : values[stop];
  } else if (slider) {
    *out = std::clamp(def, std::min(min, max), std::max(min, max));
  } else if (checkbox) {
    *out = def ? 1 : 0;
  } else if (popup && !items.empty()) {
    *out = std::clamp(def, min, min + (int)items.size() - 1);
  } else {
    return false;
  }
  return true;
}

// The module's host controls, in index order (the Windows saver sorts its
// controls by index).
std::vector<std::pair<std::string, std::string>> host_env_of(const JsonValue& module) {
  struct Var {
    int index;
    std::string name, value;
  };
  std::vector<Var> vars;
  const JsonValue* controls = module.get("controls");
  if (!controls || controls->kind != Kind::array) return {};
  for (const JsonValue& c : controls->array) {
    if (c.kind != Kind::object || !c.get("index")) continue;
    const int index = int_of(c, "index", -1);
    const JsonValue* host = c.get("host");
    if (index < 0 || !host) continue;
    // Not a string, or a name it may not set: left out (an empty one is the
    // module's own control, which the player doesn't send).
    if (host->kind != Kind::string || host->string.empty()) continue;
    if (!host_variable_ok(host->string)) continue;
    if (std::any_of(vars.begin(), vars.end(), [&](const Var& v) { return v.name == host->string; })) continue;
    int value = 0;
    if (!control_default(c, &value)) continue;
    vars.push_back({index, host->string, std::to_string(value)});
  }
  std::stable_sort(vars.begin(), vars.end(), [](const Var& a, const Var& b) { return a.index < b.index; });
  std::vector<std::pair<std::string, std::string>> env;
  for (Var& v : vars) env.emplace_back(std::move(v.name), std::move(v.value));
  return env;
}

}  // namespace

bool host_variable_ok(std::string_view name) {
  static constexpr std::string_view kOwn[] = {
      "ADSTREAM", "ADSCREENW", "ADSCREENH", "ADCVSET",   "ADCAPS",   "ADNUMLOCK",      "ADSTATE",
      "ADSEEDIMG", "ADSOUND",  "ADVOLUME",  "ADAUDIOOUT", "ADSTATUSHANDLE", "ADSTATUSLOG",
  };
  if (name.size() < 3 || name.substr(0, 2) != "AD" || name[2] == '_') return false;
  for (char ch : name) {
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
  }
  return std::find(std::begin(kOwn), std::end(kOwn), name) == std::end(kOwn);
}

const Module* Catalog::find(const std::string& id) const {
  for (const Module& m : modules) {
    if (m.id == id) return &m;
  }
  return nullptr;
}

SizeI screen_of(const std::string& s) {
  const size_t x = s.find_first_of("xX");
  if (x == std::string::npos) return {};
  auto number = [](std::string_view d, int& out) {
    if (d.empty() || d.size() > 5) return false;
    out = 0;
    for (char c : d) {
      if (c < '0' || c > '9') return false;
      out = out * 10 + (c - '0');
    }
    return true;
  };
  int w = 0, h = 0;
  if (!number(std::string_view(s).substr(0, x), w) || !number(std::string_view(s).substr(x + 1), h)) return {};
  if (w < 1 || h < 1 || w > 8192 || h > 8192 || (long long)w * h > 4096LL * 4096) return {};
  return SizeI{w, h};
}

bool load_catalog(const std::string& path, Catalog& out, std::string* error) {
  out.modules.clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "cannot read " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  auto root = adw::import::parse_json(ss.str());
  if (!root || root->kind != Kind::object) {
    if (error) *error = path + " is not JSON";
    return false;
  }
  const auto* modules = root->get("modules");
  if (!modules || modules->kind != Kind::array) {
    if (error) *error = path + " has no modules";
    return false;
  }
  for (const auto& item : modules->array) {
    if (item.kind != Kind::object) continue;
    Module m;
    m.id = item.str("id");
    m.path = item.str("path");
    if (m.id.empty() || m.path.empty()) continue;
    m.display_name = item.str("displayName", m.id);
    m.name = item.str("moduleName", m.display_name);
    if (m.name.empty()) m.name = m.display_name;
    m.lane = item.str("lane");
    m.abi = item.str("abi", "afterdark");
    if (m.abi.empty()) m.abi = "afterdark";
    m.screen = screen_of(item.str("screen"));
    m.package = item.str("package");
    if (m.package.empty()) {
      m.package = m.id.rfind("ad40.", 0) == 0 || m.id.rfind("classic.", 0) == 0 ? "deluxe" : "other";
    }
    m.package_title = item.str("packageTitle", m.package);
    m.same_as = item.str("sameAs");
    m.host_env = host_env_of(item);
    out.modules.push_back(std::move(m));
  }
  return true;
}

const Module* resolve_module(const Catalog& c, const std::string& name, std::vector<const Module*>* ambiguous) {
  if (ambiguous) ambiguous->clear();
  if (name.empty()) return nullptr;
  const std::string want = lower(name);
  const std::string want_path = norm_path(name);
  const std::function<bool(const Module&)> rules[] = {
      [&](const Module& m) { return lower(m.id) == want; },
      [&](const Module& m) { return norm_path(m.path) == want_path; },
      [&](const Module& m) { return lower(m.display_name) == want || lower(m.name) == want; },
      [&](const Module& m) {
        const size_t dot = m.id.find('.');
        return dot != std::string::npos && lower(m.id.substr(dot + 1)) == want;
      },
  };
  // The first rule that matches anything decides.
  for (const auto& matches : rules) {
    std::vector<const Module*> hits;
    for (const Module& m : c.modules) {
      if (matches(m)) hits.push_back(&m);
    }
    if (hits.empty()) continue;
    if (hits.size() == 1) return hits[0];
    // Several: when all but one are byte-identical copies of it (catalog
    // "sameAs"), they are one module, and the name means that one.
    const Module* first = nullptr;
    size_t originals = 0;
    for (const Module* m : hits) {
      if (m->same_as.empty()) {
        ++originals;
        first = m;
      }
    }
    bool all_copies_of_first = originals == 1;
    for (const Module* m : hits) {
      if (all_copies_of_first && m != first && m->same_as != first->id) all_copies_of_first = false;
    }
    if (all_copies_of_first) return first;
    if (ambiguous) *ambiguous = hits;
    return nullptr;
  }
  return nullptr;
}

}  // namespace lad
