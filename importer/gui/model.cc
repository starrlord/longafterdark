#include "model.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

#include "minijson.h"
#include "winutil.h"

namespace adw::import::gui {

namespace fs = std::filesystem;

std::string mb(uint64_t n) {
  char b[32];
  snprintf(b, sizeof(b), "%.1f MB", double(n) / (1024.0 * 1024.0));
  return b;
}

namespace {

std::wstring count(size_t n, const wchar_t* one, const wchar_t* many) {
  return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

std::wstring download_kind(const Download& d) {
  if (std::string_view(d.kind) == "zip") return L"Install files (ZIP)";
  // A release on several install floppies: an image of each.
  if (!d.more_images.empty()) return std::to_wstring(1 + d.more_images.size()) + L" floppy disk images";
  return L"CD image";
}

// A download of `p` already complete in `dir`: every file of one of its
// copies (by size: hashing 400 MB to draw a list would take seconds; the
// import checks the md5 anyway).
bool already_downloaded(const Package& p, const fs::path& dir) {
  if (dir.empty()) return false;
  std::error_code ec;
  auto have = [&](const wchar_t* name, uint64_t size) {
    return fs::is_regular_file(dir / name, ec) && fs::file_size(dir / name, ec) == size;
  };
  for (const Download& d : p.downloads)
    if (have(d.file_name, d.size) &&
        std::all_of(d.more_images.begin(), d.more_images.end(),
                    [&](const DownloadPart& q) { return have(q.file_name, q.size); }))
      return true;
  return false;
}

std::wstring installed_list(const std::vector<std::string>& titles) {
  std::wstring s;
  for (const std::string& t : titles) s += (s.empty() ? L"" : L", ") + to_wide(t);
  return s;
}

// The known image of `package` an import came from: the one with its md5,
// else the package's first (a folder of its files, a set of its disks).
const KnownImage* known_image_of(const std::string& package, const std::string& image_md5) {
  const Package* p = package.empty() ? nullptr : find_package(package);
  if (!p || p->images.empty()) return nullptr;
  for (const KnownImage& k : p->images)
    if (!image_md5.empty() && image_md5 == k.md5) return &k;
  return &p->images[0];
}

// A known image that is a ZIP of the install files (packages.h KnownImage::medium).
bool is_zip_image(const KnownImage* k) { return k && std::string_view(k->medium).rfind("ZIP", 0) == 0; }

}  // namespace

std::wstring verified_words(const std::string& verified, const std::string& package, const std::string& image_md5) {
  if (verified == "image") {
    // The Simpsons and Star Trek came on floppies, not a disc; Marvel Comics
    // Screen Posters (by either of two), Snoopy's Screen Savers, the Disney
    // Collection, ScreamSavers and the Looney Tunes (but from its CD) are
    // known by the ZIP of their install files.
    const KnownImage* k = known_image_of(package, image_md5);
    if (is_zip_image(k)) return L"verified against the known ZIP";
    const bool floppy = k && std::string_view(k->medium).find("floppy") != std::string_view::npos;
    return floppy ? L"verified against the original disks" : L"verified against the original disc";
  }
  if (verified == "files") return L"every file verified";
  if (verified == "partial") return L"partly verified";
  if (verified == "none") return L"not verified";
  return verified.empty() ? L"" : L"verified: " + to_wide(verified);
}

// ---- progress -----------------------------------------------------------------------

std::wstring phase_instruction(Progress::Phase p, const std::string& package, size_t step, size_t steps) {
  std::wstring what = package.empty() ? L"the files" : to_wide(package);
  std::wstring s;
  switch (p) {
    case Progress::Phase::download:
      s = package.empty() ? L"Downloading the disc image" : L"Downloading " + what + L" from the Internet Archive";
      break;
    case Progress::Phase::check_image:
      // A download names its release from the start; a local image is
      // identified only after this check.
      s = package.empty() ? L"Checking the disc image" : L"Checking " + what;
      break;
    case Progress::Phase::copy: s = L"Copying " + what; break;
    case Progress::Phase::verify: s = L"Verifying " + what; break;
    case Progress::Phase::cover: s = L"Getting the cover art"; break;
    case Progress::Phase::finalize: s = L"Finishing " + what; break;
  }
  if (steps > 1) s += L" (" + std::to_wstring(step + 1) + L" of " + std::to_wstring(steps) + L")";
  return s;
}

std::wstring amount_line(uint64_t done, uint64_t total, const std::string& speed) {
  std::wstring s = to_wide(mb(done));
  if (total) s += L" of " + to_wide(mb(total));
  if (!speed.empty()) s += L" (" + to_wide(speed) + L")";
  return s;
}

// ---- the Sources page ---------------------------------------------------------------------

std::wstring sources_intro(bool any_installed, std::span<const Package> registry) {
  const auto known = registry_or_builtin(registry);
  std::wstring releases;
  if (any_installed) {
    // A count in words, as the settings dialog says "(twelve releases are supported)".
    static const wchar_t* const kWords[] = {L"no",     L"one",     L"two",      L"three",    L"four",    L"five",
                                            L"six",    L"seven",   L"eight",    L"nine",     L"ten",     L"eleven",
                                            L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen", L"seventeen",
                                            L"eighteen", L"nineteen", L"twenty"};
    const size_t n = known.size();
    releases = (n < std::size(kWords) ? std::wstring(kWords[n]) : std::to_wstring(n)) + (n == 1 ? L" release" : L" releases");
  } else {
    for (size_t i = 0; i < known.size(); ++i)
      releases += std::wstring(i == 0 ? L"" : i + 1 == known.size() ? L" and " : L", ") + to_wide(known[i].title);
  }
  return L"Long After Dark runs the original Windows modules of " + releases + L". Choose where to copy them from.";
}

// ---- installed releases ---------------------------------------------------------------

std::map<std::string, int> catalog_module_counts(const fs::path& win_dir) {
  std::map<std::string, int> out;
  std::ifstream f(win_dir / L"catalog-win.json", std::ios::binary);
  if (!f) return out;
  std::stringstream ss;
  ss << f.rdbuf();
  auto j = parse_json(ss.str());
  if (!j || j->kind != JsonValue::Kind::object) return out;
  const JsonValue* packages = j->get("packages");
  if (!packages || packages->kind != JsonValue::Kind::array) return out;
  for (const JsonValue& p : packages->array) {
    if (p.kind != JsonValue::Kind::object) continue;
    const JsonValue* m = p.get("modules");
    std::string id = p.str("id");
    if (!id.empty() && m && m->kind == JsonValue::Kind::number) out[id] = m->as_int(0);
  }
  return out;
}

std::vector<InstalledRow> installed_rows(const fs::path& assets, std::span<const Package> registry) {
  std::vector<InstalledRow> rows;
  const fs::path root = assets.empty() ? default_assets_root() : assets;
  std::map<std::string, int> modules = catalog_module_counts(win_assets_dir(root));
  for (const PackageState& st : list_packages(root, registry)) {
    if (!st.installed || !st.package) continue;
    InstalledRow r;
    r.id = st.package->id;
    r.title = to_wide(st.package->title);
    auto m = modules.find(r.id);
    if (m != modules.end()) r.detail = count(size_t(std::max(0, m->second)), L"module", L"modules");
    if (!st.verified.empty())
      r.detail += (r.detail.empty() ? L"" : L" · ") + verified_words(st.verified, r.id, st.image_md5);
    r.cover = cover_info(r.id, root, registry);
    rows.push_back(std::move(r));
  }
  return rows;
}

std::vector<std::string> missing_cover_ids(const std::vector<InstalledRow>& rows) {
  std::vector<std::string> ids;
  for (const InstalledRow& r : rows)
    if (r.cover.origin == CoverOrigin::generated && r.cover.can_download) ids.push_back(r.id);
  return ids;
}

std::wstring missing_covers_note(size_t n) {
  if (n == 0) return L"";
  return n == 1 ? L"One release has no cover picture yet." : count(n, L"release", L"releases") + L" have no cover picture yet.";
}

// ---- the Internet Archive list ----------------------------------------------------------

std::vector<DownloadRow> download_rows(const fs::path& assets, const std::string& package, const fs::path& download_dir,
                                       std::span<const Package> registry) {
  std::vector<DownloadRow> rows;
  const fs::path root = assets.empty() ? default_assets_root() : assets;
  for (const PackageState& st : list_packages(root, registry)) {
    const Package& p = *st.package;
    if (p.downloads.empty() || (!package.empty() && package != p.id)) continue;
    const Download& d = p.downloads.front();
    DownloadRow r;
    r.id = p.id;
    r.title = to_wide(p.title);
    r.installed = st.installed;
    r.size = download_size(d);
    std::wstring state = st.installed ? L"Imported" + (st.verified.empty()
                                                           ? L""
                                                           : L" · " + verified_words(st.verified, p.id, st.image_md5))
                                      : L"Not imported yet";
    if (already_downloaded(p, download_dir)) state += L" · already downloaded";
    r.text = r.title + L"\n" + download_kind(d) + L" · " + to_wide(mb(r.size)) + L"\n" + state;
    r.cover = cover_info(r.id, root, registry);
    rows.push_back(std::move(r));
  }
  return rows;
}

std::optional<AllRow> all_missing_row(const std::vector<DownloadRow>& rows) {
  AllRow all;
  uint64_t bytes = 0;
  size_t n = 0;
  for (const DownloadRow& r : rows) {
    if (r.installed) continue;
    all.ids.push_back(r.id);
    bytes += r.size;
    n++;
  }
  if (n < 2) return std::nullopt;
  all.text = L"Every release not imported yet\n" + count(n, L"release", L"releases") + L" · " + to_wide(mb(bytes)) +
             L" in all";
  return all;
}

std::wstring download_card_text(const std::string& package, std::span<const Package> registry) {
  uint64_t lo = UINT64_MAX, hi = 0;
  size_t downloadable = 0;
  for (const Package& p : registry_or_builtin(registry)) {
    if (p.downloads.empty() || (!package.empty() && package != p.id)) continue;
    downloadable++;
    lo = std::min(lo, download_size(p.downloads.front()));
    hi = std::max(hi, download_size(p.downloads.front()));
  }
  if (!downloadable) return L"";
  std::wstring sizes = lo == hi ? L"About " + to_wide(mb(lo)) : L"From " + to_wide(mb(lo)) + L" to " + to_wide(mb(hi)) + L" per release";
  return L"Download from the Internet Archive…\n" + sizes + L", each checked against its published md5";
}

// ---- results ------------------------------------------------------------------------

std::wstring failure_heading(const ImportResult& r) {
  return r.status == Status::network         ? L"The download failed"
         : r.status == Status::verify_failed ? L"The files did not verify"
         : r.status == Status::source_invalid ? L"That source is not a disc Long After Dark knows, or it is damaged"
         : r.status == Status::cancelled      ? L"The import was cancelled"
                                              : L"The import failed";
}

namespace {

std::wstring mismatches(const ImportResult& r) {
  std::wstring s;
  for (const ImportedFile& f : r.files)
    if (f.known == ImportedFile::Known::mismatch) s += L"\n  differs: " + to_wide(f.path) + L" (md5 " + to_wide(f.md5) + L")";
  return s;
}

}  // namespace

ResultText single_result(const ImportResult& r) {
  ResultText t;
  if (r.status == Status::ok) {
    t.kind = Outcome::success;
    std::wstring title = to_wide(r.package_title);
    t.heading = L"Imported " + title + L": " + count(r.package_modules, L"module", L"modules");
    // "the known image of <title>": a title may start with "The", and a
    // floppy image is not a disc. A release known by the ZIP of its install
    // files (the Disney Collection's, ScreamSavers') was matched as that ZIP.
    const bool zip = r.verified == "image" && is_zip_image(known_image_of(r.package_id, r.iso_md5)) &&
                     !r.iso_md5.empty();
    std::wstring verified = zip                     ? L"The ZIP matched the known ZIP of " + title + L"."
                            : r.verified == "image" ? L"The image matched the known image of " + title + L"."
                            : r.verified == "files" ? L"Every file matched the release of " + title + L"."
                                                    : L"Some files could not be checked against the known release.";
    // No sentence period after the path: "…\FILES." reads as part of it.
    t.body = count(r.files.size(), L"file was", L"files were") + L" copied to\n" + r.files_dir.wstring() + L"\n\n" +
             verified + L"\n\nInstalled: " + installed_list(r.installed) + L". " +
             count(r.catalog_modules, L"module is", L"modules are") + L" ready to use.";
  } else {
    t.kind = Outcome::failure;
    t.heading = failure_heading(r);
    t.body = to_wide(r.message) + L"\n\nNothing was changed.";
  }
  t.details = t.heading + L"\n\n" + to_wide(r.message.empty() ? std::string() : r.message);
  if (!r.source.empty()) t.details += L"\nsource: " + to_wide(r.source);
  if (!r.url.empty()) t.details += L"\nurl: " + to_wide(r.url);
  if (!r.iso_md5.empty()) t.details += L"\nmd5: " + to_wide(r.iso_md5);
  t.details += L"\nstatus: " + to_wide(status_name(r.status)) + mismatches(r);
  return t;
}

ResultText several_result(const std::vector<std::string>& ids, const std::vector<ImportResult>& rs) {
  ResultText t;
  size_t ok = 0;
  std::wstring lines, details;
  const ImportResult* last_ok = nullptr;
  for (size_t i = 0; i < ids.size(); i++) {
    const Package* p = find_package(ids[i]);
    std::wstring title = to_wide(p ? p->title : ids[i]);
    if (i >= rs.size()) {
      lines += L"\n" + title + L": not started (cancelled)";
      continue;
    }
    const ImportResult& r = rs[i];
    if (r.status == Status::ok) {
      ok++;
      last_ok = &r;
      const std::wstring v = verified_words(r.verified, r.package_id.empty() ? ids[i] : r.package_id, r.iso_md5);
      lines += L"\n" + title + L": " + count(r.package_modules, L"module", L"modules") + (v.empty() ? L"" : L" (" + v + L")");
    } else if (r.status == Status::cancelled) {
      lines += L"\n" + title + L": cancelled";
    } else {
      lines += L"\n" + title + L": " + failure_heading(r) + L" — " + to_wide(r.message);
      details += L"\n" + title + L": " + to_wide(status_name(r.status)) + L": " + to_wide(r.message) + mismatches(r);
    }
  }
  t.kind = ok == ids.size() ? Outcome::success : ok ? Outcome::partial : Outcome::failure;
  t.heading = L"Imported " + std::to_wstring(ok) + L" of " + count(ids.size(), L"release", L"releases");
  t.body = lines.empty() ? L"" : lines.substr(1);
  if (last_ok) {
    t.body += L"\n\nInstalled: " + installed_list(last_ok->installed) + L". " +
              count(last_ok->catalog_modules, L"module is", L"modules are") + L" ready to use.";
  } else {
    t.body += L"\n\nNothing was changed.";
  }
  t.details = t.heading + L"\n\n" + t.body + details;
  return t;
}

// ---- covers -------------------------------------------------------------------------

std::wstring origin_line(const CoverInfo& c) {
  // Not "no picture": the files are there, but could not be read.
  if (!c.error.empty() && c.origin == CoverOrigin::generated) return L"The cover could not be read";
  switch (c.origin) {
    case CoverOrigin::user: return L"Your own picture";
    case CoverOrigin::generated: return L"No picture yet";
    case CoverOrigin::disc: {
      std::wstring label = c.label.empty() ? L"Art" : to_wide(c.label);
      return c.credit.empty() ? label + L" from your disc" : label + L" from " + to_wide(c.credit);
    }
    case CoverOrigin::download: {
      std::wstring label = c.label.empty() ? L"Picture" : to_wide(c.label);
      return c.credit.empty() ? label : label + L" · " + to_wide(c.credit);
    }
  }
  return L"";
}

std::wstring source_words(const std::string& label, const std::string& credit) {
  if (label.empty()) return credit.empty() ? L"" : to_wide(credit);
  return credit.empty() ? to_wide(label) : to_wide(label) + L" · " + to_wide(credit);
}

std::wstring original_note(const CoverInfo& c) {
  if (!c.has_user) return L"";
  if (c.original == CoverOrigin::generated) return L"No original picture yet: a generated cover";
  return L"Original: " + source_words(c.original_label, c.original_credit);
}

std::wstring download_note(const CoverInfo& c, bool downloads_allowed) {
  if (!c.can_download || !downloads_allowed) return L"";
  const std::wstring what = source_words(c.download_label, c.download_credit);
  const std::wstring from = what.empty() ? L"From the Internet" : L"Gets the " + what;
  // Under your own picture it replaces only the original that "Use the original cover" brings back.
  return c.has_user ? from + L"; your picture stays the cover" : from;
}

ResultText covers_result(const std::vector<CoverResult>& rs) {
  ResultText t;
  size_t got = 0, failed = 0, network = 0, same = 0;
  std::wstring lines, same_lines, details;
  for (const CoverResult& r : rs) {
    const Package* p = find_package(r.info.package);
    const std::wstring title = to_wide(p ? p->title : r.info.package);
    if (r.status == Status::ok) {
      std::wstring& to = r.changed ? lines : same_lines;
      (r.changed ? got : same)++;
      to += L"\n" + title + L": " + origin_line(r.info);
    } else {
      failed++;
      if (r.status == Status::network) network++;
      lines += L"\n" + title + L": " + (r.status == Status::network ? L"the download failed" : L"the cover could not be changed");
      details += L"\n" + title + L": " + to_wide(status_name(r.status)) + L": " + to_wide(r.message);
    }
  }
  t.kind = failed == 0 ? Outcome::success : got ? Outcome::partial : Outcome::failure;
  // Unchanged and still generated: nothing could be fetched (downloads off).
  const bool none_yet = std::any_of(rs.begin(), rs.end(), [](const CoverResult& r) {
    return r.status == Status::ok && !r.changed && r.info.origin == CoverOrigin::generated;
  });
  if (failed == 0) {
    t.heading = got == 0 ? std::wstring(none_yet ? L"No cover picture could be fetched"
                                                 : L"The covers are already the best available")
                : got == 1 && same == 0 ? L"Got the cover art"
                                        : L"Got " + count(got, L"cover", L"covers");
  } else if (got) {
    t.heading = L"Got " + std::to_wstring(got) + L" of " + count(got + failed, L"cover", L"covers");
  } else {
    t.heading = network ? L"The cover downloads failed" : L"The covers could not be changed";
  }
  if (rs.empty()) {
    t.body = L"No release is imported yet.";
  } else if (got == 0 && failed == 0) {
    t.body = same_lines.substr(1);   // what each shows
    // refresh_covers says so when it was the switch (COVERS.md §9.1: AD_COVER_DOWNLOAD=0).
    const bool off = std::any_of(rs.begin(), rs.end(), [](const CoverResult& r) {
      return r.status == Status::ok && r.message.rfind("downloads are off", 0) == 0;
    });
    if (none_yet && off) t.body += L"\n\nCover downloads are turned off on this computer.";
  } else {
    t.body = lines.substr(1);
    if (same) {
      t.body += same == 1 ? std::wstring(L"\n\nThe other cover was already the best available.")
                          : L"\n\nThe other " + std::to_wstring(same) + L" covers were already the best available.";
    }
  }
  if (network) t.body += L"\n\nThe covers you had are kept. Try again when this computer is online.";
  t.details = t.heading + L"\n\n" + t.body + details;
  return t;
}

// ---- the session ----------------------------------------------------------------------

void Tally::import_result(Status s) {
  if (s == Status::ok) changed = true;
  else if (!first_failure) first_failure = int(s);
}

int Tally::exit_code() const {
  if (changed) return 0;
  return first_failure ? first_failure : int(Status::cancelled);
}

}  // namespace adw::import::gui
