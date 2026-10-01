#include "releases.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "geometry.h"
#include "paths.h"

namespace adw::scr {

namespace {

std::string lower(std::string s) {
  for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

// "packages/ad10/AD10TH/baddog3.ad" -> "BADDOG3"
std::string upper_stem(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = file.find_last_of('.');
  if (dot != std::string::npos && dot > 0) file.resize(dot);
  for (char& ch : file) ch = (char)std::toupper((unsigned char)ch);
  return file;
}

int compare_names(const std::wstring& a, const std::wstring& b) {
  const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(), -1, b.c_str(),
                                -1, nullptr, nullptr, 0);
  return r == CSTR_LESS_THAN ? -1 : r == CSTR_GREATER_THAN ? 1 : 0;
}

} // namespace

// ---- the filter ------------------------------------------------------------------------

bool strip_shown(const Catalog& c) { return c.releases.size() >= 2; }

std::vector<std::string> effective_collections(const std::vector<std::string>& saved, const Catalog& c) {
  std::vector<std::string> f;
  for (const Release& r : c.releases) {
    if (std::find(saved.begin(), saved.end(), r.id) != saved.end()) f.push_back(r.id);
  }
  if (f.size() >= c.releases.size()) f.clear();
  return f;
}

bool in_filter(const Catalog& c, const Module& m, const std::vector<std::string>& f) {
  if (f.empty()) return true;
  const std::string& id = m.release >= 0 && m.release < (int)c.releases.size() ? c.releases[m.release].id : m.package;
  return std::find(f.begin(), f.end(), id) != f.end();
}

// ---- what Random plays -------------------------------------------------------------------

const std::string& same_as_key(const Module& m) { return m.same_as.empty() ? m.id : m.same_as; }

RotationPlan effective_rotation(const Settings& s, const Catalog& c,
                                const std::function<bool(const std::string&)>& available) {
  RotationPlan plan;
  auto ok = [&](const Module& m) { return !available || available(m.id); };
  // 1. Randomize (in catalog order), or everything when it names nothing we have.
  std::set<std::string> wanted(s.randomize.begin(), s.randomize.end());
  std::vector<const Module*> base;
  for (const Module& m : c.modules) {
    if (wanted.count(m.id) && ok(m)) base.push_back(&m);
  }
  if (base.empty()) {
    for (const Module& m : c.modules) {
      if (ok(m)) base.push_back(&m);
    }
  }
  // 2. The releases selected.
  const std::vector<std::string> f = effective_collections(s.collections, c);
  std::vector<const Module*> kept;
  for (const Module* m : base) {
    if (in_filter(c, *m, f)) kept.push_back(m);
  }
  // 4. Nothing checked in them: Collections gives way.
  if (kept.empty() && !base.empty()) {
    kept = base;
    plan.collections_ignored = true;
  }
  // 5. The lead, when the file names one in front of its list.
  if (s.has_lead()) {
    const Module* lead = c.find(s.module);
    if (lead && ok(*lead)) plan.lead = lead->id;
  }
  // 3. One per set of identical bytes, the first in catalog order; the lead
  // stands in for its own copy.
  const Module* lead = plan.lead.empty() ? nullptr : c.find(plan.lead);
  std::set<std::string> keys;
  for (const Module* m : kept) {
    const std::string& key = same_as_key(*m);
    if (!keys.insert(key).second) continue;
    plan.ids.push_back(lead && same_as_key(*lead) == key ? lead->id : m->id);
  }
  return plan;
}

bool rotation_needs_capabilities(const Settings& s, const Catalog& c,
                                 const std::function<bool(const std::string&)>& available) {
  if (!s.rotates()) return false;
  const RotationPlan plan = effective_rotation(s, c, available);
  auto other_abi = [&](const std::string& id) {
    const Module* m = c.find(id);
    return m && m->abi != kAfterDarkAbi;
  };
  return std::any_of(plan.ids.begin(), plan.ids.end(), other_abi) || (!plan.lead.empty() && other_abi(plan.lead));
}

HostRotation rotation_for_host(const Settings& s, const Catalog& c,
                               const std::function<bool(const std::string&)>& available,
                               const std::function<bool(const std::string&)>& runs) {
  auto have = [&](const std::string& id) { return !available || available(id); };
  auto can = [&](const std::string& id) { return !runs || runs(id); };
  HostRotation r;
  r.plan = effective_rotation(s, c, [&](const std::string& id) { return have(id) && can(id); });
  // What the host's answer took away: counted against the rotation without
  // it, not the whole catalog (a list of two modules loses at most two).
  const RotationPlan all = effective_rotation(s, c, have);
  std::set<std::string> gone;
  for (const std::string& id : all.ids) {
    if (!can(id)) gone.insert(id);
  }
  if (!all.lead.empty() && !can(all.lead)) gone.insert(all.lead);
  r.left_out = gone.size();
  return r;
}

std::set<SizeI> first_module_screens(const Settings& s, const Catalog& c,
                                     const std::function<bool(const std::string&)>& available) {
  auto ok = [&](const Module& m) { return !available || available(m.id); };
  std::set<SizeI> screens;
  auto add = [&](const Module& m) { screens.insert(own_screen(m.abi, m.screen)); };
  auto every_available = [&] {
    for (const Module& m : c.modules) {
      if (ok(m)) add(m);
    }
  };
  if (!s.rotates()) {
    const Module* m = c.find(s.module);
    if (m && ok(*m)) add(*m);
    else every_available();   // gone (a re-import, a hand edit): the saver shows one of those there are
    return screens;
  }
  if (rotation_needs_capabilities(s, c, available)) {
    every_available();
    return screens;
  }
  const RotationPlan plan = effective_rotation(s, c, available);
  for (const std::string& id : plan.ids) {
    if (const Module* m = c.find(id)) add(*m);
  }
  if (const Module* lead = plan.lead.empty() ? nullptr : c.find(plan.lead)) add(*lead);
  if (screens.empty()) every_available();   // an empty rotation plays every module (saver.cc)
  return screens;
}

// ---- the module list ------------------------------------------------------------------------

std::vector<int> ListModel::order() const {
  std::vector<int> o;
  for (const ListGroup& g : groups) {
    for (const ListRow& r : g.rows) o.push_back(r.module);
  }
  return o;
}

bool ListModel::shows(int module) const {
  for (const ListGroup& g : groups) {
    for (const ListRow& r : g.rows) {
      if (r.module == module) return true;
    }
  }
  return false;
}

const ListGroup* ListModel::group(int release) const {
  for (const ListGroup& g : groups) {
    if (g.release == release) return &g;
  }
  return nullptr;
}

ListModel build_list(const Catalog& c, const std::vector<std::string>& filter) {
  ListModel L;
  L.total = c.modules.size();
  std::vector<std::wstring> names;
  names.reserve(c.modules.size());
  for (const Module& m : c.modules) names.push_back(widen(m.name));
  for (size_t ri = 0; ri < c.releases.size(); ++ri) {
    ListGroup g;
    g.release = (int)ri;
    for (size_t i = 0; i < c.modules.size(); ++i) {
      const Module& m = c.modules[i];
      if (m.release == (int)ri && in_filter(c, m, filter)) g.rows.push_back(ListRow{(int)i, m.name});
    }
    if (g.rows.empty()) continue;
    // Two builds under one name (an AD4 and a Classic "Bad Dog!"): the
    // Classic one says so; if that still leaves two alike, the file stems.
    std::map<std::string, int> seen;
    for (const ListRow& r : g.rows) ++seen[lower(r.label)];
    for (ListRow& r : g.rows) {
      if (seen[lower(r.label)] > 1 && c.modules[r.module].lane == "ne16") r.label += " (Classic)";
    }
    seen.clear();
    for (const ListRow& r : g.rows) ++seen[lower(r.label)];
    for (ListRow& r : g.rows) {
      if (seen[lower(r.label)] > 1) r.label = c.modules[r.module].name + " (" + upper_stem(c.modules[r.module].path) + ")";
    }
    std::stable_sort(g.rows.begin(), g.rows.end(), [&](const ListRow& a, const ListRow& b) {
      if (int k = compare_names(names[a.module], names[b.module])) return k < 0;
      // One name: the AD4 build first, then by what the rows say.
      const bool ca = c.modules[a.module].lane == "ne16", cb = c.modules[b.module].lane == "ne16";
      if (ca != cb) return !ca;
      return compare_names(widen(a.label), widen(b.label)) < 0;
    });
    L.shown += g.rows.size();
    L.groups.push_back(std::move(g));
  }
  return L;
}

int details_after_filter(const ListModel& list, int chosen, int shown) {
  if (chosen >= 0 && list.shows(chosen)) return chosen;
  if (shown >= 0 && list.shows(shown)) return shown;
  for (const ListGroup& g : list.groups) {
    if (!g.rows.empty()) return g.rows.front().module;
  }
  return -1;
}

std::vector<std::string> also_on(const Catalog& c, int module) {
  std::vector<std::string> titles;
  if (module < 0 || module >= (int)c.modules.size()) return titles;
  const Module& m = c.modules[module];
  const std::string& key = same_as_key(m);
  std::vector<bool> on(c.releases.size(), false);
  for (size_t i = 0; i < c.modules.size(); ++i) {
    const Module& o = c.modules[i];
    if ((int)i == module || same_as_key(o) != key) continue;
    if (o.release >= 0 && o.release < (int)on.size() && o.release != m.release) on[o.release] = true;
  }
  for (size_t r = 0; r < on.size(); ++r) {
    if (on[r]) titles.push_back(c.releases[r].title);
  }
  return titles;
}

// ---- words ----------------------------------------------------------------------------

std::wstring strip_status(size_t selected, size_t releases) {
  if (selected == 0 || releases == 0) return L"Click covers to filter the list";
  if (selected >= releases) return L"Showing all " + std::to_wstring(releases) + L" releases";
  return L"Showing " + std::to_wstring(selected) + L" of " + std::to_wstring(releases) +
         (releases == 1 ? L" release" : L" releases");
}

std::wstring modules_count_label(size_t shown, size_t total) {
  if (total == 0) return L"";
  if (shown >= total) return std::to_wstring(total);
  return std::to_wstring(shown) + L" of " + std::to_wstring(total);
}

namespace {
std::wstring savers(size_t n) { return std::to_wstring(n) + (n == 1 ? L" screen saver" : L" screen savers"); }
} // namespace

std::wstring tile_name(const Release& r, size_t modules) {
  std::wstring t = widen(r.title);
  for (size_t p = 0; (p = t.find(L'&', p)) != std::wstring::npos; p += 2) t.insert(p, 1, L'&');
  return t + L", " + savers(modules);
}

std::wstring tile_tip(const Release& r, size_t modules) {
  return widen(r.title) + L" — " + savers(modules) +
         L"\r\nClick to show only this release’s screen savers, or several releases at once. Right-click to change "
         L"its cover or remove it.";
}

std::wstring also_on_tip(const std::vector<std::string>& titles) {
  if (titles.empty()) return L"";
  std::wstring t = L"Also on: ";
  for (size_t i = 0; i < titles.size(); ++i) t += (i ? L", " : L"") + widen(titles[i]);
  return t;
}

} // namespace adw::scr
