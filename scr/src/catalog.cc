#include "catalog.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include <phosg/JSON.hh>

#include "paths.h"
#include "settings.h"

namespace adw::scr {

namespace {

std::string str_or(const phosg::JSON& o, const char* key, const std::string& fallback = {}) {
  if (!o.contains(key)) return fallback;
  const auto& v = o.at(key);
  return v.is_string() ? v.as_string() : fallback;
}

int int_or(const phosg::JSON& o, const char* key, int fallback) {
  if (!o.contains(key)) return fallback;
  const auto& v = o.at(key);
  if (v.is_int()) return (int)v.as_int();
  if (v.is_float()) return (int)v.as_float();
  if (v.is_bool()) return v.as_bool() ? 1 : 0;
  return fallback;
}

ControlType control_type(const std::string& t) {
  if (t == "slider") return ControlType::slider;
  if (t == "checkbox") return ControlType::checkbox;
  if (t == "popup") return ControlType::popup;
  if (t == "button") return ControlType::button;
  return ControlType::unknown;
}

// A catalog "screen" ("640x480"): 1 to 5 decimal digits either side of an 'x'
// (or 'X'). The host renders any size up to 16384 on an axis, but the stream
// parser reads back no frame past 8192 on an axis or 4096x4096 pixels in all
// (frame_parser.cc): anything else, or another shape, is no screen at all
// ({0, 0}), and the module's ABI decides its screen as before. An axis of 6
// digits or more is none whatever its value ("000640x480"), so none can
// overflow an int on its way to the 8192 check ("4294967936" would wrap to
// 640).
SizeI screen_of(const std::string& s) {
  const size_t x = s.find_first_of("xX");
  if (x == std::string::npos) return {};
  auto number = [](std::string_view d, int& out) {
    if (d.empty() || d.size() > 5) return false;   // 5 digits hold every size allowed, and never overflow
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

} // namespace

bool Control::settable() const {
  switch (type) {
    case ControlType::slider:
    case ControlType::checkbox: return true;
    case ControlType::popup: return !items.empty();
    default: return false;
  }
}

int Control::stop_count() const { return stepped() ? (int)items.size() : 0; }

int Control::stop_of(int value) const {
  int n = stop_count();
  if (n == 0) return 0;
  if (values.empty()) return std::clamp(value - min, 0, n - 1);
  int stop = 0;
  for (int i = 0; i < n; ++i) {
    if (values[i] <= value) stop = i;
  }
  return stop;
}

int Control::value_of_stop(int stop) const {
  int n = stop_count();
  if (n == 0) return min;
  stop = std::clamp(stop, 0, n - 1);
  return values.empty() ? min + stop : values[stop];
}

int Control::clamp(int v) const {
  switch (type) {
    case ControlType::checkbox: return v ? 1 : 0;
    case ControlType::popup:
      return items.empty() ? v : std::clamp(v, min, min + (int)items.size() - 1);
    case ControlType::slider:
      if (stepped()) return value_of_stop(stop_of(v));
      return std::clamp(v, std::min(min, max), std::max(min, max));
    default: return v;
  }
}

std::string Control::value_label(int value) const {
  if (stepped()) return items[stop_of(value)];
  std::string n = std::to_string(value);
  if (type != ControlType::slider || unit.empty()) return n;
  return unit_prefix ? unit + n : n + unit;
}

const Control* Module::control(int index) const {
  for (const auto& c : controls) if (c.index == index) return &c;
  return nullptr;
}

bool host_variable_ok(std::string_view name) {
  // What the front end sets at every start (the saver's windows, the
  // dialog's live preview, thumbnails and module buttons; the Linux player's
  // own two): never a catalog's to change.
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

HostControlValues host_control_values(const Module& m, const std::map<int, int>* values) {
  HostControlValues r;
  std::map<int, int> cv;
  if (values) {
    // The catalog is authoritative about which slots exist and their ranges;
    // buttons and unknown kinds never carry a value, nor do host controls.
    for (auto [idx, val] : *values) {
      if (const Control* c = m.control(idx); c && c->settable() && !c->for_host()) cv[idx] = c->clamp(val);
    }
  }
  r.cvset = format_cvset(cv);
  // Controls are sorted by index (parse_catalog).
  for (const Control& c : m.controls) {
    if (!c.for_host() || !c.settable()) continue;
    int v = c.def;
    if (values) {
      if (auto it = values->find(c.index); it != values->end()) v = c.clamp(it->second);
    }
    r.env.emplace_back(widen(c.host), std::to_wstring(v));
  }
  return r;
}

HostControlValues host_control_values(const Module& m, const std::map<std::string, std::map<int, int>>& all) {
  auto it = all.find(m.id);
  return host_control_values(m, it == all.end() ? nullptr : &it->second);
}

std::string describe_env(const std::vector<std::pair<std::wstring, std::wstring>>& env) {
  std::string s;
  for (const auto& [k, v] : env) {
    if (!s.empty()) s += ",";
    s += narrow(k) + "=" + narrow(v);
  }
  return s;
}

const Module* Catalog::find(const std::string& id) const {
  for (const auto& m : modules) if (m.id == id) return &m;
  return nullptr;
}

const Module* resolve_module(const Catalog& c, const std::string& name, std::vector<const Module*>* ambiguous) {
  if (ambiguous) ambiguous->clear();
  if (name.empty()) return nullptr;
  auto path_of = [](std::string s) {
    for (char& ch : s) if (ch == '\\') ch = '/';
    return s;
  };
  const std::string want_path = path_of(name);
  const std::function<bool(const Module&)> rules[] = {
      [&](const Module& m) { return iequals(m.id, name); },
      [&](const Module& m) { return iequals(path_of(m.path), want_path); },
      [&](const Module& m) {
        return iequals(m.display_name, name) || iequals(m.name, name) ||
               (!m.module_name.empty() && iequals(m.module_name, name));
      },
      [&](const Module& m) {
        const size_t dot = m.id.find('.');
        return dot != std::string::npos && iequals(std::string_view(m.id).substr(dot + 1), name);
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
    // Several: when all but one are byte-identical copies of it (sameAs),
    // they are one module, and the name means that one.
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

int Catalog::release_index(const std::string& id) const {
  for (size_t i = 0; i < releases.size(); ++i) {
    if (releases[i].id == id) return (int)i;
  }
  return -1;
}

size_t Catalog::modules_in(int index) const {
  return (size_t)std::count_if(modules.begin(), modules.end(), [&](const Module& m) { return m.release == index; });
}

namespace {

// A few Classic modules' names were cut to fit the Windows 3.x control panel
// (15 characters) or run together; in a list they read as typos. Only the
// name shown changes: ids, and so settings.ini, stay as they are.
// Classic module names the Windows 3.x control panel cut short, shown whole
// (INTERACTION.md §9.3). Keyed on the module's own name (catalog
// `moduleName`, else `displayName`), so every package's copy is fixed; only
// that leading part of the display name is replaced, keeping a package
// suffix: "SlideShow (After Dark 3.2)" -> "Slide Show (After Dark 3.2)".
// AD4 modules are named in full already.
std::string display_name_of(const std::string& lane, const std::string& module_name, const std::string& name) {
  static const struct {
    const char *catalog, *shown;
  } kNames[] = {
      {"Strange Attract", "Strange Attractors"},
      {"ConfettiFactory", "Confetti Factory"},
      {"SlideShow", "Slide Show"},
      {"Om Appliances", "OM Appliances"},
  };
  if (lane != "ne16") return name;
  const std::string& key = module_name.empty() ? name : module_name;
  for (const auto& n : kNames) {
    if (key != n.catalog) continue;
    const size_t len = strlen(n.catalog);
    // The display name starts with the module's name (a package copy adds
    // " (<package>)"); anything else is left as the catalog wrote it.
    if (name.compare(0, len, n.catalog) == 0 && (name.size() == len || name[len] == ' ')) {
      return n.shown + name.substr(len);
    }
  }
  return name;
}

// A module without `package` (a catalog from before packages): the Deluxe
// disc's ids keep their old prefixes (DESIGN.md §6a).
std::string package_of(const std::string& id, const std::string& package) {
  if (!package.empty()) return package;
  if (id.rfind("ad40.", 0) == 0 || id.rfind("classic.", 0) == 0) return "deluxe";
  return "other";
}

// A release the catalog doesn't describe: titled after its modules
// (COVERS.md §1.10), with a generated cover.
Release made_up_release(const std::string& id, const std::string& package_title) {
  Release r;
  r.id = id;
  r.title = !package_title.empty() ? package_title : id == "deluxe" ? "After Dark 4.0 Deluxe" : "Other modules";
  r.short_title = r.title;
  return r;
}

Cover parse_cover(const phosg::JSON& o) {
  Cover c;
  if (!o.is_dict()) return c;
  c.origin = str_or(o, "origin", "generated");
  c.tile = str_or(o, "tile");
  c.tile_md5 = str_or(o, "tileMd5");
  c.image = str_or(o, "image");
  c.art = str_or(o, "art");
  c.label = str_or(o, "label");
  c.credit = str_or(o, "credit");
  c.original = str_or(o, "original");
  c.width = std::max(0, int_or(o, "width", 0));
  c.height = std::max(0, int_or(o, "height", 0));
  if (c.origin != "user" && c.origin != "download" && c.origin != "disc") c.origin = "generated";
  if (c.generated()) {
    // Nothing to show but the generated cover: nothing else is kept.
    c = Cover{};
  }
  return c;
}

void parse_releases(const phosg::JSON& root, Catalog& out) {
  if (root.contains("packages") && root.at("packages").is_list()) {
    out.has_packages = true;
    for (const auto& pp : root.at("packages").as_list()) {
      const phosg::JSON& p = *pp;
      if (!p.is_dict()) continue;
      Release r;
      r.id = str_or(p, "id");
      if (r.id.empty() || out.release_index(r.id) >= 0) continue;
      r.title = str_or(p, "title", r.id);
      if (r.title.empty()) r.title = r.id;
      r.short_title = str_or(p, "shortTitle", r.title);
      if (r.short_title.empty()) r.short_title = r.title;
      r.modules = std::max(0, int_or(p, "modules", 0));
      if (p.contains("cover")) r.cover = parse_cover(p.at("cover"));
      out.releases.push_back(std::move(r));
    }
  }
  // Every module belongs to a listed release: one the array leaves out (or
  // every one, without an array) gets an entry of its own, in order of
  // first appearance.
  for (Module& m : out.modules) {
    int i = out.release_index(m.package);
    if (i < 0) {
      out.releases.push_back(made_up_release(m.package, m.package_title));
      i = (int)out.releases.size() - 1;
    }
    m.release = i;
  }
}

} // namespace

bool parse_catalog(const std::string& json_text, Catalog& out, std::string* error) {
  out = Catalog{};
  try {
    phosg::JSON root = phosg::JSON::parse(json_text);
    if (!root.is_dict()) throw std::runtime_error("top level is not an object");
    out.version = int_or(root, "version", 0);
    if (!root.contains("modules") || !root.at("modules").is_list()) throw std::runtime_error("no \"modules\" list");
    for (const auto& mp : root.at("modules").as_list()) {
      const phosg::JSON& m = *mp;
      if (!m.is_dict()) continue;
      Module mod;
      mod.id = str_or(m, "id");
      mod.path = str_or(m, "path");
      if (mod.id.empty() || mod.path.empty()) continue;   // unusable entry; skip, don't fail the catalog
      mod.lane = str_or(m, "lane");
      // Written only for a module whose ABI is not After Dark's (the importer
      // puts it last); absent, empty or not a string, it is After Dark's.
      mod.abi = str_or(m, "abi");
      if (mod.abi.empty()) mod.abi = kAfterDarkAbi;
      // A fixed screen of its own (PACKAGES.md §6), for a module that
      // composes a scene of that size: absent, not a string or not a size
      // it can use, it has none.
      mod.screen = screen_of(str_or(m, "screen"));
      mod.module_name = str_or(m, "moduleName");
      mod.display_name = display_name_of(mod.lane, mod.module_name, str_or(m, "displayName", mod.id));
      if (mod.display_name.empty()) mod.display_name = mod.id;
      mod.name = mod.module_name.empty() ? mod.display_name : display_name_of(mod.lane, mod.module_name, mod.module_name);
      mod.package = package_of(mod.id, str_or(m, "package"));
      mod.package_title = str_or(m, "packageTitle");
      mod.same_as = str_or(m, "sameAs");
      if (mod.same_as == mod.id) mod.same_as.clear();
      mod.about = str_or(m, "about");
      mod.credits = str_or(m, "credits");
      if (m.contains("controls") && m.at("controls").is_list()) {
        for (const auto& cp : m.at("controls").as_list()) {
          const phosg::JSON& c = *cp;
          if (!c.is_dict() || !c.contains("index")) continue;
          Control ctl;
          ctl.index = int_or(c, "index", -1);
          if (ctl.index < 0) continue;
          // A host control (catalog.h): absent or "", the module's own. A
          // name it may not set, or one an earlier control took, can do
          // nothing: the control is left out.
          if (c.contains("host")) {
            const phosg::JSON& h = c.at("host");
            if (!h.is_string()) continue;
            ctl.host = h.as_string();
            if (!ctl.host.empty()) {
              if (!host_variable_ok(ctl.host)) continue;
              if (std::any_of(mod.controls.begin(), mod.controls.end(),
                              [&](const Control& o) { return o.host == ctl.host; })) continue;
            }
          }
          ctl.name = str_or(c, "name", "Control " + std::to_string(ctl.index));
          ctl.type_name = str_or(c, "type");
          ctl.type = control_type(ctl.type_name);
          ctl.min = int_or(c, "min", 0);
          ctl.max = int_or(c, "max", ctl.type == ControlType::checkbox ? 1 : 100);
          if (c.contains("items") && c.at("items").is_list()) {
            for (const auto& ip : c.at("items").as_list()) {
              if (ip->is_string()) ctl.items.push_back(ip->as_string());
            }
          }
          int def = int_or(c, "default", ctl.min);
          const int default_stop = int_or(c, "defaultStop", -1);
          if (ctl.type == ControlType::slider && !ctl.items.empty()) {
            // A string slider: one value per labelled stop. A table that
            // doesn't line up with the labels keeps only the stops that have
            // both; values must ascend for stop_of() to mean anything.
            // `defaultStop` indexes the table as written, so it is resolved
            // to a value before any reordering.
            if (c.contains("values") && c.at("values").is_list()) {
              for (const auto& vp : c.at("values").as_list()) {
                if (vp->is_int()) ctl.values.push_back((int)vp->as_int());
                else if (vp->is_float()) ctl.values.push_back((int)vp->as_float());
                else break;
              }
              if (!ctl.values.empty()) {
                size_t n = std::min(ctl.values.size(), ctl.items.size());
                ctl.values.resize(n);
                ctl.items.resize(n);
                if (default_stop >= 0 && default_stop < (int)n) def = ctl.values[default_stop];
                const int bold_stop = int_or(c, "boldStop", -1);
                if (bold_stop >= 0 && bold_stop < (int)n) {
                  ctl.has_bold = true;
                  ctl.bold_value = ctl.values[bold_stop];
                }
                if (!std::is_sorted(ctl.values.begin(), ctl.values.end())) {
                  std::vector<std::pair<int, std::string>> stops;
                  for (size_t i = 0; i < n; ++i) stops.emplace_back(ctl.values[i], ctl.items[i]);
                  std::stable_sort(stops.begin(), stops.end(),
                                   [](const auto& a, const auto& b) { return a.first < b.first; });
                  for (size_t i = 0; i < n; ++i) std::tie(ctl.values[i], ctl.items[i]) = stops[i];
                }
              }
            }
            ctl.min = ctl.values.empty() ? ctl.min : ctl.values.front();
            ctl.max = ctl.values.empty() ? ctl.min + (int)ctl.items.size() - 1 : ctl.values.back();
            if (ctl.values.empty() && default_stop >= 0 && default_stop < (int)ctl.items.size()) {
              def = ctl.min + default_stop;
            }
            if (ctl.values.empty()) {
              const int bold_stop = int_or(c, "boldStop", -1);
              if (bold_stop >= 0 && bold_stop < (int)ctl.items.size()) {
                ctl.has_bold = true;
                ctl.bold_value = ctl.min + bold_stop;
              }
            }
          }
          if (ctl.type == ControlType::slider) {
            ctl.unit = str_or(c, "unit");
            ctl.unit_prefix = str_or(c, "unitPos") == "prefix";
            if (str_or(c, "unitPos") == "none") ctl.unit.clear();
          }
          ctl.def = ctl.clamp(def);
          if (ctl.for_host() && !ctl.settable()) continue;   // a host control without a value
          mod.controls.push_back(std::move(ctl));
        }
        std::sort(mod.controls.begin(), mod.controls.end(),
                  [](const Control& a, const Control& b) { return a.index < b.index; });
      }
      if (!out.find(mod.id)) out.modules.push_back(std::move(mod));
    }
    parse_releases(root, out);
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    out = Catalog{};
    return false;
  }
  return true;
}

bool load_catalog(const std::wstring& path, Catalog& out, std::string* error) {
  std::string text;
  if (!read_file(path, text)) {
    if (error) *error = "cannot read " + narrow(path);
    out = Catalog{};
    return false;
  }
  return parse_catalog(text, out, error);
}

} // namespace adw::scr
