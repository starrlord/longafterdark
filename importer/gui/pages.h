// adimport's windows (COVERS.md §4.2): Sources, Downloads, Progress, Result
// and Cover. Each is a Page (page.h); gui.cc runs them in turn.
//
// Command ids (TDM_CLICK_BUTTON presses any of them):
//   Sources    101 a disc image, 102 a drive or folder, 103 the Internet Archive,
//              104 "Get the covers" (releases with no cover picture yet),
//              400+i "Change cover…" of registry package i, IDCANCEL Cancel/Close
//   Downloads  200+i the i-th release listed (registry order), 299 every release
//              not imported yet, IDCANCEL Back
//   Progress   IDCANCEL Cancel
//   Result     IDOK Done, 501 Copy details
//   Cover      601 Choose a picture…, 602 Use the original cover,
//              603 Download the original cover, IDOK Done
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "cancel.h"
#include "page.h"

struct IShellItemArray;  // shobjidl.h

namespace adw::import::gui {

inline constexpr int kIdImage = 101, kIdFolder = 102, kIdDownload = 103, kIdGetCovers = 104;
inline constexpr int kIdChangeCoverBase = 400;
inline constexpr int kIdDownloadBase = 200, kIdDownloadAll = 299;
inline constexpr int kIdCopyDetails = 501;
inline constexpr int kIdChoosePicture = 601, kIdUseOriginal = 602, kIdDownloadCover = 603;

// The file dialogs, or AD_IMPORT_TEST_PICK (paths separated by '|') for tests.
std::vector<std::filesystem::path> pick_source(HWND owner, bool folder);
std::optional<std::filesystem::path> pick_picture(HWND owner);
// The dialogs' own two halves, which the tests check without showing a
// window: the options a dialog gets (FOS_*, over `base`), and the paths its
// results name (items that are no file-system path are left out).
DWORD dialog_options(DWORD base, bool folder, bool multi);
std::vector<std::filesystem::path> dialog_paths(::IShellItemArray* items);

// ---- Sources --------------------------------------------------------------------------

class SourcesPage : public Page {
 public:
  explicit SourcesPage(Session& s);
  ~SourcesPage() override;
  enum class Choice { cancel, image, folder, downloads, change_cover, get_covers };
  Choice choice = Choice::cancel;
  std::vector<std::filesystem::path> paths;   // image(s) or the folder
  std::string cover_id;                       // change_cover
  std::vector<std::string> cover_ids;         // get_covers: the releases with no cover picture yet
  // A caution under the cards ("That is not a disc Long After Dark knows…").
  void show_caution(const std::wstring& text);
  // The installed list as last laid out, in px (the screenshot hook's
  // report): the height that shows, the whole list's, and a row's; all 0 when
  // nothing is installed.
  struct ListHeights {
    int shown = 0, whole = 0, row = 0;
  };
  ListHeights list_heights() const { return list_heights_; }

 protected:
  std::wstring header_tagline() const override { return L"From your discs or the Internet Archive"; }
  void build() override;
  int layout(int w, int max_h) override;
  void paint(HDC dc) override;
  void command(int id, int code, HWND ctl) override;
  int default_id() const override { return 0; }
  int initial_focus() const override { return kIdImage; }

 private:
  std::vector<InstalledRow> rows_;
  std::vector<ui::Image> tiles_;
  std::unique_ptr<ScrollPanel> body_, list_;
  HWND intro_ = nullptr, installed_label_ = nullptr, installed_note_ = nullptr, from_label_ = nullptr;
  HWND card_image_ = nullptr, card_folder_ = nullptr, card_download_ = nullptr;
  HWND caution_ = nullptr, footer_text_ = nullptr, cancel_ = nullptr;
  HWND covers_note_ = nullptr, covers_link_ = nullptr;   // "N releases have no cover picture yet." Get the covers
  std::vector<std::string> missing_;
  std::vector<HWND> row_title_, row_detail_, row_link_;
  RECT list_card_{};
  int row_h_ = 0;
  ListHeights list_heights_;
};

// ---- Downloads ------------------------------------------------------------------------

class DownloadsPage : public Page {
 public:
  explicit DownloadsPage(Session& s);
  ~DownloadsPage() override;
  std::vector<std::string> ids;   // what to fetch; empty = Back

 protected:
  std::wstring header_tagline() const override { return L"Download from the Internet Archive"; }
  void build() override;
  int layout(int w, int max_h) override;
  void command(int id, int code, HWND ctl) override;
  int default_id() const override { return 0; }
  int initial_focus() const override { return kIdDownloadBase; }

