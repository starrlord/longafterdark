// The machine under Windows that Classic binaries reach directly (ABI.md
// §3.6): INT 21h (DOS — directly and through KERNEL.DOS3Call), INT 1Ah (BIOS
// clock), INT 2Fh (multiplex: the Windows/MSCDEX install checks), INT 25h/26h
// (absolute disk I/O — refused), INT 10h (the VGA palette functions), INT 16h
// (keyboard — empty), INT 31h (the DPMI descriptor services), and the DOS
// file handles KERNEL's _lopen/_lread/OpenFile share.
//
// Files go through the guest file system (win32::Vfs, INTERACTION.md §7): a
// guest path outside every mount does not exist. The lane mounts C:\WINDOWS
// and C:\AFTERDRK as copy-on-write overlays over read-only lower layers
// (virtual seed files; the module dir), whose upper layer is the per-user
// state (ADSTATE) or, by default, memory — so a module that saves its state
// sees it again, the host's files are never modified, and a headless run
// starts from the same disk every time. DosFiles is the DOS handle table over
// Vfs::open (VfsFile); KERNEL's _lopen/_lcreat/OpenFile and INT 21h share it.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "win16/runtime16.hh"

namespace adw::win32 {
class IniStore;
class VfsFile;
}  // namespace adw::win32

namespace adw::win16 {

// DOS error codes the file layer returns.
namespace doserr {
constexpr uint16_t kInvalidFunction = 1, kFileNotFound = 2, kPathNotFound = 3, kTooManyFiles = 4,
                   kAccessDenied = 5, kInvalidHandle = 6, kInvalidDrive = 15, kNoMoreFiles = 18, kFileExists = 80;
}

// The guest's current drive and directories are DOS's (win32::Vfs keeps a
// current drive and a current directory per drive; relative paths, "X:name"
// included, resolve against them): INT 21h AH=0Eh selects a drive, 19h
// reports it, 3Bh sets its drive's directory, 47h reports any drive's, and
// USER's DlgDirList moves to the drive and directory it lists. A current
// directory is at most kMaxCurDir characters with its drive ("C:\" and 63
// more): a DOS CDS held 67 bytes with the NUL, and AH=47h's 64-byte buffer the
// part after "C:\" with its NUL. A chdir that would go deeper fails with
// error 3, as DOS's did, so a folder dialog never enters such a folder and
// AH=47h never has to cut a path short. The drive letters run to H: (AH=0Eh
// reports 8): C:, and the host's drives' H: when mounted.
constexpr size_t kMaxCurDir = 66;
constexpr uint8_t kLastDrive = 8;
// Makes `path` its drive's current directory (DOS chdir), and with
// select_drive (DlgDirList) that drive the current one too. 0, or -error:
// kPathNotFound when it is not an existing directory, holds a wildcard, or is
// longer than kMaxCurDir; nothing changes then.
int dos_chdir(Runtime16& rt, const std::string& path, bool select_drive);

class DosFiles : public RuntimeState16 {
 public:
  explicit DosFiles(Runtime16& rt) : rt_(rt) {}
  ~DosFiles() override;

  // mode: 0 read, 1 write, 2 read/write (low bits of the DOS open mode).
  // create: make/truncate it; exclusive (with create): fail when it exists
  // (INT 21h 5Bh). Returns a handle (>= 5) or -error (a DOS error code).
  int open(const std::string& guest_path, int mode, bool create, bool exclusive = false);
  int close(uint16_t h);                                     // 0 or -error
  int32_t read(uint16_t h, uint32_t buf_fp, uint32_t n);     // bytes or -error (huge buffers allowed)
  int32_t write(uint16_t h, uint32_t buf_fp, uint32_t n);    // bytes or -error; n = 0 truncates
  int64_t seek(uint16_t h, int32_t off, int whence);         // new position or -error
  int dup(uint16_t h);                                       // shares the file pointer, as DOS does
  bool is_device(uint16_t h) const { return h < 5; }
  bool valid(uint16_t h) const { return h < 5 || files_.count(h); }
  uint32_t size_of(uint16_t h);
  // A file (not a directory) at this guest path.
  bool exists(const std::string& guest_path);
  bool is_dir(const std::string& guest_path);
  // 0 or -error.
  int remove(const std::string& guest_path);
  int rename(const std::string& from, const std::string& to);
  int make_dir(const std::string& guest_path);
  int remove_dir(const std::string& guest_path);

