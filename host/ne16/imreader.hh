// The Classic lane's host side of the Intermission module protocol
// (lane.hh "Intermission (IMX)"): a reader drives an Intermission module the
// way Delrina's INTERMIS.EXE had one drive it — one entry point,
// SAVERMAIN(info, msg), and the saver's IMINFO record — so the IMX protocol
// (imx_protocol.cc) is the same whichever reader runs. The bridge.hh analog.
//
//   ImqReader    — Intermission's own IMX reader, IMIMXPLY.IMQ ("IMX Player"),
//                  as real emulated code: loaded by its guest path with
//                  LoadLibrary, its SAVERMAIN found with GetProcAddress
//                  ("saverMain": the name INTRMLIB asks for, 1:2044), as
//                  INTRMLIB's LOADSAVER did (1:1fe1..1:208a). The same for
//                  the ASA reader, IMASAPLY.IMQ ("ASA Player"), and for an
//                  IMQ module, its own reader (package.hh "Form").
//   NativeReader — IMIMXPLY's SAVERMAIN (2:002a..2:0550) in C++, message by
//                  message: the same Win16 calls (LoadLibrary, the six
//                  GetProcAddress, GlobalAlloc/Lock/Handle/Unlock/Free,
//                  lstrlen, lstrcpy, DialogBox, CreateDialog, FreeLibrary)
//                  through the runtime's thunks, so the census, api16 traces,
//                  virtual time and the scanout hook see them as they see the
//                  real reader's, and the same far calls into the module's
//                  SAVERINIT/SAVERDRAW/PALETTE. The oracle of the real reader
//                  (ADNE16READER=native), and what runs when no IMIMXPLY.IMQ
//                  was installed.
//
// SAVERMAIN, PASCAL far: DWORD SaverMain(IMINFO FAR* info, WORD msg) — info
// pushed first, msg last ([bp+6] msg, [bp+8] info; retf 6); the result in
// DX:AX: 1 for every handled message, 0 for a load (10) or configure (8) that
// failed, the dialog's HWND for 9; any message above 11 answers 1.
//
//   msg  IMIMXPLY
//     0  saverdraw(+4, +6, hLib, +8, 0) — one frame
//     1  [saverdraw(…, 3) when +1 & 0x40 (preview)]; saverdraw(…, 1) — start
//     2  saverdraw(…, 2); [saverdraw(…, 4) when +1 & 0x40] — stop
//     5  saverdraw(…, 2); saverdraw(…, 1) — restart (a later WM_PAINT)
//     6  palette((BYTE)+0x54) when the module exports PALETTE
//     7  query. With a path: +0 bit 0x04 = SAVERDLGPROC2 exported; +1 =
//        (+1 & 0xF3) | 0x10; w = palette(0) when exported (0x100 → 0xFE),
//        +0x54 = w; name = saverinit(&w), cut in place at 40 characters,
//        copied to +0x14; w less 200 when ≥ 200, less 100 when ≥ 100 → +0x53.
//        Without a path: the reader describes itself ("IMX" at +0x5B/+0x14)
//     8  DialogBox(hLib, "DIALOGBOX", +4, saverdlgproc); 0 without one
//     9  w = 999; saverinit(&w); CreateDialog(hLib, "DIALOGBOX", +4, saverdlgproc2)
//    10  load: no path → 1; a file named IMXX_* → 0; the block
//        GlobalLock(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 0x16)) → +0x55;
//        block+0 = LoadLibrary(path) (0 → 0); a module exporting setcurrsaver
//        → 0; saverinit and saverdraw required (0 without); the dialog procs
//        and palette optional
//    11  free: FreeLibrary(block+0) when set; GlobalUnlock/GlobalFree of the
//        block's handle (GlobalHandle of its selector)
//
// The IMINFO record and the reader's block are guest memory (far pointers).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace adw::win16 {
class Runtime16;
}

