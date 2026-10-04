#include "window_mode.h"

#include "paths.h"
#include "releases.h"

namespace adw::scr {

SizeI window_size_for_client(SizeI client, UINT dpi) {
  RECT r{0, 0, client.w, client.h};
  if (!AdjustWindowRectExForDpi(&r, kWindowModeStyle, FALSE, kWindowModeExStyle, dpi)) return client;
  return {(int)(r.right - r.left), (int)(r.bottom - r.top)};
}

Settings window_settings(const Settings& s, const Catalog& c, const std::string& module, bool random) {
  if (module.empty() && !random) return s;
  Settings w = s;
  if (!random) {
    w.module = module;
    w.randomize.clear();   // a list would make it rotate
    return w;
  }
  w.module = module.empty() ? std::string("random") : module;
  w.randomize = dialog_checklist_none(s) ? std::vector<std::string>{} : dialog_checklist(s);
  // A named module leads a list (Settings::has_lead); an empty one would
  // leave it alone, so "every module" is spelled out.
  if (!module.empty() && w.randomize.empty()) {
    for (const Module& m : c.modules) w.randomize.push_back(m.id);
  }
  return w;
}

WindowModule window_module(const Catalog& c, const std::wstring& name,
                           const std::function<bool(const std::string&)>& available) {
  WindowModule r;
  if (c.modules.empty()) {
    r.error = L"No release is imported yet, so there is no module \"" + name +
              L"\". Import one first, with adimport.exe or Import… in the settings window.";
    return r;
  }
  std::vector<const Module*> several;
  const Module* m = resolve_module(c, narrow(name), &several);
  if (!m && several.empty()) {
    // The settings list tells two builds of one name apart ("Bad Dog!
    // (Classic)", releases.cc build_list): a row's label names its module.
    const std::string want = narrow(name);
    std::vector<const Module*> rows;
    for (const ListGroup& g : build_list(c, {}).groups) {
      for (const ListRow& row : g.rows) {
        if (iequals(row.label, want)) rows.push_back(&c.modules[(size_t)row.module]);
      }
    }
    if (rows.size() == 1) m = rows.front();
    else several = rows;
  }
  if (!m && !several.empty()) {
    std::wstring ids;
    for (const Module* x : several) ids += (ids.empty() ? L"" : L", ") + widen(x->id);
    r.error = L"\"" + name + L"\" names several modules (" + ids + L"): name one by its id, as in /module " +
              widen(several.front()->id) + L".";
    return r;
  }
  if (!m) {
    // The examples: the first module whose name means it alone ("Bad Dog!"
    // names three).
    const Module* e = &c.modules.front();
    for (const Module& x : c.modules) {
      if (resolve_module(c, x.name, nullptr) == &x) {
        e = &x;
        break;
      }
    }
    r.error = L"There is no module \"" + name + L"\" among the " + std::to_wstring(c.modules.size()) +
              L" imported. Name one by its id (such as " + widen(e->id) +
              L") or by its name as the settings window lists it (such as \"" + widen(e->name) + L"\").";
    return r;
  }
  if (available && !available(m->id)) {
    r.error = L"\"" + widen(m->name) + L"\" (" + widen(m->id) +
              L") is in the catalog, but its file is missing: import its release again.";
    return r;
  }
  r.module = m;
  return r;
}

} // namespace adw::scr