  // The directory listing FindFirst/FindNext walk (the merged Vfs listing).
  struct Found {
    std::string name;  // 8.3 upper case
    uint32_t size = 0;
    uint8_t attr = 0;
  };
  std::vector<Found> list(const std::string& guest_pattern, uint8_t attr_mask);

  // A Win32 error from the Vfs as a DOS error code.
  static uint16_t dos_error(uint32_t win32_error);

 private:
  struct File {
    std::string guest;
    std::shared_ptr<win32::VfsFile> f;  // dup'd handles share it (and its position)
  };
  int add(File f);

  Runtime16& rt_;
  std::map<uint16_t, File> files_;
  std::string line_;  // text written to stdout/stderr, until its newline
  friend void register_dos(Runtime16& rt);
  friend void dos_write_console(Runtime16& rt, const std::string& text);
};

// The profile store (Get/WritePrivateProfileString…, INTERACTION.md §7.3):
// seeds ⊕ the file, writes to the upper layer only.
win32::IniStore& profiles16(Runtime16& rt);

// Installs the INT handlers above on `rt` and seeds the guest disk: C:\WINDOWS
// and C:\WINDOWS\TEMP as in-memory overlays (the lane mounts its own over
// them), the INI files After Dark's installer left there as empty virtual
// files, their settings (and WIN.INI's [Berkeley Systems]) as profile seeds.
void register_dos(Runtime16& rt);

// The task's PSP (the system segment's, kSysPsp): its selector, made on first
// use (INT 21h AH=51h/62h answer it; a task's start and InitTask hand it in
// ES, modules16.hh "Tasks"), with the environment's selector at 2Ch.
uint16_t dos16_psp(Runtime16& rt);
// The PSP's command tail, DOS style: its length at 80h, the text from 81h
// (" /s": a blank first, as Windows' loader left it), CR after it; at most
// 126 characters.
void dos16_set_command_tail(Runtime16& rt, std::string_view tail);
// (Re)seeds C:\WINDOWS\MODULES.INI's per-install settings for what the
// install directory (C:\AFTERDRK) holds now; register_dos seeds it before
// anything is mounted, the lane again once the module's folder is (PACKAGES.md
// §7.3). Profile seeds, never written out:
//   [The Artist] Image = C:\AFTERDRK\BITMAPS\ADLOGO.BMP
//   [Ray] RaySceneFile = the first of TRACES\ROTCUBE.TRC, DIAMOND.TRC,
//         ROTPYRA.TRC that exists (ROTCUBE when none does): Deluxe ships
//         ROTCUBE, AD 3.2 the other two
//   [Slide Show] CatalogName = BITMAPS
//   [Logo Section] LogoFile = C:\AFTERDRK\BITMAPS\ADLOGO.BMP (AD 3.2's LOGO
//         refuses to start without it)
void seed_modules_ini(Runtime16& rt);
// The profile seeds an Intermission module (the ne16 lane's IMX protocol:
// Star Wars Screen Entertainment) runs over, read as seed ⊕ file and never
// written out (host_integration.md §3.4):
//   SYSTEM.INI [boot] display.drv = pnpdrvr.drv — Windows 95's display
//        driver name, which SWSE's GETBLITTECHNOLOGY compares with SWSE.INI's
//        (1:4E95..1:4EB6; empty sends it to its WinG test);
//   with swse_gdi (the module folder holds SWSE.DLL): SWSE.INI [technology]
//        display.drv = pnpdrvr.drv, WinGFound = 1, DibBlit = GDI — what
//        SWSESET's "Use GDI Graphics" left after a probe: SWSE draws with GDI
//        and the DIB driver at once, never loads WING.DLL, never waits
//        (1:4EBC..1:4F3B);
//   ANTSW.INI [Intermission] Volume = volume, 0..100 — Intermission's own
//        volume, which SWSE turns into waveOutSetVolume (×595) and where 0 is
//        "Off": no effects and no music (1:7073..1:70D2) — and Saver Path =
//        saver_path (default: the guest directory), where INTRMLIB looks for
//        savers (1:2243..1:225C; its default is "c:\saver").
// The profile seeds a Windows 3.1 screen saver (the ne16 lane's scr
// protocol) runs over, read as seed ⊕ file and never written out — what
// its installer left (Johnny Castaway's INSTALL.INS, InstallSHIELD 1.02):
//   WIN.INI [Windows] ScreenSaveActive = 1, ScreenSaveTimeOut = 120;
//   SYSTEM.INI [boot] SCRNSAVE.EXE = program, the saver's guest path;
//   with source_dir (SCRANTIC.SCR's install dir): SCRANTIC.INI
//        [ScreenSaver.ScreenAntics] SourceDir = source_dir, where the
//        program finds RESOURCE.MAP and RESOURCE.001 (its default is the
//        current directory).
// What the program writes there (SCRANTIC.INI's story: NumDays,
// CurrentYear/Month/Day, StartTime, Introduction; its Setup... settings)
// lands in C:\WINDOWS's upper layer, ADSTATE's when set.
struct ScrnsaveSeeds {
  std::string program;
  std::string source_dir;
};
void seed_scrnsave(Runtime16& rt, const ScrnsaveSeeds& seeds);
struct IntermissionSeeds {
  int volume = 0;
  bool swse_gdi = false;
  std::string saver_path;
};
void seed_intermission(Runtime16& rt, const IntermissionSeeds& seeds);
// The profile seeds an After Dark 2.0 module (the ne16 lane's AD3 protocol,
// when the module folder holds AD_MOD.DLL: ne16/package.hh after_dark2) runs
// over, read as seed ⊕ file and never written out — what Star Trek: The
// Screen Saver's installer left in C:\WINDOWS\AD_PREFS.INI, with Windows'
// own sound for the PC speaker's (PACKAGES.md §7.3):
//   [After Dark] Path = the guest directory and a backslash (C:\AFTERDRK\),
//        where AD_MOD.DLL finds its ST_RES\ art and sound (without it every
//        module but Sounder stops with "File not found."), AD_SND its sound
//        drivers (*.DRV) and Sounder its SOUNDS\*.WAV;
//   [Sound] SoundDriver = AD_MME.DRV, AD_SND 1.0's plug-in driver for
//        "Multimedia Windows Sound": MMSYSTEM's sndPlaySound and waveOut
//        volume. The disk's AD_PREFS.INI named the PC speaker's AD_MPT.DRV,
//        whose SPALETTE.DLL busy-waits on the timer chip, which the runtime
//        has not (the first sound would hang the module).
// The modules' own writes (AD_SND's [Sound] Mute, Communications' [Communications]
// MessageText, Sounder's [Sounder] SoundPath) land in the upper layer.
void seed_after_dark2(Runtime16& rt);
// The profile seeds an After Dark 3.x package's modules run over (the ne16
// lane's AD3 protocol, when the engine dir holds ADW30.EXE: ne16/package.hh
// after_dark3_host), read as seed ⊕ file and never written out — what
// ADW30.EXE, the After Dark 3.x host the lane stands in for, wrote into
// C:\WINDOWS\AD_PREFS.INI at every start (its routine 3:0166, called from
// start-up at 1:0100), with its own directory: the guest directory, without
// a trailing backslash (ADW30 cut its module path at the last '\'):
//   [After Dark] Path = C:\AFTERDRK, where ADXPL100 (The Disney Collection's
//        module library, one of the After Dark 2.0 generation) finds
//        DIS_SND.DLL and MUSIC\ (without it every Disney module stops with
//        "File not found.");
//   [Sound] SoundDriver = AD_MME.DRV.
// ADW30's third key, WIN.INI [Berkeley Systems] After Dark = its directory,
// register_dos seeds for every module.
void seed_after_dark3(Runtime16& rt);
// A Windows 3.1 Program Manager's files — C:\WINDOWS\PROGMAN.INI [Groups]
// naming five .GRP files in C:\WINDOWS (Main, Accessories, Games, StartUp,
// After Dark) — which the desktop-icon gatherers of ADXPL40 and ADXPL310 read
// (PACKAGES.md §7.3). Virtual lower files of the C:\WINDOWS overlay.
// Idempotent; user16's synthetic desktop seeds them when it first comes into
// being (EnumWindows).
void seed_program_manager(Runtime16& rt);
// A Win 3.1 group file (GROUPDEF, "PMCC") holding just a name: pName at 0x16,
// no items, checksum making the words of the file sum to 0.
std::string progman_group_file(const std::string& name);
// INT 21h with the current registers (KERNEL.DOS3Call uses it too).
void dos_int21(Runtime16& rt);
// Text a guest wrote to its console handles, to the log (by line).
void dos_write_console(Runtime16& rt, const std::string& text);

}  // namespace adw::win16
