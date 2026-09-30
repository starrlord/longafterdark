// The host's own AD_SND (PACKAGES.md §7.4, AUDIO.md §2.10): After Dark 3.0's
// sound library as a Win16 system module named AD_SND, for a package whose
// engine dir holds no AD_SND.DLL (ne16/package.hh host_ad_snd; the AD3
// protocol registers it before the native bridge opens, a rule by file).
// Snoopy's Screen Savers is that package: Image Smith's eight modules were
// installed into the user's After Dark 2.0 or 3.0 and used its AD_SND, six
// of them import it by name, and the native bridge needs one before it loads
// any module (bridge.hh). Nothing of Berkeley's is in here: this is our own
// code for what the library does, as the Snoopy survey measured it
// (research/win/pkg/snoopy: AD_SND 3.0.3 and 3.2 under the lane, and the
// oracle of frames and captures against AD_SND 3.2, byte for byte).
//
// Entries: AD_SND 3.0.3's, all 36, with their ordinals, names and argument
// sizes (the Simpsons package's AD_SND.DLL, version resource 3.0.3; 3.2
// exports the same set and a stub). The native bridge's seven are among
// them (adwSoundInit, adwSoundCleanup, adwGetSystemVolumes,
// adwSetSystemVolumes, adwSetVolume, adwSetSoundMute, adwStopSound), and so
// are AD_SND 1.0's volume pair and every entry the Snoopy modules import.
//
// What they do, as the real library does it:
//   * adwSoundInit(w, err) finds the wave device: mmsystemGetVersion, then
//     waveOutGetNumDevs, then for 8-bit mono PCM at 11025 and at 22050 Hz
//     the first device whose waveOutOpen(WAVE_FORMAT_QUERY) takes it (a
//     second pass adds WAVE_ALLOWSYNC). None, or no device at all
//     (ADSOUNDDEV=0), fails it — 1, and why in err — and every entry that
//     needs the device then answers 0: adwOpenSound, adwLoadSoundResource,
//     adwSoundAsyncCap and the other capabilities, adwStopSound; but
//     adwPlaySound answers 1 (its volume is 0), so the modules run silent.
//     Found: 0, the level 25 until adwSetVolume sets one, and the
//     capabilities from the devices' caps: volume (1) when both have
//     WAVECAPS_VOLUME, async (2) and loop (4) unless the 11 kHz device has
//     WAVECAPS_SYNC. The lane's device gives 7 (adwSoundAsyncCap 2). Init
//     and cleanup also clear the mute and forget the last volume.
//   * A sound is a GlobalAlloc'd record (GMEM_MOVEABLE | GMEM_DDESHARE,
//     0x42 bytes) whose handle the module keeps: its mode, a file name, and
//     the handle of its image. adwLoadSoundResource(hInstance, name) is
//     FindResource(hInstance, name, 3000), LoadResource, LockResource (the
//     image stays locked); adwLoadSoundFile reads a file whole into a global
//     block (_lopen, _llseek, _hread). The mode: 0x200 loops, 0x100 does not
//     (the default), 0x20 plays synchronously, 0x10 asynchronously (the
//     default); adwSetSoundMode refuses both of a pair and a synchronous
//     loop, and changes only the pairs the call names.
//   * adwPlaySound(h): muted, or at level 0, it plays nothing and answers 1;
//     else sndPlaySound(image, SND_MEMORY | SND_NODEFAULT, SND_ASYNC unless
//     synchronous, SND_LOOP when looping) — flags 0x07, 0x06 or 0x0F — with
//     the image locked for the call and unlocked right after it, and the
//     sound is the current one. A synchronous loop (two adwSetSoundMode
//     calls make one) plays nothing: 0. adwStopSound is sndPlaySound(NULL,
//     0); adwCloseSound(2) stops too; adwFreeSound stops the sound when it
//     is the current one, then frees the image (a resource: GlobalUnlock and
//     FreeResource) and the record.
//   * adwSetSoundMute stores the value (adwPlaySound tests it) and stops
//     nothing. adwSetVolume(v): muted, v counts as 0; the value it last took
//     returns 1 at once; 0..100 sets the level and the devices' volume, v ×
//     0xFFFF / 100 on both channels: midiOutSetVolume on every MIDI device
//     with MIDICAPS_VOLUME, then waveOutSetVolume on the wave devices
//     (AD_SND 3.2's levels and order: 50 gives 0x7FFF7FFF on both buses, as
//     AUDIO.md §6.7 has it; 3.0.3's MIDI level followed a curve of its own,
//     which no module of such a package hears — none plays MIDI); anything
//     else answers 0, and is remembered all the same.
//   * adwGetSystemVolumes(&h) saves in a new global block (GHND, 0xB4
//     bytes) the volume of every wave, MIDI and aux device that has one;
//     adwSetSystemVolumes(h) writes them back and frees the block (0; 1 for
//     no block, 2 for one that does not lock, 3 for one that is not such a
//     block). adwSavePreviousVolume/adwRestorePreviousVolume (AD_SND 1.0's
//     pair) do the same for the wave devices alone.
//   * The rest: adwOpenSound, adwPauseSound, adwResumeSound answer 1 with the
//     device; adwIsSoundDone 1 for the current sound (or 0) and 0 for any
//     other (the library cannot ask sndPlaySound); adwGetSoundInfo reads a
//     PCM image's fmt and data chunks; adwCreateSound makes a record, but
//     not one for a named file, so adwPlaySoundFile fails (it does in 3.0.3
//     and 3.2); adwPlaySoundResource loads, sets the mode and plays; no sound
//     effects (adwQuerySfx and adwDoEffect answer 1 for effect 0 alone) and
//     no setup dialog; adwSoundDllVer "3.0.3", VerStr 303.
// There is no mixer path (MMMIXER, a Windows 3.1 sound card's DLL: the host
// has none), no AD_PREFS.INI (3.2 reads [Sound] Mute there, which nothing in
// a package writes; the bridge sets the mute anyway), and no VerStr gate to
// pass: the native bridge has none, and OLDMOD16, which wants 400, never
// gets this library (the AD3 protocol registers it for the native bridge).
//
// Every call it makes goes through the thunks (KERNEL, MMSYSTEM), as the real
// library's imports did, so the census, api16 traces, virtual time and the
// audio path see them alike; its own work costs no instructions.
#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "adw/core/log.h"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win16/shims16.hh"