 private:
  std::vector<DownloadRow> rows_;
  std::optional<AllRow> all_;
  std::vector<ui::Image> tiles_;
  std::vector<std::unique_ptr<ui::CardCover>> covers_;
  std::unique_ptr<ScrollPanel> list_;
  HWND intro_ = nullptr, note_ = nullptr, back_ = nullptr;
  std::vector<HWND> cards_;
  HWND all_card_ = nullptr;
};

// ---- Progress -------------------------------------------------------------------------

struct Job {
  Source source;
  std::vector<std::string> all;   // several downloads, one after another (else: import `source`)
  // No import: refresh_covers over `covers` (empty = every installed release); "Get the covers".
  bool covers_job = false;
  std::vector<std::string> covers;
  bool force = false;
};

class ProgressPage : public Page {
 public:
  // `start` false: no worker (the screenshot hook shows synthetic progress).
  ProgressPage(Session& s, Job job, bool start = true);
  ~ProgressPage() override;
  const std::vector<ImportResult>& results() const { return results_; }
  const std::vector<CoverResult>& cover_results() const { return cover_results_; }
  const Job& job() const { return job_; }
  // The screenshot hook: what the window shows, as if the worker had reported it.
  void show_progress(const Progress& p, size_t step, bool cancelling = false);

 protected:
  void build() override;
  int layout(int w, int max_h) override;
  void command(int id, int code, HWND ctl) override;
  void cancel() override { command(IDCANCEL, BN_CLICKED, nullptr); }
  void timer(UINT_PTR id) override;
  int default_id() const override { return 0; }
  int initial_focus() const override { return IDCANCEL; }
  bool minimizable() const override { return true; }

 private:
  void refresh();
  void set_marquee(bool on);

  Job job_;
  bool start_;
  HWND phase_ = nullptr, bar_ = nullptr, amount_ = nullptr, item_ = nullptr, cancel_btn_ = nullptr;
  // Cancel (the button, Esc, the close box) and the destructor cancel through
  // it: a download blocked on the network stops at once (cancel.h).
  CancelToken token_;
  std::thread worker_;
  std::mutex m_;
  Progress progress_;
  bool have_progress_ = false;
  size_t step_ = 0;
  std::atomic<bool> cancel_{false}, finished_{false};
  std::vector<ImportResult> results_;
  std::vector<CoverResult> cover_results_;
  // What is shown.
  bool shown_any_ = false, marquee_ = false;
  Progress::Phase shown_phase_ = Progress::Phase::finalize;
  std::string shown_package_;
  size_t shown_step_ = 0;
  ULONGLONG speed_tick_ = 0;
  uint64_t speed_bytes_ = 0;
  std::string speed_;
};

// ---- Result ---------------------------------------------------------------------------

class ResultPage : public Page {
 public:
  ResultPage(Session& s, ResultText text);
  ~ResultPage() override;

 protected:
  void build() override;
  int layout(int w, int max_h) override;
  void paint(HDC dc) override;
  void command(int id, int code, HWND ctl) override;
  void cancel() override { finish(IDOK); }
  int default_id() const override { return IDOK; }
  int initial_focus() const override { return IDOK; }

 private:
  ResultText rt_;
  HWND heading_ = nullptr, body_ = nullptr, copy_ = nullptr, done_ = nullptr;
  RECT glyph_{};
};

// ---- Cover (COVERS.md §2.11) ----------------------------------------------------------

class CoverPage : public Page {
 public:
  CoverPage(Session& s, std::string id);
  ~CoverPage() override;
  // The screenshot hook: a message as an action would leave it, or the running state.
  void show_status(const std::wstring& text, Ink ink);
  void show_running(const Progress& p);

 protected:
  std::wstring header_name() const override { return L"Change cover"; }
  std::wstring header_tagline() const override { return title_; }
  void build() override;
  int layout(int w, int max_h) override;
  void paint(HDC dc) override;
  void command(int id, int code, HWND ctl) override;
  void cancel() override;
  void timer(UINT_PTR id) override;
  int default_id() const override { return IDOK; }
  int initial_focus() const override { return kIdChoosePicture; }

 private:
  enum class Action { set, clear, refresh };
  void start(Action a, std::filesystem::path picture = {});
  void finish_action();
  void load_info(const CoverInfo* info = nullptr);
  void update_buttons();

  std::string id_;
  std::wstring title_;
  CoverInfo info_;
  ui::Image tile_;
  HWND origin_ = nullptr, choose_ = nullptr, original_ = nullptr, download_ = nullptr;
  HWND original_note_ = nullptr, download_note_ = nullptr;   // what each of the two would bring
  HWND status_ = nullptr, bar_ = nullptr, note_ = nullptr, done_ = nullptr;
  RECT preview_{};
  bool running_ = false;
  CancelToken token_;   // the running action's (reset for each one)
  std::thread worker_;
  std::mutex m_;
  Progress progress_;
  bool have_progress_ = false;
  std::atomic<bool> cancel_{false}, action_done_{false};
  std::vector<CoverResult> action_results_;
};

}  // namespace adw::import::gui
