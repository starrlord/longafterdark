// adimport's windows (COVERS.md §4): the flow from the source chooser through
// progress to the result, the cover window, and the screenshot hook.
#include "gui.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

#include "adw/ui/capture.h"
#include "pages.h"
#include "winutil.h"

namespace adw::import::gui {

namespace fs = std::filesystem;

namespace {

std::wstring env(const wchar_t* name) {
  const wchar_t* v = _wgetenv(name);
  return v ? v : L"";
}

// The windows follow one another: each opens where the last one was, before
// the last one closes (so there is always exactly one "Long After Dark").
class Flow {
 public:
  template <typename P>
  P* go(std::unique_ptr<P> next) {
    P* p = next.get();
    if (!next->open(cur_.get())) return nullptr;
    cur_ = std::move(next);
    return p;
  }

 private:
  std::unique_ptr<Page> cur_;
};

// "Get the covers": refresh_covers in a progress window, then (unless
// AD_GUI_AUTOCLOSE) what it got. Returns false when a window could not open.
bool run_covers(Flow& flow, Session& s, Job job) {
  job.covers_job = true;
  ProgressPage* prog = flow.go(std::make_unique<ProgressPage>(s, std::move(job)));
  if (!prog) return false;
  prog->run();
  const std::vector<CoverResult> rs = prog->cover_results();
  for (const CoverResult& cr : rs) {
    if (cr.status == Status::ok) {
      if (cr.changed) s.tally.cover_changed();
    } else {
      s.tally.import_result(cr.status);   // the first failure, unless something changed
    }
  }
  const bool cancelled = std::any_of(rs.begin(), rs.end(), [](const CoverResult& cr) { return cr.status == Status::cancelled; });
  if (!s.autoclose && !cancelled) {
    ResultPage* rp = flow.go(std::make_unique<ResultPage>(s, covers_result(rs)));
    if (rp) rp->run();
  }
  return true;
}

int run_flow(Session& s) {
  const Request& r = s.req;
  Flow flow;
  if (r.refresh_covers) {
    // adimport --gui --refresh-covers [<id> | all]: the progress window alone.
    Job job;
    if (!r.refresh_id.empty()) job.covers = {r.refresh_id};
    job.force = r.force;
    if (!run_covers(flow, s, std::move(job))) return int(Status::error);
    return s.tally.exit_code();
  }
  if (!r.change_cover.empty()) {
    CoverPage* cp = flow.go(std::make_unique<CoverPage>(s, r.change_cover));
    if (!cp) return int(Status::error);
    cp->run();
    // COVERS.md §2.11: 0 when a cover changed, else 5 (errors were shown in the window).
    return s.tally.changed ? 0 : int(Status::cancelled);
  }
  if (!r.remove.empty()) {
    // adimport --gui --remove <id> (the settings dialog's "Remove …"): the
    // window that asks, and removes it. 0 when it was removed, else the
    // failure it showed, else 5.
    RemovePage* rp = flow.go(std::make_unique<RemovePage>(s, r.remove));
    if (!rp) return int(Status::error);
    rp->run();
    return s.tally.exit_code();
  }

  Job job;
  bool have_job = false;
  if (r.source) {
    job.source = *r.source;
    if (r.download_all) job.all = downloadable_packages();
    have_job = true;
  }
  while (!have_job) {
    SourcesPage* src = flow.go(std::make_unique<SourcesPage>(s));
    if (!src) return int(Status::error);
    src->run();
    switch (src->choice) {
      case SourcesPage::Choice::cancel:
        return s.tally.exit_code();
      case SourcesPage::Choice::image:
        job.source.kind = Source::Kind::image;
        job.source.path = src->paths.front();
        job.source.more_images.assign(src->paths.begin() + 1, src->paths.end());
        job.source.package = r.package;
        have_job = true;
        break;
      case SourcesPage::Choice::folder:
        job.source.kind = Source::Kind::folder;
        job.source.path = src->paths.front();
        job.source.package = r.package;
        have_job = true;
        break;
      case SourcesPage::Choice::change_cover: {
        CoverPage* cp = flow.go(std::make_unique<CoverPage>(s, src->cover_id));
        if (!cp) return int(Status::error);
        cp->run();
        break;   // back to the sources
      }
      case SourcesPage::Choice::remove: {
        RemovePage* rp = flow.go(std::make_unique<RemovePage>(s, src->cover_id));
        if (!rp) return int(Status::error);
        rp->run();
        if (rp->removed()) s.notice = removed_note(rp->title());
        break;   // back to the sources (without it)
      }
      case SourcesPage::Choice::get_covers: {
        Job covers;
        covers.covers = src->cover_ids;
        if (!run_covers(flow, s, std::move(covers))) return int(Status::error);
        break;   // back to the sources
      }
      case SourcesPage::Choice::downloads: {
        DownloadsPage* dp = flow.go(std::make_unique<DownloadsPage>(s));
        if (!dp) return int(Status::error);
        dp->run();
        if (dp->ids.empty()) break;   // Back: to the sources
        job.source.kind = Source::Kind::download;
        job.source.path = r.download_dir;   // --download-dir (empty: the default folder)
        job.source.package = r.package;
        if (dp->ids.size() == 1) job.source.package = dp->ids.front();
        else job.all = dp->ids;
        have_job = true;
        break;
      }
    }
  }

  ProgressPage* prog = flow.go(std::make_unique<ProgressPage>(s, job));
  if (!prog) return int(Status::error);
  prog->run();
  const std::vector<ImportResult> results = prog->results();
  if (results.empty()) return s.tally.changed ? 0 : int(Status::error);
  for (const ImportResult& ir : results) s.tally.import_result(ir.status);

  bool show_result = !s.autoclose;
  ResultText text;
  if (job.all.empty()) {
    // A cancelled import needs no words: nothing changed, and the user knows why.
    if (results.front().status == Status::cancelled) show_result = false;
    text = single_result(results.front());
  } else {
    bool any = false;
    for (const ImportResult& ir : results) any = any || ir.status == Status::ok;
    if (!any && results.size() == 1 && results.back().status == Status::cancelled) show_result = false;
    text = several_result(job.all, results);
  }
  if (show_result) {
    ResultPage* rp = flow.go(std::make_unique<ResultPage>(s, std::move(text)));
    if (rp) rp->run();
  }
  return s.tally.exit_code();
}

// ---- the screenshot hook ------------------------------------------------------------------
// Renders one page without it ever appearing on screen (parked off every
// monitor and cloaked), from synthetic state: no import, no network.
//   AD_IMPORT_TEST_SCREENSHOT=<png>
//   AD_IMPORT_TEST_SCREENSHOT_STATE=key=value;…
//     page=sources|downloads|progress|result|error|cover|remove   theme=light|dark|hc   dpi=<n>
//     focus=<command id>   progress=<0..1>|marquee   phase=download|check_image|copy|verify|cover|finalize
//     result=single|several|partial|covers (page=result)   job=covers (page=progress: "Get the covers")
//     package=<id> (page=cover or remove; default: the first installed)
//     caution=1 (page=sources: the "not a known disc" message)   status=ok|error|network|running (page=cover)
//     status=error|running (page=remove: a removal that failed, or one under way)
//     notice=1 (page=sources: the line a removal leaves, "Removed …")
//     workarea=<w>x<h> (DIPs: the work area the window is fitted to; default unlimited)
//     dpichange=<n> (without dpi=: WM_DPICHANGED as if dragged to a monitor at that DPI)
//     themechange=light|dark|hc (after opening: a live theme change to that mode)
//     report=<path> (where the client area is in the picture, and pal.base; on
//       Sources also list=<shown>,<whole>,<row>,<rows>,<cols>: the installed covers' heights
//       in px, and the grid's rows and columns)

std::map<std::wstring, std::wstring> parse_state(const std::wstring& s) {
  std::map<std::wstring, std::wstring> kv;
  size_t p = 0;
  while (p < s.size()) {
    size_t semi = s.find(L';', p);
    std::wstring part = s.substr(p, semi == std::wstring::npos ? std::wstring::npos : semi - p);
    while (!part.empty() && part.front() == L' ') part.erase(part.begin());
    size_t eq = part.find(L'=');
    if (eq != std::wstring::npos) kv[part.substr(0, eq)] = part.substr(eq + 1);
    else if (!part.empty()) kv[part] = L"1";
    if (semi == std::wstring::npos) break;
    p = semi + 1;
  }
  return kv;
}

ImportResult sample_result(const Session& s, bool ok, const std::string& id) {
  ImportResult r;
  const Package* p = find_package(id);
  r.package_id = id;
  r.package_title = p ? p->title : id;
  r.status = ok ? Status::ok : Status::verify_failed;
  r.verified = "image";
  r.files_dir = win_assets_dir(s.assets) / (p && !p->is_deluxe() ? fs::path(L"packages") / to_wide(id) : fs::path(L"FILES"));
  r.files.resize(ok ? 1266 : 3);
  if (!ok) {
    const char* names[] = {"FILES/AD40/TOASTERS.AD", "FILES/AD40/FISH.AD", "FILES/ENGINE/ADXPL510.DLL"};
    for (size_t i = 0; i < r.files.size(); ++i) {
      r.files[i].path = names[i];
      r.files[i].md5 = "0123456789abcdef0123456789abcdef";
      r.files[i].known = ImportedFile::Known::mismatch;
    }
    r.message = "3 files differ from the known release of " + r.package_title;
    r.source = "E:\\";
  }
  r.package_modules = 84;
  r.catalog_modules = 202;
  for (const Package& q : builtin_packages()) r.installed.push_back(q.title);
  return r;
}

int run_screenshot(Session& s, const std::wstring& png) {
  auto kv = parse_state(env(L"AD_IMPORT_TEST_SCREENSHOT_STATE"));
  const std::wstring page = kv[L"page"].empty() ? L"sources" : kv[L"page"];
  if (kv[L"theme"] == L"dark") s.theme_mode = ui::ThemeMode::dark;
  if (kv[L"theme"] == L"light") s.theme_mode = ui::ThemeMode::light;
  if (kv[L"theme"] == L"hc") s.theme_mode = ui::ThemeMode::high_contrast;
  if (!kv[L"dpi"].empty()) s.forced_dpi = std::clamp(_wtoi(kv[L"dpi"].c_str()), 72, 480);
  if (!kv[L"workarea"].empty()) swscanf(kv[L"workarea"].c_str(), L"%dx%d", &s.work_w, &s.work_h);
  s.offscreen = true;

  std::unique_ptr<Page> pg;
  SourcesPage* src = nullptr;   // page=sources (the report's list heights)
  std::vector<InstalledRow> installed = installed_rows(s.assets);
  if (page == L"sources") {
    if (kv[L"notice"] == L"1") s.notice = removed_note(L"The Simpsons Screen Saver");
    auto sp = std::make_unique<SourcesPage>(s);
    src = sp.get();
    pg = std::move(sp);
    if (!pg->open(nullptr)) return 1;
    if (kv[L"caution"] == L"1")
      src->show_caution(L"That is not a disc Long After Dark knows. D:\\Backup\\Screen savers: no known release is "
                        L"there. Choose the CD drive itself (for example E:\\) or a copy of the disc or floppies.");
  } else if (page == L"downloads") {
    pg = std::make_unique<DownloadsPage>(s);
    if (!pg->open(nullptr)) return 1;
  } else if (page == L"progress") {
    Job job;
    job.source.kind = Source::Kind::download;
    job.all = {"ad10", "ad32", "tt", "simpsons"};
    if (kv[L"job"] == L"covers") {
      // "Get the covers" (no import).
      job = Job{};
      job.covers_job = true;
    }
    auto pp = std::make_unique<ProgressPage>(s, job, false);
    ProgressPage* prog = pp.get();
    pg = std::move(pp);
    if (!pg->open(nullptr)) return 1;
    static const std::map<std::wstring, Progress::Phase> phases = {
        {L"download", Progress::Phase::download}, {L"check_image", Progress::Phase::check_image},
        {L"copy", Progress::Phase::copy},         {L"verify", Progress::Phase::verify},
        {L"cover", Progress::Phase::cover},       {L"finalize", Progress::Phase::finalize}};
    Progress p;
    p.phase = phases.count(kv[L"phase"]) ? phases.at(kv[L"phase"]) : Progress::Phase::download;
    p.package = "After Dark 10th Anniversary";
    const std::wstring amount = kv[L"progress"].empty() ? L"0.4" : kv[L"progress"];
    if (amount == L"marquee") {
      p.total = 0;
      p.done = 0;
    } else {
      p.total = 395214848;
      p.done = (uint64_t)(std::clamp(_wtof(amount.c_str()), 0.0, 1.0) * (double)p.total);
    }
    if (job.covers_job && amount != L"marquee") {
      // A cover download's size (Totally Twisted's box front).
      p.package.clear();
      p.total = 26382;
      p.done = (uint64_t)(std::clamp(_wtof(amount.c_str()), 0.0, 1.0) * (double)p.total);
    }
    p.item = job.covers_job                      ? "Box front from Wayback Machine"
             : p.phase == Progress::Phase::cover ? "Disc label from Internet Archive"
             : p.phase == Progress::Phase::download ? "ad10th.iso"   // the file a download saves
                                                    : "AD10TH/TOAST2K.AD";
    prog->show_progress(p, 0);
  } else if (page == L"result" || page == L"error") {
    const std::string id = installed.empty() ? "deluxe" : installed.front().id;
    ResultText text;
    if (page == L"error") {
      text = single_result(sample_result(s, false, id));
    } else if (kv[L"result"] == L"covers") {
      // "Get the covers": two fetched, one offline.
      std::vector<CoverResult> rs;
      for (const char* cid : {"deluxe", "tt", "ad10"}) {
        CoverResult cr;
        cr.info.package = cid;
        cr.info.origin = CoverOrigin::download;
        cr.info.label = std::string(cid) == "ad10" ? "Disc label" : "Box front";
        cr.info.credit = std::string(cid) == "ad10" ? "Internet Archive" : "Wayback Machine";
        cr.status = std::string(cid) == "ad10" ? Status::network : Status::ok;
        cr.changed = cr.status == Status::ok;
        if (cr.status != Status::ok) cr.message = "the download failed (could not connect to archive.org)";
        rs.push_back(cr);
      }
      text = covers_result(rs);
    } else if (kv[L"result"] == L"several" || kv[L"result"] == L"partial") {
      std::vector<std::string> ids = {"ad10", "ad32", "tt", "simpsons"};
      std::vector<ImportResult> rs;
      for (const std::string& i : ids) rs.push_back(sample_result(s, true, i));
      if (kv[L"result"] == L"partial") {
        rs[2].status = Status::network;
        rs[2].message = "the download stopped after 3 attempts (connection reset)";
      }
      text = several_result(ids, rs);
    } else {
      text = single_result(sample_result(s, true, id));
    }
    pg = std::make_unique<ResultPage>(s, std::move(text));
    if (!pg->open(nullptr)) return 1;
  } else if (page == L"cover") {
    std::string id = kv[L"package"].empty() ? (installed.empty() ? "deluxe" : installed.front().id) : to_utf8(kv[L"package"]);
    auto cp = std::make_unique<CoverPage>(s, id);
    CoverPage* cover = cp.get();
    pg = std::move(cp);
    if (!pg->open(nullptr)) return 1;
    const std::wstring st = kv[L"status"];
    if (st == L"ok") cover->show_status(L"The cover is now your picture.", Ink::text2);
    else if (st == L"error") cover->show_status(L"Windows can't read this picture. Save it as PNG or JPEG and try again.", Ink::critical);
    else if (st == L"network") cover->show_status(L"The download failed; the cover you had is kept.", Ink::caution);
    else if (st == L"running") {
      Progress p;
      p.phase = Progress::Phase::cover;
      p.item = "Box front from Wikisimpsons";
      p.total = 1011608;
      p.done = 404643;
      cover->show_running(p);
    }
  } else if (page == L"remove") {
    std::string id = kv[L"package"].empty() ? (installed.empty() ? "deluxe" : installed.front().id) : to_utf8(kv[L"package"]);
    auto rp = std::make_unique<RemovePage>(s, id);
    RemovePage* remove = rp.get();
    pg = std::move(rp);
    if (!pg->open(nullptr)) return 1;
    const std::wstring st = kv[L"status"];
    if (st == L"running") remove->show_running();
    else if (st == L"error")
      remove->show_failure("cannot remove C:\\Users\\Pat\\AppData\\Local\\LongAfterDark\\assets\\win\\packages\\" + id +
                           " (a file in it is in use \xE2\x80\x94 close Long After Dark and try again): The process cannot "
                           "access the file because it is being used by another process.");
  } else {
    fprintf(stderr, "adimport: unknown screenshot page\n");
    return 1;
  }
  HWND h = pg->hwnd();
  if (!kv[L"dpichange"].empty() && !s.forced_dpi) {
    // As Windows does when the window is dragged to a monitor of another
    // scale: the new DPI and a suggested window rect scaled to match.
    const int to = std::clamp(_wtoi(kv[L"dpichange"].c_str()), 72, 480), from = (int)pg->theme().dpi;
    RECT wr{};
    GetWindowRect(h, &wr);
    RECT sug{wr.left, wr.top, wr.left + MulDiv(wr.right - wr.left, to, from), wr.top + MulDiv(wr.bottom - wr.top, to, from)};
    SendMessageW(h, WM_DPICHANGED, MAKEWPARAM(to, to), (LPARAM)&sug);
  }
  if (!kv[L"themechange"].empty()) {
    // A live switch of the app mode (or high contrast): the page reloads its theme.
    const std::wstring to = kv[L"themechange"];
    s.theme_mode = to == L"dark" ? ui::ThemeMode::dark : to == L"hc" ? ui::ThemeMode::high_contrast : ui::ThemeMode::light;
    SendMessageW(h, WM_THEMECHANGED, 0, 0);
  }
  if (!kv[L"focus"].empty()) pg->show_focus_on(_wtoi(kv[L"focus"].c_str()));
  ui::settle_for_capture(h, 250);
  std::string err;
  POINT origin{};
  bool ok = ui::capture_window_png(h, png, &err, &origin);
  if (!ok) fprintf(stderr, "adimport: screenshot failed: %s\n", err.c_str());
  if (ok && !kv[L"report"].empty()) {
    POINT client{0, 0};
    ClientToScreen(h, &client);
    RECT cr{};
    GetClientRect(h, &cr);
    POINT probe = pg->base_probe();
    const COLORREF base = pg->theme().pal.base;
    char buf[256];
    snprintf(buf, sizeof(buf), "client=%ld,%ld,%ld,%ld\nprobe=%ld,%ld\nbase=%02X%02X%02X\ndpi=%d\n", client.x - origin.x,
             client.y - origin.y, cr.right, cr.bottom, client.x - origin.x + probe.x, client.y - origin.y + probe.y,
             GetRValue(base), GetGValue(base), GetBValue(base), pg->theme().dpi);
    std::string report = buf;
    if (src) {
      const SourcesPage::ListHeights l = src->list_heights();
      snprintf(buf, sizeof(buf), "list=%d,%d,%d,%d,%d\n", l.shown, l.whole, l.row, l.rows, l.cols);
      report += buf;
    }
    if (FILE* f = _wfopen(kv[L"report"].c_str(), L"wb")) {
      fputs(report.c_str(), f);
      fclose(f);
    }
  }
  pg.reset();
  return ok ? 0 : 1;
}

}  // namespace

int run(const Request& r) {
  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);
  // The manifest asks for per-monitor v2; say it again for hosts without it.
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  ui::gdiplus_startup();
  int code = 0;
  {
    Session s;
    s.req = r;
    s.assets = r.dest.empty() ? default_assets_root() : r.dest;
    const std::wstring autoclose = env(L"AD_GUI_AUTOCLOSE");
    s.autoclose = !autoclose.empty() && autoclose != L"0";
    const std::wstring shot = env(L"AD_IMPORT_TEST_SCREENSHOT");
    code = shot.empty() ? run_flow(s) : run_screenshot(s, shot);
    if (s.icon_big) DestroyIcon(s.icon_big);
    if (s.icon_small) DestroyIcon(s.icon_small);
  }
  ui::gdiplus_shutdown();
  if (SUCCEEDED(co)) CoUninitialize();
  return code;
}

}  // namespace adw::import::gui
