#include "catalog.h"

#include <fstream>
#include <functional>
#include <sstream>
#include <string_view>

#include "minijson.h"

namespace lad {

namespace {

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

}  // namespace

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
  using Kind = adw::import::JsonValue::Kind;
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