namespace adw::win16 {

namespace {

constexpr const char* kModule = "AD_SND";
constexpr const char* K = "KERNEL";
constexpr const char* M = "MMSYSTEM";

// A sound record: the mode word, a file name (file sounds), the image's handle.
constexpr uint32_t kRecMode = 0x00, kRecName = 0x02, kRecData = 0x40, kRecSize = 0x42;
constexpr uint16_t kRecAlloc = 0x2002;  // GMEM_MOVEABLE | GMEM_DDESHARE
constexpr uint16_t kModeAsync = 0x0010, kModeSync = 0x0020, kModeNoLoop = 0x0100, kModeLoop = 0x0200;
constexpr uint16_t kKindFile = 0x1000, kKindMemory = 0x2000, kKindResource = 0x4000;
constexpr uint32_t kSoundType = 3000;  // the resource type of a sound (0x0BB8)
// sndPlaySound's flags.
constexpr uint16_t kSndAsync = 0x01, kSndNoDefault = 0x02, kSndMemory = 0x04, kSndLoop = 0x08;
// waveOutOpen's WAVE_FORMAT_QUERY and WAVE_ALLOWSYNC; the caps' volume and sync bits.
constexpr uint32_t kFormatQuery = 0x0001, kAllowSync = 0x0002;
constexpr uint32_t kWaveCapsVolume = 0x0004, kWaveCapsSync = 0x0010, kMidiCapsVolume = 0x0001, kAuxCapsVolume = 0x0001;
// What adwSound*Cap answer.
constexpr uint16_t kCapVolume = 1, kCapAsync = 2, kCapLoop = 4;
constexpr uint16_t kNoDevice = 0xFFFF;
constexpr uint16_t kNoVolume = 0xFFFF;  // adwSetVolume took no value yet
constexpr int16_t kInitLevel = 25;      // the level adwSoundInit leaves, until adwSetVolume sets one

// adwGetSystemVolumes' block: a signature, a mixer flag (0: none), then the
// wave, MIDI and aux devices' volumes — a count, then {DWORD volume, WORD
// saved} for each device.
constexpr uint32_t kSysSize = 0xB4;
constexpr uint16_t kSysSignature = 0x6969, kGhnd = 0x0042;  // GMEM_MOVEABLE | GMEM_ZEROINIT
struct SysGroup {
  uint32_t count, first;
  uint16_t max;
  const char* count_call;
  const char* caps_call;
  uint16_t caps_size;
  uint32_t support_at, volume_bit;
  const char* get_call;
  const char* set_call;
};
const SysGroup kSysGroups[] = {
    {0x04, 0x06, 8, "waveOutGetNumDevs", "waveOutGetDevCaps", 0x30, 44, kWaveCapsVolume, "waveOutGetVolume",
     "waveOutSetVolume"},
    {0x36, 0x38, 8, "midiOutGetNumDevs", "midiOutGetDevCaps", 0x32, 46, kMidiCapsVolume, "midiOutGetVolume",
     "midiOutSetVolume"},
    {0x68, 0x6A, 12, "auxGetNumDevs", "auxGetDevCaps", 0x2C, 40, kAuxCapsVolume, "auxGetVolume", "auxSetVolume"},
};

// Guest memory for what MMSYSTEM reads or writes through a pointer: the two
// PCM formats of the probe, a caps buffer, a DWORD.
constexpr uint32_t kFmt11 = 0x00, kFmt22 = 0x10, kCaps = 0x20, kDword = 0x60, kScratchSize = 0x80;

struct HostAdSnd16 : RuntimeState16 {
  bool device = false;  // adwSoundInit found the wave device (until adwSoundCleanup)
  uint16_t caps = 0;
  uint16_t dev11 = kNoDevice, dev22 = kNoDevice;  // the devices that play 11025 and 22050 Hz
  int16_t level = 0;                               // adwGetVolume's answer
  uint16_t last = kNoVolume;                       // the value adwSetVolume last took
  uint16_t mute = 0;
  uint16_t current = 0;                            // the sound adwPlaySound last started
  uint32_t saved11 = 0xFFFFFFFF, saved22 = 0xFFFFFFFF;  // adwSavePreviousVolume's
  bool midi_listed = false;                        // the MIDI devices with a volume, listed at the first volume
  std::vector<uint16_t> midi;
  uint32_t scratch = 0;                            // far pointer to kFmt11..kDword
};

HostAdSnd16& st(Runtime16& rt) { return rt.state<HostAdSnd16>(); }

uint32_t api(Runtime16& rt, const char* module, const char* name, std::initializer_list<Arg16> args) {
  Shim16Entry* e = rt.shims().find_name(module, name);
  if (!e) throw GuestError16(GuestError16::Kind::fatal, std::string("host AD_SND: no shim ") + module + "." + name);
  return rt.call_far(rt.thunk_far(*e), args);
}
uint16_t api16(Runtime16& rt, const char* module, const char* name, std::initializer_list<Arg16> args) {
  return uint16_t(api(rt, module, name, args));
}

uint32_t scratch(Runtime16& rt) {
  HostAdSnd16& s = st(rt);
  if (!s.scratch) {
    uint16_t h = rt.global().alloc(GlobalHeap16::kZeroInit, kScratchSize);
    s.scratch = h ? rt.global().lock(h) : 0;
    if (!s.scratch) throw GuestError16(GuestError16::Kind::fatal, "host AD_SND: no guest memory");
    rt.ldt().set_tag(uint16_t(s.scratch >> 16), "AD_SND (the host's) data");
  }
  return s.scratch;
}

// PCMWAVEFORMAT: PCM, mono, `rate` Hz, 8 bits.
void write_format(Runtime16& rt, uint32_t at, uint32_t rate) {
  rt.wr16(at, 1);
  rt.wr16(at + 2, 1);
  rt.wr32(at + 4, rate);
  rt.wr32(at + 8, rate);
  rt.wr16(at + 12, 1);
  rt.wr16(at + 14, 8);
}

// A device's dwSupport (its caps structure's, at `support_at`); 0 when the caps call fails.
uint32_t device_support(Runtime16& rt, const char* caps_call, uint16_t dev, uint16_t size, uint32_t support_at) {
  uint32_t caps = scratch(rt) + kCaps;
  if (api16(rt, M, caps_call, {w16(dev), l16(caps), w16(size)}) != 0) return 0;
  return rt.rd32(caps + support_at);
}
uint32_t wave_support(Runtime16& rt, uint16_t dev) { return device_support(rt, "waveOutGetDevCaps", dev, 0x30, 44); }

uint32_t both_channels(uint16_t v) {
  uint32_t x = std::min<uint32_t>(uint32_t(v) * 0xFFFF / 100, 0xFFFF);
  return (x << 16) | x;
}

bool valid_mode(uint16_t m) {
  return !((m & kModeLoop) && (m & kModeNoLoop)) && !((m & kModeAsync) && (m & kModeSync)) &&
         !((m & kModeSync) && (m & kModeLoop));
}

// adwSoundInit's probe: "" when the devices are there, else why not.
std::string find_devices(Runtime16& rt, HostAdSnd16& s) {
  s.dev11 = s.dev22 = kNoDevice;
  s.level = -1;
  s.saved11 = s.saved22 = 0xFFFFFFFF;
  s.midi_listed = false;
  s.midi.clear();
  if (api16(rt, M, "mmsystemGetVersion", {}) == 0) return "Windows multimedia is not installed.";
  uint16_t n = api16(rt, M, "waveOutGetNumDevs", {});
  if (!n) return "No wave output device is installed.";
  uint32_t fmt = scratch(rt);
  write_format(rt, fmt + kFmt11, 11025);
  write_format(rt, fmt + kFmt22, 22050);
  for (uint32_t flags : {kFormatQuery, kFormatQuery | kAllowSync}) {
    for (uint16_t dev = 0; dev < n && (s.dev11 == kNoDevice || s.dev22 == kNoDevice); dev++) {
      if (s.dev11 == kNoDevice &&
          api16(rt, M, "waveOutOpen", {l16(0), w16(dev), l16(fmt + kFmt11), l16(0), l16(0), l16(flags)}) == 0) {
        s.dev11 = dev;
      }
      if (s.dev22 == kNoDevice &&
          api16(rt, M, "waveOutOpen", {l16(0), w16(dev), l16(fmt + kFmt22), l16(0), l16(0), l16(flags)}) == 0) {
        s.dev22 = dev;
      }
    }
  }
  s.level = kInitLevel;
  if (s.dev11 == kNoDevice || s.dev22 == kNoDevice) {
    return "No wave output device plays 8-bit mono sound at 11 and 22 kHz.";
  }
  return {};
}

uint16_t capabilities(Runtime16& rt, const HostAdSnd16& s) {
  uint32_t a = wave_support(rt, s.dev11), b = wave_support(rt, s.dev22);
  uint16_t caps = 0;
  if ((a & kWaveCapsVolume) && (b & kWaveCapsVolume)) caps |= kCapVolume;
  if (!(a & kWaveCapsSync)) caps |= kCapAsync | kCapLoop;
  return caps;
}

// The wave devices' volume (the second only when it is another device): true when every call succeeded.
bool set_wave_volume(Runtime16& rt, const HostAdSnd16& s, uint32_t v11, uint32_t v22) {
  if (api16(rt, M, "waveOutSetVolume", {w16(s.dev11), l16(v11)}) != 0) return false;
  return s.dev22 == s.dev11 || api16(rt, M, "waveOutSetVolume", {w16(s.dev22), l16(v22)}) == 0;
}

// adwSetVolume's work, for 0..100: the level, then the MIDI devices, then the wave devices.
bool apply_volume(Runtime16& rt, HostAdSnd16& s, uint16_t v) {
  s.level = int16_t(v);
  const uint32_t both = both_channels(v);
  if (!s.midi_listed) {
    s.midi_listed = true;
    uint16_t n = api16(rt, M, "midiOutGetNumDevs", {});
    for (uint16_t dev = 0; dev < n; dev++) {
      if (device_support(rt, "midiOutGetDevCaps", dev, 0x32, 46) & kMidiCapsVolume) s.midi.push_back(dev);
    }
  }
  for (uint16_t dev : s.midi) api(rt, M, "midiOutSetVolume", {w16(dev), l16(both)});
  return set_wave_volume(rt, s, both, both);
}

bool stop(Runtime16& rt) {
  HostAdSnd16& s = st(rt);
  if (!s.device) return false;
  api(rt, M, "sndPlaySound", {l16(0), w16(0)});
  s.current = 0;
  return true;
}

// sndPlaySound's flags for a mode; false for a synchronous loop, which plays nothing.
bool play_flags(uint16_t mode, uint16_t* flags) {
  if ((mode & kModeSync) && (mode & kModeLoop)) return false;
  *flags = uint16_t(((mode & kModeSync) ? 0 : kSndAsync) | ((mode & kModeLoop) ? kSndLoop : 0));
  return true;
}

// An image in a global block (a file's) or a loaded resource, locked for the call.
bool play_image(Runtime16& rt, uint16_t data, uint16_t mode, bool resource) {
  uint16_t flags = 0;
  if (!play_flags(mode, &flags)) return false;
  uint32_t image = api(rt, K, resource ? "LockResource" : "GlobalLock", {w16(data)});
  if (!image) return false;
  bool ok = true;
  if (st(rt).level > 0) ok = api16(rt, M, "sndPlaySound", {l16(image), w16(flags | kSndNoDefault | kSndMemory)}) != 0;
  api(rt, K, "GlobalUnlock", {w16(data)});
  return ok;
}

// A file sound: sndPlaySound by name.
bool play_file(Runtime16& rt, uint32_t name, uint16_t mode) {
  uint16_t flags = 0;
  if (!play_flags(mode, &flags)) return false;
  if (st(rt).level == 0) return true;
  return api16(rt, M, "sndPlaySound", {l16(name), w16(flags | kSndNoDefault)}) != 0;
}

// adwPlaySound.
uint16_t play(Runtime16& rt, uint16_t h) {
  HostAdSnd16& s = st(rt);
  if (s.mute || !s.device || s.level <= 0) return 1;  // adwGetVolume answers 0 without the device
  if (!h) return 0;
  uint32_t p = api(rt, K, "GlobalLock", {w16(h)});
  if (!p) return 0;
  uint16_t mode = rt.rd16(p + kRecMode), data = rt.rd16(p + kRecData);
  bool ok = true;
  if (mode & kKindFile) {
    ok = play_file(rt, p + kRecName, mode);
  } else if ((mode & (kKindMemory | kKindResource)) && data) {
    ok = play_image(rt, data, mode, !(mode & kKindMemory));
  }
  api(rt, K, "GlobalUnlock", {w16(h)});
  if (ok) s.current = h;
  return ok ? 1 : 0;
}

// adwCreateSound: a new record, 0 when refused.
uint16_t create_sound(Runtime16& rt, uint32_t name, uint16_t mode) {
  if (!valid_mode(mode)) return 0;
  if (name && (mode & kKindFile)) return 0;  // as in 3.0.3 and 3.2: a named file sound is refused
  uint16_t h = api16(rt, K, "GlobalAlloc", {w16(kRecAlloc), l16(kRecSize)});
  if (!h) return 0;
  uint32_t p = api(rt, K, "GlobalLock", {w16(h)});
  if (!p) {
    api(rt, K, "GlobalFree", {w16(h)});
    return 0;
  }
  rt.write_str(p + kRecName, (mode & kKindFile) ? rt.read_str(name) : std::string(), kRecData - kRecName);
  if (!(mode & (kModeLoop | kModeNoLoop))) mode |= kModeNoLoop;
  if (!(mode & (kModeAsync | kModeSync))) mode |= kModeAsync;
  rt.wr16(p + kRecMode, mode);
  rt.wr16(p + kRecData, 0);
  api(rt, K, "GlobalUnlock", {w16(h)});
  return h;
}

// adwSetSoundMode.
bool set_mode(Runtime16& rt, uint16_t h, uint16_t mode) {
  if (!h || !valid_mode(mode)) return false;
  uint32_t p = api(rt, K, "GlobalLock", {w16(h)});
  if (!p) return false;
  uint16_t m = rt.rd16(p + kRecMode);
  if (mode & kModeLoop) m = uint16_t((m & ~kModeNoLoop) | kModeLoop);
  else if (mode & kModeNoLoop) m = uint16_t((m & ~kModeLoop) | kModeNoLoop);
  if (mode & kModeAsync) m = uint16_t((m & ~kModeSync) | kModeAsync);
  else if (mode & kModeSync) m = uint16_t((m & ~kModeAsync) | kModeSync);
  rt.wr16(p + kRecMode, m);
  api(rt, K, "GlobalUnlock", {w16(h)});
  return true;
}

// adwFreeSound.
bool free_sound(Runtime16& rt, uint16_t h) {
  if (!h) return false;
  bool ok = h != st(rt).current || stop(rt);
  if (ok) {
    uint32_t p = api(rt, K, "GlobalLock", {w16(h)});
    ok = p != 0;
    if (p) {
      uint16_t data = rt.rd16(p + kRecData);
      if (data && (rt.rd16(p + kRecMode) & kKindResource)) {
        api(rt, K, "GlobalUnlock", {w16(data)});
        api(rt, K, "FreeResource", {w16(data)});
      } else if (data) {
        api(rt, K, "GlobalFree", {w16(data)});
      }
      api(rt, K, "GlobalUnlock", {w16(h)});
    }
  }
  api(rt, K, "GlobalFree", {w16(h)});
  return ok;
}

// adwLoadSoundResource.
uint16_t load_resource(Runtime16& rt, uint16_t hinst, uint32_t name) {
  if (!st(rt).device || !name) return 0;
  uint16_t h = create_sound(rt, 0, kKindResource);
  if (!h) return 0;
  bool ok = false;
  if (uint32_t p = api(rt, K, "GlobalLock", {w16(h)})) {
    uint16_t hrsrc = api16(rt, K, "FindResource", {w16(hinst), l16(name), l16(kSoundType)});
    uint16_t data = api16(rt, K, "LoadResource", {w16(hinst), w16(hrsrc)});
    if (data && api(rt, K, "LockResource", {w16(data)})) {
      rt.wr16(p + kRecData, data);
      ok = true;
    } else if (data) {
      api(rt, K, "FreeResource", {w16(data)});
    }
    api(rt, K, "GlobalUnlock", {w16(h)});
  }
  if (ok) return h;
  free_sound(rt, h);
  return 0;
}

// A file read whole into a new global block (GMEM_MOVEABLE), 0 when it cannot be.
uint16_t load_file(Runtime16& rt, uint32_t path) {
  if (!path) return 0;
  uint16_t f = api16(rt, K, "_lopen", {l16(path), w16(0)});
  if (f == 0xFFFF) return 0;  // HFILE_ERROR
  int32_t size = int32_t(api(rt, K, "_llseek", {w16(f), l16(0), w16(2)}));
  uint16_t h = 0;
  if (size > 0) {
    api(rt, K, "_llseek", {w16(f), l16(0), w16(0)});
    h = api16(rt, K, "GlobalAlloc", {w16(0x0002), l16(uint32_t(size))});
    if (h) {
      uint32_t p = api(rt, K, "GlobalLock", {w16(h)});
      int32_t got = p ? int32_t(api(rt, K, "_hread", {w16(f), l16(p), l16(uint32_t(size))})) : -1;
      api(rt, K, "GlobalUnlock", {w16(h)});
      if (got == -1) {
        api(rt, K, "GlobalFree", {w16(h)});
        h = 0;
      }
    }
  }
  api(rt, K, "_lclose", {w16(f)});
  return h;
}

// A PCM image's fmt and data chunks into adwGetSoundInfo's block: +0 the
// data's length, +4 bytes per second, +8 samples per second, +12 channels.
// Another format tag: 1000 and 1000 in the first two, and false.
bool sound_info(Runtime16& rt, uint32_t image, uint32_t info) {
  if (!image || !info) return false;
  auto is = [&](uint64_t at, const char* id) {
    char b[4];
    rt.read_bytes(Runtime16::huge_add(image, uint32_t(at)), b, 4);
    return std::equal(b, b + 4, id);
  };
  auto dword = [&](uint64_t at) { return rt.rd32(Runtime16::huge_add(image, uint32_t(at))); };
  if (!is(0, "RIFF") || !is(8, "WAVE")) return false;
  const uint64_t end = 8 + uint64_t(dword(4));
  // The first chunk named `id` at or after `at`, 0 when none is.
  auto find = [&](uint64_t at, const char* id) -> uint64_t {
    for (; at + 8 <= end; at += 8 + uint64_t(dword(at + 4)) + (dword(at + 4) & 1)) {
      if (is(at, id)) return at;
    }
    return 0;
  };
  uint64_t fmt = find(12, "fmt ");
  if (!fmt) return false;
  uint32_t f = Runtime16::huge_add(image, uint32_t(fmt + 8));
  rt.wr32(info + 4, rt.rd32(f + 8));
  rt.wr32(info + 8, rt.rd32(f + 4));
  rt.wr16(info + 12, rt.rd16(f + 2));
  if (rt.rd16(f) != 1) {
    rt.wr32(info, 1000);
    rt.wr32(info + 4, 1000);
    return false;
  }
  uint64_t data = find(fmt + 8 + uint64_t(dword(fmt + 4)) + (dword(fmt + 4) & 1), "data");
  if (!data) return false;
  rt.wr32(info, dword(data + 4));
  return true;
}

}  // namespace

void register_host_ad_snd(Runtime16& rt) {
  Shim16Registry& r = rt.shims();
  auto add = [&](uint16_t ord, const char* name, int bytes, Shim16Fn fn) {
    r.add(kModule, ord, name, Conv16::pascal_, true, bytes, std::move(fn));
  };

  // ---- the device ----
  add(11, "ADWSOUNDINIT", 6, [](Call16& c) {  // (WORD, LPSTR err): 0 ok, 1 no sound (why in err)
    c.w();
    uint32_t err = c.ptr();
    Runtime16& rt = c.rt;
    HostAdSnd16& s = st(rt);
    s.device = false;
    s.caps = 0;
    s.current = 0;
    s.mute = 0;
    s.last = kNoVolume;
    std::string why = find_devices(rt, s);
    if (why.empty()) {
      s.device = true;
      s.caps = capabilities(rt, s);
      trace("sound", "AD_SND (the host's): wave devices %u (11 kHz) and %u (22 kHz), capabilities %u", s.dev11, s.dev22,
            s.caps);
      return c.ret(0);
    }
    trace("sound", "AD_SND (the host's): no sound: %s", why.c_str());
    if (err) rt.write_str(err, why, 0xFF);
    c.ret(1);
  });
  add(9, "ADWSOUNDCLEANUP", 0, [](Call16& c) {
    HostAdSnd16& s = st(c.rt);
    s.current = 0;
    if (s.device) {
      s.device = false;
      s.mute = 0;
      s.last = kNoVolume;
    }
    c.ret(1);
  });
  add(16, "ADWOPENSOUND", 0, [](Call16& c) { c.ret(st(c.rt).device ? 1 : 0); });
  add(22, "ADWCLOSESOUND", 2, [](Call16& c) {  // (WORD how): 2 stops the sound
    uint16_t how = c.w();
    if (!st(c.rt).device) return c.ret(0);
    if (how == 2) stop(c.rt);
    c.ret(1);
  });
  add(13, "ADWSOUNDLOOPCAP", 0, [](Call16& c) { c.ret(st(c.rt).device ? st(c.rt).caps & kCapLoop : 0); });
  add(24, "ADWSOUNDASYNCCAP", 0, [](Call16& c) { c.ret(st(c.rt).device ? st(c.rt).caps & kCapAsync : 0); });
  add(12, "ADWSOUNDVOLUMECAP", 0, [](Call16& c) { c.ret(st(c.rt).device ? st(c.rt).caps & kCapVolume : 0); });
  add(4, "ADWGETSOUNDDRIVERINFO", 8, [](Call16& c) {  // (LPSTR driver, LPSTR description)
    uint32_t driver = c.ptr(), description = c.ptr();
    if (driver && description) {
      bool on = st(c.rt).device;
      c.rt.write_str(driver, on ? "MMSYSTEM.DLL" : "", 0x40);
      c.rt.write_str(description, on ? "Long After Dark" : "", 0x40);
    }
    c.ret(0);
  });

  // ---- sounds ----
  add(15, "ADWCREATESOUND", 6, [](Call16& c) {  // (LPCSTR name, WORD mode)
    uint32_t name = c.ptr();
    uint16_t mode = c.w();
    c.ret(create_sound(c.rt, name, mode));
  });
  add(28, "ADWLOADSOUNDRESOURCE", 6, [](Call16& c) {  // (HINSTANCE, LPCSTR name)
    uint16_t hinst = c.w();
    uint32_t name = c.ptr();
    c.ret(load_resource(c.rt, hinst, name));
  });
  add(6, "ADWLOADSOUNDFILE", 4, [](Call16& c) {  // (LPCSTR path)
    uint32_t path = c.ptr();
    Runtime16& rt = c.rt;
    if (!st(rt).device || !path) return c.ret(0);
    uint16_t h = create_sound(rt, path, kKindMemory);
    if (!h) return c.ret(0);
    bool ok = false;
    if (uint32_t p = api(rt, K, "GlobalLock", {w16(h)})) {
      uint16_t data = load_file(rt, path);
      rt.wr16(p + kRecData, data);
      ok = data != 0;
      api(rt, K, "GlobalUnlock", {w16(h)});
    }
    if (!ok) {
      free_sound(rt, h);
      h = 0;
    }
    c.ret(h);
  });
  add(33, "ADWSETSOUNDMODE", 4, [](Call16& c) {  // (HSOUND, WORD mode)
    uint16_t h = c.w(), mode = c.w();
    c.ret(set_mode(c.rt, h, mode) ? 1 : 0);
  });
  add(25, "ADWPLAYSOUND", 2, [](Call16& c) { c.ret(play(c.rt, c.w())); });  // (HSOUND)
  add(14, "ADWPLAYSOUNDRESOURCE", 8, [](Call16& c) {  // (HINSTANCE, LPCSTR name, WORD mode)
    uint16_t hinst = c.w();
    uint32_t name = c.ptr();
    uint16_t mode = c.w();
    Runtime16& rt = c.rt;
    const HostAdSnd16& s = st(rt);
    if (s.mute || !s.device || s.level <= 0) return c.ret(1);
    if (!name) return c.ret(0);
    // The sound is never freed: the library's own leak, kept.
    uint16_t h = load_resource(rt, hinst, name);
    c.ret(h && set_mode(rt, h, mode) && play(rt, h) ? 1 : 0);
  });
  add(31, "ADWPLAYSOUNDFILE", 6, [](Call16& c) {  // (LPCSTR path, WORD mode)
    uint32_t path = c.ptr();
    uint16_t mode = c.w();
    Runtime16& rt = c.rt;
    const HostAdSnd16& s = st(rt);
    if (s.mute || !s.device || s.level <= 0) return c.ret(1);
    if (!path) return c.ret(0);
    uint16_t h = create_sound(rt, path, uint16_t(mode | kKindFile));  // refused: a named file sound
    c.ret(h && play(rt, h) ? 1 : 0);
  });
  add(26, "ADWSTOPSOUND", 0, [](Call16& c) { c.ret(stop(c.rt) ? 1 : 0); });
  add(23, "ADWPAUSESOUND", 0, [](Call16& c) { c.ret(st(c.rt).device ? 1 : 0); });
  add(3, "ADWRESUMESOUND", 0, [](Call16& c) { c.ret(st(c.rt).device ? 1 : 0); });
  add(21, "ADWISSOUNDDONE", 2, [](Call16& c) {  // (HSOUND)
    uint16_t h = c.w();
    const HostAdSnd16& s = st(c.rt);
    if (!s.device) return c.ret(0);
    c.ret(!h || h == s.current ? 1 : 0);
  });
  add(2, "ADWFREESOUND", 2, [](Call16& c) { c.ret(free_sound(c.rt, c.w()) ? 1 : 0); });  // (HSOUND)
  add(7, "ADWGETSOUNDINFO", 6, [](Call16& c) {  // (HSOUND, LPSOUNDINFO)
    uint16_t h = c.w();
    uint32_t info = c.ptr();
    Runtime16& rt = c.rt;
    if (!h || !info) return c.ret(0);
    uint32_t p = api(rt, K, "GlobalLock", {w16(h)});
    if (!p) return c.ret(0);
    uint16_t mode = rt.rd16(p + kRecMode), data = rt.rd16(p + kRecData);
    bool ok = false, file = (mode & kKindFile) != 0;
    if (file) data = load_file(rt, p + kRecName);
    if (data) {
      uint32_t image = api(rt, K, "GlobalLock", {w16(data)});
      ok = image && sound_info(rt, image, info);
      api(rt, K, "GlobalUnlock", {w16(data)});
      if (file) api(rt, K, "GlobalFree", {w16(data)});
    }
    api(rt, K, "GlobalUnlock", {w16(h)});
    c.ret(ok ? 1 : 0);
  });

  // ---- mute and volume ----
  add(8, "ADWSETSOUNDMUTE", 2, [](Call16& c) {  // (WORD): the value, as it came
    uint16_t v = c.w();
    st(c.rt).mute = v;
    c.ret(v);
  });
  add(5, "ADWGETSOUNDMUTE", 0, [](Call16& c) { c.ret(st(c.rt).mute); });
  add(29, "ADWSETVOLUME", 2, [](Call16& c) {  // (WORD 0..100)
    uint16_t v = c.w();
    Runtime16& rt = c.rt;
    HostAdSnd16& s = st(rt);
    if (!s.device) return c.ret(0);
    if (s.mute) v = 0;
    if (v == s.last) return c.ret(1);
    bool ok = v <= 100 && apply_volume(rt, s, v);
    s.last = v;
    c.ret(ok ? 1 : 0);
  });
  add(27, "ADWGETVOLUME", 0, [](Call16& c) { c.ret(st(c.rt).device ? uint16_t(st(c.rt).level) : 0); });
  add(101, "ADWGETSYSTEMVOLUMES", 4, [](Call16& c) {  // (LPWORD): 0 ok, 1 no pointer, 2 no memory
    uint32_t out = c.ptr();
    Runtime16& rt = c.rt;
    if (!out) return c.ret(1);
    rt.wr16(out, 0);
    uint16_t h = api16(rt, K, "GlobalAlloc", {w16(kGhnd), l16(kSysSize)});
    if (!h) return c.ret(2);
    uint32_t p = api(rt, K, "GlobalLock", {w16(h)});
    if (!p) {
      api(rt, K, "GlobalFree", {w16(h)});
      return c.ret(2);
    }
    rt.wr16(p, kSysSignature);
    rt.wr16(p + 2, 0);
    for (const SysGroup& g : kSysGroups) {
      rt.wr16(p + g.count, std::min<uint16_t>(api16(rt, M, g.count_call, {}), g.max));
    }
    for (const SysGroup& g : kSysGroups) {
      for (uint16_t dev = 0, n = rt.rd16(p + g.count); dev < n; dev++) {
        uint32_t e = p + g.first + 6u * dev;
        if ((device_support(rt, g.caps_call, dev, g.caps_size, g.support_at) & g.volume_bit) &&
            api16(rt, M, g.get_call, {w16(dev), l16(e)}) == 0) {
          rt.wr16(e + 4, 1);
        }
      }
    }
    api(rt, K, "GlobalUnlock", {w16(h)});
    rt.wr16(out, h);
    c.ret(0);
  });
  add(102, "ADWSETSYSTEMVOLUMES", 2, [](Call16& c) {  // (WORD): 0 ok, 1 no block, 2 no lock, 3 not such a block
    uint16_t h = c.w();
    Runtime16& rt = c.rt;
    st(rt).last = kNoVolume;
    if (!h) return c.ret(1);
    uint32_t p = api(rt, K, "GlobalLock", {w16(h)});
    if (!p) {
      api(rt, K, "GlobalFree", {w16(h)});
      return c.ret(2);
    }
    if (rt.rd16(p) != kSysSignature) {
      api(rt, K, "GlobalUnlock", {w16(h)});
      api(rt, K, "GlobalFree", {w16(h)});
      return c.ret(3);
    }
    for (const SysGroup& g : kSysGroups) {
      for (uint16_t dev = 0, n = std::min<uint16_t>(rt.rd16(p + g.count), g.max); dev < n; dev++) {
        uint32_t e = p + g.first + 6u * dev;
        if (rt.rd16(e + 4)) api(rt, M, g.set_call, {w16(dev), l16(rt.rd32(e))});
      }
    }
    api(rt, K, "GlobalUnlock", {w16(h)});
    api(rt, K, "GlobalFree", {w16(h)});
    c.ret(0);
  });
  // AD_SND 1.0's pair (After Dark 2.0), for the wave devices alone.
  add(20, "ADWSAVEPREVIOUSVOLUME", 0, [](Call16& c) {
    Runtime16& rt = c.rt;
    HostAdSnd16& s = st(rt);
    if (!s.device) return c.ret(0);
    uint32_t v = scratch(rt) + kDword;
    bool ok = api16(rt, M, "waveOutGetVolume", {w16(s.dev11), l16(v)}) == 0;
    if (ok) s.saved11 = rt.rd32(v);
    if (ok && s.dev22 != s.dev11) {
      ok = api16(rt, M, "waveOutGetVolume", {w16(s.dev22), l16(v)}) == 0;
      if (ok) s.saved22 = rt.rd32(v);
    }
    c.ret(ok ? 1 : 0);
  });
  add(10, "ADWRESTOREPREVIOUSVOLUME", 0, [](Call16& c) {
    Runtime16& rt = c.rt;
    HostAdSnd16& s = st(rt);
    if (!s.device) return c.ret(0);
    s.last = kNoVolume;
    c.ret(set_wave_volume(rt, s, s.saved11, s.saved22) ? 1 : 0);
  });

  // ---- what the library has no use for here ----
  add(50, "ADWQUERYSFX", 2, [](Call16& c) { c.ret(c.w() == 0 ? 1 : 0); });  // (WORD effect)
  add(51, "ADWDOEFFECT", 8, [](Call16& c) { c.ret(c.w() == 0 ? 1 : 0); });  // (WORD effect, three WORDs)
  add(19, "ADWSOUNDSETUP", 4, [](Call16& c) { c.ret(0); });                 // no setup dialog
  add(17, "ADWCLOSEDIALOG", 0, [](Call16& c) { c.ret(0); });
  add(1, "WEP", 2, [](Call16& c) { c.ret(1); });

  // ---- versions: the entry set's, 3.0.3 ----
  r.add(kModule, 32, "ADWSOUNDDLLVER", Conv16::pascal_, false, 0,
        [](Call16& c) { c.ret32(c.rt.static_bytes("AD_SND (the host's) version", "3.0.3")); });
  add(100, "VERSTR", 6, [](Call16& c) {  // (LPSTR buf, int size)
    uint32_t buf = c.ptr();
    int16_t size = c.sw();
    if (buf && size > 0) c.rt.write_str(buf, "AD_SND ver 303 (Long After Dark's own)", size_t(size));
    c.ret(303);
  });
}

}  // namespace adw::win16