namespace adw::ne16 {

// The IMINFO record (INTRMLIB's per-saver record: intermission_protocol.md §3).
namespace iminfo {
constexpr uint16_t kFlags = 0x00;        // DWORD flags (below)
constexpr uint16_t kHwnd = 0x04;         // the saver window (the dialog owner for 8 and 9)
constexpr uint16_t kHdc = 0x06;          // this call's DC
constexpr uint16_t kPalette = 0x08;      // this call's engine palette (0: none)
constexpr uint16_t kReader = 0x0A;       // the reader's instance handle
constexpr uint16_t kPaint = 0x0C;        // RECT: the paint rectangle of a REPAINT (5)
constexpr uint16_t kName = 0x14;         // char[41]: the display name (QUERY)
constexpr uint16_t kNameSize = 41;
constexpr uint16_t kFile = 0x44;         // char[14]: the file name, 8.3 ("VADER.IMX")
constexpr uint16_t kFileSize = 14;
constexpr uint16_t kShown = 0x52;        // BYTE: bit 0, shown in this random cycle
constexpr uint16_t kPaletteType = 0x53;  // BYTE: the engine palette the module wants (0 none, 1 CLUT, 2 HSV, 3 PRIM)
constexpr uint16_t kPaletteState = 0x54; // BYTE: palette(0)'s answer (message 6's argument)
constexpr uint16_t kBlock = 0x55;        // far pointer: the reader's own block
constexpr uint16_t kReaderIndex = 0x59;  // WORD: the index of the reader that handles the file (-1: a reader)
constexpr uint16_t kType = 0x5B;         // char[4]: a reader's file type ("IMX")
constexpr uint16_t kPath = 0x63;         // far pointer: the module file's full path (0: the reader alone)
constexpr uint16_t kSize = 0x67;
// Flags: byte 0 bit 0x04 has an inline panel (SAVERDLGPROC2); bits 0x70 the
// engine's DC mode (0: GetDC + SaveDC/RestoreDC + palette around each call;
// 0x20: the reader draws on its own). Byte 1 (the word's 0x0100 bits):
// 0x02 enabled, 0x10 runnable saver, 0x08 a reader, 0x20 takes input, 0x40
// preview with the inline panel (saverdraw codes 3/4).
constexpr uint32_t kModuleFlags = 0x0000120C;  // INTRMLIB's defaults for an enabled module (1:2274..1:22b5)
constexpr uint32_t kSaver = 0x00001000;        // a runnable saver: what a QUERY without a path says of an IMQ module
constexpr uint32_t kIsReader = 0x00000800;     // a reader: never one of INTERMIS's savers, whatever 0x1000 says
constexpr uint32_t kTakesInput = 0x00002000;   // the saver takes input (lane.hh "Intermission (IMX)")
constexpr uint32_t kPreview = 0x00004000;
constexpr uint16_t kOwnReader = 0xFFFF;        // +0x59 of a reader's record, and of an IMQ module's: it is its own
}  // namespace iminfo

// IMIMXPLY's block (msg 10): far pointers to the module's exports.
namespace imblock {
constexpr uint16_t kLib = 0x00;       // WORD: the module's instance
constexpr uint16_t kDraw = 0x02;      // saverdraw(HWND, HDC, HINSTANCE, HPALETTE, WORD code), retf 10
constexpr uint16_t kInit = 0x06;      // LPSTR saverinit(WORD FAR*), retf 4
constexpr uint16_t kPaletteFn = 0x0A; // WORD palette(WORD), retf 2
constexpr uint16_t kDlgProc = 0x0E;   // SAVERDLGPROC: DIALOGBOX's (Configure) dialog procedure
constexpr uint16_t kDlgProc2 = 0x12;  // SAVERDLGPROC2: the same template as the preview's inline panel
constexpr uint16_t kSize = 0x16;
}  // namespace imblock

// SAVERMAIN messages (the names are ours; intermission_protocol.md §5).
namespace immsg {
constexpr uint16_t kDraw = 0, kStart = 1, kStop = 2, kRepaint = 5, kPalette = 6, kQuery = 7, kConfigure = 8,
                   kPanel = 9, kLoad = 10, kFree = 11;
}

class ImReader {
 public:
  virtual ~ImReader() = default;
  virtual const char* name() const = 0;  // "imq", "native"
  // SAVERMAIN(info, msg): DX:AX.
  virtual uint32_t saver_main(uint32_t info, uint16_t msg) = 0;
  // The reader's instance handle (the record's +0x0A): the IMQ's; 0 for the native reader.
  virtual uint16_t instance() const { return 0; }
  // After the last SAVERMAIN: the IMQ is freed (INTRMLIB FREESAVER's FreeLibrary, 1:2220).
  virtual void close() {}
};

// The real reader at `guest_path` (C:\WINDOWS\SYSTEM\IMIMXPLY.IMQ, or
// C:\SAVER\IMIMXPLY.IMQ): KERNEL.LoadLibrary (its LibEntry runs) and
// GetProcAddress("saverMain") through the thunks. Null with *why set when
// it cannot be loaded or has no SAVERMAIN.
std::unique_ptr<ImReader> open_imq_reader(win16::Runtime16& rt, const std::string& guest_path, std::string* why);

// The native reader. Null with *why set when guest memory runs out.
std::unique_ptr<ImReader> open_native_reader(win16::Runtime16& rt, std::string* why);

}  // namespace adw::ne16
