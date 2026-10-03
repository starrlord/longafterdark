// adimport's windows (COVERS.md §4.2): Sources, Downloads, Progress, Result
// and Cover. Each is a Page (page.h); gui.cc runs them in turn.
//
// Command ids (TDM_CLICK_BUTTON presses any of them):
//   Sources    101 a disc image, 102 a drive or folder, 103 the Internet Archive,
//              104 "Get the covers" (releases with no cover picture yet),
//              300+i the cover of registry package i (opens its menu), and from
//              that menu 400+i "Change cover…" and 450+i "Remove…", IDCANCEL Cancel/Close
//   Downloads  200+i the i-th release listed (registry order), 299 every release
//              not imported yet, IDCANCEL Back
//   Progress   IDCANCEL Cancel
//   Result     IDOK Done, 501 Copy details
//   Cover      601 Choose a picture…, 602 Use the original cover,
//              603 Download the original cover, IDOK Done
//   Remove     701 Remove, IDCANCEL Cancel (Close once it can't remove)
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
inline constexpr int kIdTileBase = 300, kIdChangeCoverBase = 400, kIdRemoveBase = 450;
inline constexpr int kIdDownloadBase = 200, kIdDownloadAll = 299;
inline constexpr int kIdCopyDetails = 501;
inline constexpr int kIdChoosePicture = 601, kIdUseOriginal = 602, kIdDownloadCover = 603;
inline constexpr int kIdRemove = 701;

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
  enum class Choice { cancel, image, folder, downloads, change_cover, get_covers, remove };
  Choice choice = Choice::cancel;
  std::vector<std::filesystem::path> paths;   // image(s) or the folder
  std::string cover_id;                       // change_cover, remove: the release
  std::vector<std::string> cover_ids;         // get_covers: the releases with no cover picture yet
  // A caution under the cards ("That is not a disc Long After Dark knows…").
  void show_caution(const std::wstring& text);
  // The installed covers as last laid out, in px (the screenshot hook's
  // report): the height that shows, the whole grid's, a row's, and the
  // grid's rows and columns; all 0 when nothing is installed.
  struct ListHeights {
    int shown = 0, whole = 0, row = 0, rows = 0, cols = 0;
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
  bool draw_button(NMCUSTOMDRAW* cd, LRESULT* result) override;
  bool context_menu(HWND ctl, POINT pt) override;
  bool menu_command(int id) const override;
  void theme_changed() override;

 private:
  // The installed releases are covers in a grid (COVERS.md §4.2): a cover is
  // a button that opens its menu ("Change cover…", "Remove…"); one of them is
  // a tab stop, the arrow keys move between them.
  static LRESULT CALLBACK tile_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
  int tile_index(HWND h) const;
  void show_menu(int i, POINT pt);
  void focus_tile(int i);

  std::vector<InstalledRow> rows_;
  std::vector<ui::Image> tiles_;
  std::unique_ptr<ScrollPanel> body_, list_;
  HWND intro_ = nullptr, notice_ = nullptr, installed_label_ = nullptr, installed_note_ = nullptr, from_label_ = nullptr;
  HWND card_image_ = nullptr, card_folder_ = nullptr, card_download_ = nullptr;
  HWND caution_ = nullptr, footer_text_ = nullptr, cancel_ = nullptr;
  HWND covers_note_ = nullptr, covers_link_ = nullptr;   // "N releases have no cover picture yet." Get the covers
  HWND tip_ = nullptr;                                     // the covers' tooltips
  std::vector<std::string> missing_;
  std::vector<HWND> tile_btn_;
  RECT list_card_{};
  int row_h_ = 0, cols_ = 1;
  ListHeights list_heights_;
};

// ---- Downloads ------------------------------------------------------------------------

class DownloadsPage : public Page {
 public:
  explicit DownloadsPage(Session& s);
  ~DownloadsPage() override;
  std::vector<std::string> ids;   // what to fetch; empty = Back
  // The cards the list shows at the most (whole ones; it scrolls for the rest).
  static constexpr int kMostCards = 9;
  // The list as last laid out, in px (the screenshot hook's report): the
  // height that shows, the whole list's, and where each card ends in it (a
  // list that tall shows that card and the ones above it whole).
  struct ListHeights {
    int shown = 0, whole = 0;
    std::vector<int> ends;
  };
  const ListHeights& list_heights() const { return list_heights_; }

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
  ListHeights list_heights_;
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

// ---- Remove ---------------------------------------------------------------------------

// "Remove …" (--gui --remove <id>, or Remove… on a Sources cover): the
// release's cover, the question, what it holds and what removing it does,
// then Remove and Cancel. Remove runs remove_package on a worker (the window
// keeps answering while a locked folder's rename is retried); a failure
// shows in the window and changes nothing, and Remove can try again. A
// release that isn't installed says so, with only Close.
class RemovePage : public Page {
 public:
  RemovePage(Session& s, std::string id);
  ~RemovePage() override;
  bool removed() const { return removed_; }
  const std::wstring& title() const { return title_; }
  // The screenshot hook: a failure as remove_package would leave it, or the running state.
  void show_failure(const std::string& message);
  void show_running();

 protected:
  std::wstring header_name() const override { return L"Remove a release"; }
  void build() override;
  int layout(int w, int max_h) override;
  void paint(HDC dc) override;
  void command(int id, int code, HWND ctl) override;
  void cancel() override;
  void timer(UINT_PTR id) override;
  int default_id() const override { return installed_ ? kIdRemove : IDCANCEL; }
  int initial_focus() const override { return installed_ ? kIdRemove : IDCANCEL; }

 private:
  void start();
  void finish_remove();
  void set_status(const std::wstring& text, Ink ink);

  std::string id_;
  std::wstring title_;
  InstalledRow row_;
  bool installed_ = false;
  ui::Image tile_;
  HWND question_ = nullptr, detail_ = nullptr, text_ = nullptr, bar_ = nullptr, status_ = nullptr;
  HWND remove_ = nullptr, cancel_btn_ = nullptr;
  RECT art_{};
  bool running_ = false, removed_ = false;
  std::thread worker_;
  std::mutex m_;
  std::atomic<bool> worker_done_{false};
  RemoveResult result_;
};

}  // namespace adw::import::gui
