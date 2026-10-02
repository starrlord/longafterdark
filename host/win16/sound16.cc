// MMSYSTEM's sound half (sound16.hh, docs/AUDIO.md §8): sndPlaySound,
// waveOut, midiOut (volumes, and the raw port a self-sequencing guest plays
// through), aux, the MCI sequencer's command strings, and the delivery of
// their callbacks and of the multimedia timer events, over the host audio
// engine. Every shim keeps the silent device's answers, byte for byte, while
// no enabled engine is attached (AUDIO.md §3 invariant 4); the calls the
// silent device never answered (waveOutWrite & co.) are registered only with
// an enabled engine, so they stay in the unimplemented census without one —
// except midiOut's, which fail honestly there (a signature-only midiOutOpen
// returned 0, "success", without writing the handle).
#include "win16/sound16.hh"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/log.h"
#include "win16/dos16.hh"
#include "win16/modules16.hh"
#include "win16/shim_families16.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

namespace adw::win16 {

namespace {

constexpr const char* M = "MMSYSTEM";

// MMSYSERR_* / WAVERR_* (the same values in Win16 and Win32).
constexpr uint16_t kMmOk = 0, kMmBadDeviceId = 2, kMmInvalHandle = 5, kMmNoDriver = 6, kMmNoMem = 7,
                   kMmNotSupported = 8, kMmInvalFlag = 10, kMmInvalParam = 11;
constexpr uint16_t kWaveBadFormat = 32, kWaveStillPlaying = 33, kWaveUnprepared = 34;
static_assert(kMmNotSupported == MMSYSERR_NOTSUPPORTED && kWaveStillPlaying == WAVERR_STILLPLAYING &&
              kWaveUnprepared == WAVERR_UNPREPARED && kMmInvalFlag == MMSYSERR_INVALFLAG);
// Win16's WAVE_MAPPER / MIDI_MAPPER: (UINT)-1.
constexpr uint16_t kMapper = 0xFFFF;
// waveOutOpen's dwFlags.
constexpr uint32_t kWaveFormatQuery = 0x0001;
constexpr uint32_t kCallbackMask = 0x00070000, kCallbackWindow = 0x00010000, kCallbackTask = 0x00020000,
                   kCallbackFunction = 0x00030000;
static_assert(kCallbackFunction == CALLBACK_FUNCTION && kCallbackWindow == CALLBACK_WINDOW);
// WAVEHDR (16-bit, 32 bytes): lpData, dwBufferLength, dwBytesRecorded, dwUser,
// dwFlags, dwLoops, lpNext, reserved.
constexpr uint32_t kHdrData = 0, kHdrLength = 4, kHdrFlags = 16, kHdrSize = 32;
constexpr uint32_t kWhdrDone = 0x01, kWhdrPrepared = 0x02, kWhdrBeginLoop = 0x04, kWhdrEndLoop = 0x08,
                   kWhdrInQueue = 0x10;
static_assert(kWhdrInQueue == WHDR_INQUEUE && kWhdrPrepared == WHDR_PREPARED);
// MIDIHDR (16-bit, 28 bytes): lpData, dwBufferLength, dwBytesRecorded, dwUser,
// dwFlags, lpNext, reserved.
constexpr uint32_t kMidiHdrData = 0, kMidiHdrLength = 4, kMidiHdrFlags = 16, kMidiHdrSize = 28;
constexpr uint32_t kMhdrDone = 0x01, kMhdrPrepared = 0x02, kMhdrInQueue = 0x04;
static_assert(kMhdrDone == MHDR_DONE && kMhdrPrepared == MHDR_PREPARED && kMhdrInQueue == MHDR_INQUEUE);
constexpr uint16_t kMmAllocated = 4, kMidiUnprepared = 64, kMidiStillPlaying = 65;
static_assert(kMmAllocated == MMSYSERR_ALLOCATED && kMidiUnprepared == MIDIERR_UNPREPARED &&
              kMidiStillPlaying == MIDIERR_STILLPLAYING && kMmNoDriver == MMSYSERR_NODRIVER);
// Messages and notify codes.
constexpr uint16_t kMmMciNotify = 0x3B9, kMmWomOpen = 0x3BB, kMmWomClose = 0x3BC, kMmWomDone = 0x3BD;
static_assert(kMmMciNotify == MM_MCINOTIFY && kMmWomOpen == MM_WOM_OPEN && kMmWomDone == MM_WOM_DONE);
constexpr uint16_t kMmMomOpen = 0x3C7, kMmMomClose = 0x3C8, kMmMomDone = 0x3C9;
static_assert(kMmMomOpen == MM_MOM_OPEN && kMmMomClose == MM_MOM_CLOSE && kMmMomDone == MM_MOM_DONE);
// Multimedia timer events: at most 16 at once, the table Wine's winmm keeps
// (dlls/winmm/time.c, timers[16]); a periodic event catches up at most this
// much of its missed periods at one delivery point (at least the latest one).
constexpr size_t kMaxTimers = 16;
constexpr uint64_t kTimerMaxLagUs = 250000;
constexpr uint16_t kNotifySuccessful = 1, kNotifySuperseded = 2, kNotifyAborted = 4;
static_assert(kNotifySuperseded == MCI_NOTIFY_SUPERSEDED && kNotifyAborted == MCI_NOTIFY_ABORTED);
// sndPlaySound's flags.
constexpr uint16_t kSndAsync = 0x01, kSndMemory = 0x04, kSndLoop = 0x08, kSndNoStop = 0x10;
// MMTIME types.
constexpr uint16_t kTimeMs = 1, kTimeSamples = 2, kTimeBytes = 4;
// MCIERR_* (MCIERR_BASE 256).
constexpr uint32_t kMciUnrecognizedKeyword = MCIERR_UNRECOGNIZED_KEYWORD,
                   kMciUnrecognizedCommand = MCIERR_UNRECOGNIZED_COMMAND,
                   kMciInvalidDeviceName = MCIERR_INVALID_DEVICE_NAME, kMciDeviceOpen = MCIERR_DEVICE_OPEN,
                   kMciMissingCommandString = MCIERR_MISSING_COMMAND_STRING, kMciParamOverflow = MCIERR_PARAM_OVERFLOW,
                   kMciBadInteger = MCIERR_BAD_INTEGER, kMciMissingParameter = MCIERR_MISSING_PARAMETER,
                   kMciUnsupportedFunction = MCIERR_UNSUPPORTED_FUNCTION, kMciFileNotFound = MCIERR_FILE_NOT_FOUND,
                   kMciCannotUseAll = MCIERR_CANNOT_USE_ALL, kMciOutOfRange = MCIERR_OUTOFRANGE,
                   kMciDuplicateAlias = MCIERR_DUPLICATE_ALIAS, kMciMissingDeviceName = MCIERR_MISSING_DEVICE_NAME,
                   kMciBadTimeFormat = MCIERR_BAD_TIME_FORMAT, kMciNoClosingQuote = MCIERR_NO_CLOSING_QUOTE,
                   kMciInvalidFile = MCIERR_INVALID_FILE, kMciNonapplicableFunction = MCIERR_NONAPPLICABLE_FUNCTION,
                   kMciDeviceNotInstalled = MCIERR_DEVICE_NOT_INSTALLED;
static_assert(kMciDeviceNotInstalled == 256 + 50);
// MCI_ALL_DEVICE_ID in a Win16 WORD.
constexpr uint16_t kMciAllDevices = 0xFFFF;

// A callback waiting for its delivery point (AUDIO.md §8.6).
struct Callback16 {
  uint64_t at = 0, seq = 0;   // virtual µs, then issue order
  bool call = false;          // a CALLBACK_FUNCTION procedure (else a posted message)
  uint16_t hwnd = 0, msg = 0, wparam = 0;
  uint32_t lparam = 0;
  uint32_t proc = 0, instance = 0, p1 = 0, p2 = 0;
  uint16_t handle = 0, ds = 0;
};

struct WaveOut16 {
  audio::StreamId stream = 0;
  audio::WaveFormat src;       // what the guest opened
  audio::WaveFormat pcm;       // what the stream plays (src, or the ADPCM decoded)
  bool adpcm = false;
  uint32_t cb_type = 0, callback = 0, instance = 0;
  uint16_t ds = 0;             // CALLBACK_FUNCTION: its module's DGROUP
  // WAVEHDRs written and not done yet, in order, each with the delivery
  // (Sound16::deliveries) whose procedure wrote it; 0 = written outside one.
  struct Queued {
    uint32_t hdr = 0;
    uint64_t delivery = 0;
  };
  std::deque<Queued> queue;
};

// An open midiOut handle: the one MIDI device's raw port (§8.3).
struct MidiOut16 {
  uint16_t device = 0;         // as opened: 0 or MIDI_MAPPER (midiOutGetID)
  uint32_t cb_type = 0, callback = 0, instance = 0;
  uint16_t ds = 0;             // CALLBACK_FUNCTION: its module's DGROUP
};

// A multimedia timer event (timeSetEvent, system16.cc).
struct Timer16 {
  uint32_t proc = 0, user = 0;
  uint16_t ds = 0;             // the procedure's module's DGROUP
  bool periodic = false;
  uint64_t period = 0;         // µs
  uint64_t due = 0, seq = 0;   // the next call: virtual µs, then issue order
};

struct Mci16 {
  uint16_t id = 0;
  std::string name;            // what commands call it (upper case): the alias, else the element, else the type
  std::string element;         // guest path; "" = the device alone
  audio::SongId song = 0;
  bool notify = false;         // a play's notify is pending
  uint16_t notify_hwnd = 0;
  uint64_t to_at = 0, to_seq = 0;  // `play … to n`: when the song reaches n (0 = none)
  uint64_t to_pos = 0;             // … and n, in µs
};

struct Sound16 : RuntimeState16 {
  audio::Engine* engine = nullptr;
  bool on = false;             // an enabled engine is attached
  audio::VoiceId snd = 0;      // sndPlaySound's one voice
  std::map<uint16_t, WaveOut16> wave;
  std::map<uint16_t, MidiOut16> midi;  // open midiOut handles: one at most (Win16's MIDI out had one client)
  std::map<uint16_t, Mci16> mci;  // by device id
  std::map<uint16_t, Timer16> timers;  // by id; with or without an engine
  uint32_t timers_created = 0;
  uint32_t wave_volume = 0xFFFFFFFF, midi_volume = 0xFFFFFFFF, cd_volume = 0xFFFFFFFF;
  std::vector<Callback16> pending;
  uint64_t seq = 0;
  bool hooked = false;         // the runtime's delivery hook is in (an enabled engine, or a timer set)
  bool delivering = false;
  uint64_t deliveries = 0;     // deliver() runs so far (while delivering: the running one's number)
  uint64_t deliver_t = 0;      // … and the time it delivers up to
  // The CALLBACK_FUNCTION or timer procedure running now, and its due time (cb_time).
  bool in_callback = false;
  uint64_t cb_due = 0, cb_start = 0;
  uint64_t timer_calls = 0, timer_dropped = 0, timer_insns = 0;
  std::vector<audio::Event> events;
};

Sound16& snd(Runtime16& rt) { return rt.state<Sound16>(); }

std::string lower(std::string_view s) {
  std::string l(s);
  for (char& ch : l) ch = char(tolower(uint8_t(ch)));
  return l;
}

// ---- callbacks (§8.6) ------------------------------------------------------------------------------------

// The engine's next event, or a `play … to` stop, whichever comes first (UINT64_MAX: none).
uint64_t engine_due(Sound16& s) {
  if (!s.on) return UINT64_MAX;
  uint64_t due = UINT64_MAX;
  if (audio::Time n = s.engine->next_event_time()) due = n;
  for (const auto& [id, d] : s.mci) {
    if (d.to_at) due = std::min(due, d.to_at);
  }
  return due;
}

void update_due(Runtime16& rt, Sound16& s) {
  if (!s.hooked) return;
  uint64_t due = engine_due(s);
  for (const Callback16& cb : s.pending) due = std::min(due, cb.at);
  for (const auto& [id, tm] : s.timers) due = std::min(due, tm.due);
  rt.set_audio_due(due);
}

// The guest time of an MMSYSTEM call made now. A CALLBACK_FUNCTION or timer
// procedure ran at interrupt time on the original, at its event's time, and
// here reaches the guest at the first safe point after it: the calls it makes
// (MEMMIDI's midiOutShortMsg, a one-shot timer set again) are dated from its
// due time plus the virtual time it has run since, so a procedure delivered
// late still sounds, and schedules, on time. Neither deliver() nor the lane's
// step end renders the engine past a due point before it is delivered (the
// one takes it only as far as the events due, the other only up to
// Runtime16::audio_due()), so those dates are not clamped to a later one the
// engine has seen. MCI commands are dated at peek_us() even there
// (mci_command). Everywhere else, and for every clock the guest reads, it is
// peek_us().
uint64_t cb_time(Runtime16& rt, const Sound16& s) {
  uint64_t now = rt.peek_us();
  return s.in_callback ? s.cb_due + (now - s.cb_start) : now;
}

// When a notification goes out: at `t`, its date — but one a procedure caused
// while deliver() runs it (`by_procedure`: its device opened or closed, a long
// MIDI message sent, a WAVEHDR written that the stream is done with within
// the same delivery) waits for a later delivery point, whatever its date. A
// procedure that answers each notification with a request whose own
// notification is due at once (midiOutLongMsg from MM_MOM_DONE, an empty
// WAVEHDR written from MM_WOM_DONE) then takes one step per delivery point,
// instead of looping inside one delivery while its dates, and virtual time,
// barely move (not at all with ADMIPS=0, or once the 1 s cap on the
// instructions no clock read has charged yet is reached).
uint64_t notify_at(const Sound16& s, uint64_t t, bool by_procedure) {
  return by_procedure ? std::max(t, s.deliver_t + 1) : t;
}

void post_cb(Sound16& s, uint64_t at, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
  Callback16 cb;
  cb.at = at;
  cb.seq = ++s.seq;
  cb.hwnd = hwnd;
  cb.msg = msg;
  cb.wparam = wp;
  cb.lparam = lp;
  s.pending.push_back(cb);
}

// A device's MM_WOM_* / MM_MOM_* in the form it asked for at open.
void device_notify(Sound16& s, uint16_t h, uint32_t cb_type, uint32_t callback, uint32_t instance, uint16_t ds,
                   uint16_t msg, uint32_t p1, uint64_t at) {
  switch (cb_type) {
    case kCallbackWindow:
      post_cb(s, at, uint16_t(callback), msg, h, p1);
      break;
    case kCallbackTask:  // PostAppMessage: the task's queue, no window
      post_cb(s, at, 0, msg, h, p1);
      break;
    case kCallbackFunction: {
      Callback16 cb;
      cb.at = at;
      cb.seq = ++s.seq;
      cb.call = true;
      cb.proc = callback;
      cb.handle = h;
      cb.msg = msg;
      cb.instance = instance;
      cb.p1 = p1;
      cb.ds = ds;
      s.pending.push_back(cb);
      break;
    }
    default:
      break;
  }
}
void wave_notify(Sound16& s, uint16_t h, const WaveOut16& w, uint16_t msg, uint32_t p1, uint64_t at) {
  device_notify(s, h, w.cb_type, w.callback, w.instance, w.ds, msg, p1, at);
}
void midi_notify(Sound16& s, uint16_t h, const MidiOut16& m, uint16_t msg, uint32_t p1, uint64_t at) {
  device_notify(s, h, m.cb_type, m.callback, m.instance, m.ds, msg, p1, at);
}

// Ends a play's pending notify: SUCCESSFUL, SUPERSEDED or ABORTED.
void end_notify(Sound16& s, Mci16& d, uint16_t code, uint64_t at) {
  if (!d.notify) return;
  d.notify = false;
  trace("sound", "MCI device %u (%s): notify %s at %llu us", d.id, d.name.c_str(),
        code == kNotifySuccessful ? "SUCCESSFUL" : code == kNotifySuperseded ? "SUPERSEDED" : "ABORTED",
        (unsigned long long)at);
  if (d.notify_hwnd) post_cb(s, at, d.notify_hwnd, kMmMciNotify, code, d.id);
}

void mark_done(Runtime16& rt, uint32_t hdr) {
  try {
    uint32_t f = rt.rd32(hdr + kHdrFlags);
    rt.wr32(hdr + kHdrFlags, (f & ~kWhdrInQueue) | kWhdrDone);
  } catch (const GuestError16& e) {
    log("win16: MMSYSTEM: a queued WAVEHDR %04X:%04X is gone: %s", hdr >> 16, hdr & 0xFFFF, e.what());
  }
}

void apply_event(Runtime16& rt, Sound16& s, const audio::Event& ev) {
  switch (ev.kind) {
    case audio::Event::Kind::chunk_done:
      for (auto& [h, w] : s.wave) {
        if (w.stream != ev.id) continue;
        uint32_t hdr = uint32_t(ev.cookie);
        auto it = std::find_if(w.queue.begin(), w.queue.end(), [&](const auto& q) { return q.hdr == hdr; });
        if (it == w.queue.end()) break;
        // Written by a procedure of the delivery running now (notify_at).
        const bool fresh = s.delivering && it->delivery == s.deliveries;
        w.queue.erase(it);
        mark_done(rt, hdr);
        wave_notify(s, h, w, kMmWomDone, hdr, notify_at(s, ev.at, fresh));
        break;
      }
      break;
    case audio::Event::Kind::song_end:
      for (auto& [id, d] : s.mci) {
        if (d.song != ev.id) continue;
        trace("sound", "MCI device %u (%s): song end at %llu us", d.id, d.name.c_str(), (unsigned long long)ev.at);
        d.to_at = 0;
        end_notify(s, d, kNotifySuccessful, ev.at);
      }
      break;
    case audio::Event::Kind::voice_end:
      if (ev.id == s.snd) trace("sound", "sndPlaySound: the sound ended at %llu us", (unsigned long long)ev.at);
      break;
  }
}

// The engine's events up to t (and `play … to n` stops), applied in time
// order: WAVEHDRs marked done, callbacks queued.
void sync(Runtime16& rt, Sound16& s, uint64_t t) {
  s.events.clear();
  s.engine->poll(t, s.events);
  struct To {
    uint64_t at, seq;
    uint16_t id;
  };
  std::vector<To> tos;
  for (const auto& [id, d] : s.mci) {
    if (d.to_at && d.to_at <= t) tos.push_back({d.to_at, d.to_seq, id});
  }
  std::sort(tos.begin(), tos.end(), [](const To& a, const To& b) { return a.at != b.at ? a.at < b.at : a.seq < b.seq; });
  std::vector<audio::Event> events;
  events.swap(s.events);
  size_t i = 0, j = 0;
  while (i < events.size() || j < tos.size()) {
    if (j >= tos.size() || (i < events.size() && events[i].at <= tos[j].at)) {
      apply_event(rt, s, events[i++]);
      continue;
    }
    const To& to = tos[j++];
    auto it = s.mci.find(to.id);
    if (it == s.mci.end() || it->second.to_at != to.at) continue;
    Mci16& d = it->second;
    d.to_at = 0;
    // Stopped where `to` said: the engine takes the time as the latest it has
    // seen (the API call that noticed it), so the position is set, not left.
    if (d.song) s.engine->song_seek(d.song, d.to_pos, to.at);
    trace("sound", "MCI device %u (%s): reached its `to` position at %llu us", d.id, d.name.c_str(),
          (unsigned long long)to.at);
    end_notify(s, d, kNotifySuccessful, to.at);
  }
  events.clear();
  s.events.swap(events);
}

struct Delivering {
  explicit Delivering(Sound16& s) : s_(s) { s_.delivering = true; }
  ~Delivering() { s_.delivering = false; }
  Sound16& s_;
};

// A procedure called for an event due at `due` (cb_time).
struct InCallback {
  InCallback(Runtime16& rt, Sound16& s, uint64_t due)
      : s_(s), was_(s.in_callback), due_(s.cb_due), start_(s.cb_start) {
    s.in_callback = true;
    s.cb_due = due;
    s.cb_start = rt.peek_us();
  }
  ~InCallback() {
    s_.in_callback = was_;
    s_.cb_due = due_;
    s_.cb_start = start_;
  }
  Sound16& s_;
  bool was_;
  uint64_t due_, start_;
};

// A periodic timer event whose periods fell behind by more than
// kTimerMaxLagUs (a streamed run's host held up, frames not stepped) drops the
// oldest: at most that much of them — and at least the latest — is caught up
// at once, on the event's own grid.
void bound_lag(Sound16& s, uint16_t id, Timer16& tm, uint64_t t) {
  if (!tm.periodic || tm.due > t) return;
  uint64_t due_now = (t - tm.due) / tm.period + 1;
  uint64_t keep = std::max<uint64_t>(1, kTimerMaxLagUs / tm.period);
  if (due_now <= keep) return;
  uint64_t skip = due_now - keep;
  tm.due += skip * tm.period;
  s.timer_dropped += skip;
  trace("sound", "timer %u: %llu missed period(s) dropped, %llu caught up at %llu us", id, (unsigned long long)skip,
        (unsigned long long)keep, (unsigned long long)t);
}

// Delivers everything due by now, in (time, issue) order: the engine's events
// applied (WAVEHDRs done, notifies queued) as their times come, messages
// posted to the guest's queue, CALLBACK_FUNCTION procedures and one timer
// callback per period called, each dated at its due time (cb_time). The engine
// is taken no further than the events it has due, so what a late procedure
// sends keeps its date (the lane's step end renders no further than the next
// due point either: Runtime16::audio_due). What the procedures cause waits for
// a later delivery point (notify_at), and a timer event they set is dated no
// more than kTimerMaxLagUs back, so every delivery ends. Never re-entered.
void deliver(Runtime16& rt, Sound16& s) {
  if (s.delivering) return;
  Delivering scope(s);
  const uint64_t t = rt.peek_us();
  s.deliveries++;
  s.deliver_t = t;
  for (auto& [id, tm] : s.timers) bound_lag(s, id, tm, t);
  for (;;) {
    uint64_t eng = engine_due(s);
    auto cb = s.pending.end();
    for (auto it = s.pending.begin(); it != s.pending.end(); ++it) {
      if (it->at > t) continue;
      if (cb == s.pending.end() || it->at < cb->at || (it->at == cb->at && it->seq < cb->seq)) cb = it;
    }
    auto tm = s.timers.end();
    for (auto it = s.timers.begin(); it != s.timers.end(); ++it) {
      const Timer16& x = it->second;
      if (x.due > t) continue;
      if (tm == s.timers.end() || x.due < tm->second.due || (x.due == tm->second.due && x.seq < tm->second.seq)) tm = it;
    }
    const uint64_t cb_at = cb == s.pending.end() ? UINT64_MAX : cb->at;
    const uint64_t tm_at = tm == s.timers.end() ? UINT64_MAX : tm->second.due;
    if (eng <= t && eng <= cb_at && eng <= tm_at) {
      sync(rt, s, eng);  // may queue callbacks at its time
      continue;
    }
    if (cb_at == UINT64_MAX && tm_at == UINT64_MAX) break;
    if (cb_at < tm_at || (cb_at == tm_at && cb->seq < tm->second.seq)) {
      Callback16 c = *cb;
      s.pending.erase(cb);
      if (!c.call) {
        user16_post_host(rt, c.hwnd, c.msg, c.wparam, c.lparam);
        trace("sound", "msg %04X (%04X, %08X) posted to %04X at %llu us (due %llu)", c.msg, c.wparam, c.lparam, c.hwnd,
              (unsigned long long)t, (unsigned long long)c.at);
        continue;
      }
      trace("sound", "callback %04X:%04X(%04X, msg %04X, %08X, %08X) at %llu us (due %llu)", c.proc >> 16,
            c.proc & 0xFFFF, c.handle, c.msg, c.instance, c.p1, (unsigned long long)t, (unsigned long long)c.at);
      Regs16In in;
      if (c.ds) in.ds = c.ds;
      InCallback mark(rt, s, c.at);
      rt.call_far(c.proc, {w16(c.handle), w16(c.msg), l16(c.instance), l16(c.p1), l16(c.p2)}, &in);
      continue;
    }
    // One period of a timer event: TimeProc(wID, wMsg 0, dwUser, 0, 0), FAR
    // PASCAL (MEMMIDI's MIDITIMERPROC pops 16 bytes, 1:1380). The event is
    // rescheduled first, so the procedure may kill or set events itself.
    const uint16_t id = tm->first;
    const Timer16 ev = tm->second;
    if (ev.periodic) {
      tm->second.due += ev.period;
      tm->second.seq = ++s.seq;
    } else {
      s.timers.erase(tm);
    }
    if (!rt.ldt().in_use(uint16_t(ev.proc >> 16))) {
      // Its code was freed with the event still set (Windows would have
      // faulted at interrupt time): the event goes, the guest goes on.
      log("win16: MMSYSTEM: timer %u's procedure %04X:%04X is gone; the event is killed", id, ev.proc >> 16,
          ev.proc & 0xFFFF);
      s.timers.erase(id);
      continue;
    }
    s.timer_calls++;
    if (tracing("timer16")) {
      trace("timer16", "timer %u: %04X:%04X(%04X, 0, %08X) due %llu us, at %llu us", id, ev.proc >> 16,
            ev.proc & 0xFFFF, id, ev.user, (unsigned long long)ev.due, (unsigned long long)t);
    }
    Regs16In in;
    if (ev.ds) in.ds = ev.ds;
    InCallback mark(rt, s, ev.due);
    const uint64_t i0 = rt.instructions();
    rt.call_far(ev.proc, {w16(id), w16(0), l16(ev.user), l16(0), l16(0)}, &in);
    s.timer_insns += rt.instructions() - i0;
  }
  update_due(rt, s);
}

// The runtime's delivery hook, once: an enabled engine, or the first timer event.
void ensure_hook(Runtime16& rt, Sound16& s) {
  if (s.hooked) return;
  s.hooked = true;
  rt.set_audio_hook([&rt] { deliver(rt, snd(rt)); });
  rt.set_audio_due(UINT64_MAX);
}

// One audio shim with an enabled engine: the engine's events up to its time
// (cb_time, unless the shim dates itself) first, so WAVEHDRs and notifies are
// current; the next due time after.
struct Op {
  Op(Runtime16& rt, Sound16& s) : Op(rt, s, cb_time(rt, s)) {}
  Op(Runtime16& rt, Sound16& s, uint64_t at) : rt_(rt), s_(s), t(at), e(*s.engine) { sync(rt_, s_, t); }
  ~Op() { update_due(rt_, s_); }
  Runtime16& rt_;
  Sound16& s_;
  const uint64_t t;
  audio::Engine& e;
};

// ---- sound images ----------------------------------------------------------------------------------------

// SND_MEMORY: the RIFF image at fp, as much of it as its segment holds
// (AD_SND unlocks the memory right after the call: the copy is made now).
std::vector<uint8_t> read_riff(Runtime16& rt, uint32_t fp) {
  uint8_t head[12];
  rt.read_bytes(fp, head, sizeof(head));  // a bad pointer faults, as MMSYSTEM's would have
  size_t n = audio::riff_extent(std::span<const uint8_t>(head, sizeof(head)));
  if (!n) return {};
  uint32_t off = fp & 0xFFFF, limit = rt.ldt().limit_of(uint16_t(fp >> 16));
  size_t avail = limit >= off ? size_t(limit - off) + 1 : 0;
  n = std::min(n, avail);
  std::vector<uint8_t> img(n);
  rt.read_bytes(fp, img.data(), n);
  return img;
}

bool read_guest_file(Runtime16& rt, const std::string& path, std::vector<uint8_t>* out) {
  win32::Vfs& vfs = rt.vfs();
  std::string full = vfs.full_path(path), bytes;
  if (!vfs.read_file(full, &bytes)) return false;
  out->assign(bytes.begin(), bytes.end());
  return true;
}

// A sound by name: a file (as given, then in WINDOWS and SYSTEM; ".WAV"
// assumed), else a WIN.INI [sounds] entry ("file,description").
std::vector<uint8_t> load_named(Runtime16& rt, const std::string& name) {
  const Runtime16Options& o = rt.options();
  auto find = [&](const std::string& n, std::vector<uint8_t>* out) {
    if (n.empty()) return false;
    std::vector<std::string> names{n};
    size_t slash = n.find_last_of("\\/:");
    if (n.find('.', slash == std::string::npos ? 0 : slash) == std::string::npos) names.push_back(n + ".WAV");
    for (const std::string& f : names) {
      if (read_guest_file(rt, f, out)) return true;
      if (slash != std::string::npos) continue;
      for (const std::string& dir : {o.windows_dir, o.system_dir}) {
        if (read_guest_file(rt, dir + "\\" + f, out)) return true;
      }
    }
    return false;
  };
  std::vector<uint8_t> img;
  if (find(name, &img)) return img;
  if (auto v = profiles16(rt).get(o.windows_dir + "\\WIN.INI", "sounds", name)) {
    std::string file = v->substr(0, v->find(','));
    while (!file.empty() && isspace(uint8_t(file.back()))) file.pop_back();
    while (!file.empty() && isspace(uint8_t(file.front()))) file.erase(file.begin());
    if (find(file, &img)) return img;
  }
  return {};
}

void stop_snd(Sound16& s, uint64_t t) {
  if (!s.snd) return;
  s.engine->destroy_voice(s.snd, t);
  s.snd = 0;
}

// The image decoded into one engine buffer and played on sndPlaySound's voice.
bool start_snd(Sound16& s, const std::vector<uint8_t>& image, bool loop, uint64_t t, audio::Time* end,
               std::string* what) {
  audio::Wave w;
  if (image.empty() || !audio::parse_wave(image, w)) {
    *what = "not a RIFF WAVE image";
    return false;
  }
  if (!audio::decodable(w.format)) {
    *what = "format tag " + std::to_string(w.format.tag) + " cannot be decoded";
    return false;
  }
  std::vector<uint8_t> pcm = audio::decode(w.format, w.data);
  audio::WaveFormat f = audio::decoded_format(w.format);
  size_t align = std::max<size_t>(f.block_align, 1);
  size_t n = pcm.size() - pcm.size() % align;
  if (!n || n > 0x0FFFFFFF) {
    *what = "no samples";
    return false;
  }
  audio::Engine& e = *s.engine;
  audio::BufferId b = e.create_buffer(f, uint32_t(n));
  if (!b) {
    *what = "no engine buffer";
    return false;
  }
  e.write_buffer(b, 0, std::span<const uint8_t>(pcm.data(), n), t);
  audio::VoiceId v = e.create_voice(b, audio::Bus::wave);
  e.release_buffer(b);
  if (!v) {
    *what = "no engine voice";
    return false;
  }
  e.play(v, loop, t);
  s.snd = v;
  *end = loop ? 0 : e.end_time(v, t);
  char buf[96];
  snprintf(buf, sizeof(buf), "tag %u, %u Hz, %u ch, %u bit, %zu PCM bytes", w.format.tag, w.format.rate,
           w.format.channels, w.format.bits, n);
  *what = buf;
  return true;
}

bool play_sound(Runtime16& rt, Sound16& s, uint32_t name, uint16_t flags, uint64_t t) {
  audio::Engine& e = *s.engine;
  if (!name) {
    stop_snd(s, t);
    trace("sound", "sndPlaySound(NULL, %04X): stopped at %llu us", flags, (unsigned long long)t);
    return true;
  }
  if ((flags & kSndNoStop) && s.snd && e.playing(s.snd, t)) {
    trace("sound", "sndPlaySound(%08X, %04X): SND_NOSTOP while a sound plays", name, flags);
    return false;
  }
  stop_snd(s, t);
  std::vector<uint8_t> image;
  std::string label;
  if (flags & kSndMemory) {
    image = read_riff(rt, name);
    char b[16];
    snprintf(b, sizeof(b), "%04X:%04X", name >> 16, name & 0xFFFF);
    label = b;
  } else {
    label = rt.read_str(name);
    image = load_named(rt, label);
  }
  // SND_LOOP only ever plays asynchronously (a looping synchronous call would never return).
  bool loop = flags & kSndLoop, async = (flags & kSndAsync) || loop;
  audio::Time end = 0;
  std::string what;
  if (!start_snd(s, image, loop, t, &end, &what)) {
    trace("sound", "sndPlaySound(%s, %04X): cannot be played (%s)", label.c_str(), flags, what.c_str());
    return false;
  }
  if (tracing("sound")) {
    std::string how = loop ? "looping" : (async ? "ends at " : "synchronous until ") + std::to_string(end) + " us";
    trace("sound", "sndPlaySound(%s, %04X): voice %u at %llu us, %s, %s", label.c_str(), flags, s.snd,
          (unsigned long long)t, what.c_str(), how.c_str());
  }
  // Synchronous: the call returns when the sound has played (virtual time).
  if (!async && end > t) rt.wait_until_us(end);
  return true;
}

// ---- waveOut ---------------------------------------------------------------------------------------------

// A WAVEFORMAT/PCMWAVEFORMAT/WAVEFORMATEX in guest memory (what its segment holds of it).
bool read_format(Runtime16& rt, uint32_t fp, audio::WaveFormat* f) {
  if (!fp) return false;
  uint32_t off = fp & 0xFFFF, limit = rt.ldt().limit_of(uint16_t(fp >> 16));
  size_t avail = limit >= off ? size_t(limit - off) + 1 : 0;
  std::vector<uint8_t> b(std::min<size_t>(std::max<size_t>(avail, 1), 1024));
  rt.read_bytes(fp, b.data(), b.size());
  return audio::parse_waveformat(b, *f);
}

WaveOut16* wave_of(Sound16& s, uint16_t h) {
  auto it = s.wave.find(h);
  return it == s.wave.end() ? nullptr : &it->second;
}

uint16_t new_device_handle(Sound16& s) {
  // Multiples of 4 above the real-window range (0xC000–0xDFFC): no selector,
  // GDI object, icon or window has one.
  for (uint16_t h = 0xE000; h < 0xFFFC; h += 4) {
    if (!s.wave.count(h) && !s.midi.count(h)) return h;
  }
  return 0;
}

MidiOut16* midi_of(Sound16& s, uint16_t h) {
  if (!s.on) return nullptr;
  auto it = s.midi.find(h);
  return it == s.midi.end() ? nullptr : &it->second;
}

void write_caps_name(uint8_t* p, const char* name) { memcpy(p, name, strlen(name) + 1); }

// ---- MCI (§8.4) ------------------------------------------------------------------------------------------

// Whitespace-separated words; a double-quoted word may hold spaces.
bool mci_words(const std::string& text, std::vector<std::string>* out) {
  size_t i = 0;
  while (i < text.size()) {
    if (isspace(uint8_t(text[i]))) {
      i++;
      continue;
    }
    std::string w;
    if (text[i] == '"') {
      size_t e = text.find('"', i + 1);
      if (e == std::string::npos) return false;
      w = text.substr(i + 1, e - i - 1);
      i = e + 1;
    } else {
      while (i < text.size() && !isspace(uint8_t(text[i]))) w.push_back(text[i++]);
    }
    out->push_back(w);
  }
  return true;
}

bool mci_number(const std::string& s, uint64_t* v) {
  if (s.empty() || s.size() > 10) return false;
  uint64_t n = 0;
  for (char ch : s) {
    if (ch < '0' || ch > '9') return false;
    n = n * 10 + uint64_t(ch - '0');
  }
  *v = n;
  return true;
}

Mci16* mci_find(Sound16& s, const std::string& name) {
  std::string u = upper16(name);
  for (auto& [id, d] : s.mci) {
    if (d.name == u) return &d;
  }
  return nullptr;
}

uint16_t mci_new_id(Sound16& s) {
  uint16_t id = 1;
  while (s.mci.count(id)) id++;
  return id;
}

struct MciCall {
  Runtime16& rt;
  Sound16& s;
  audio::Engine& e;
  uint64_t t;
  uint16_t callback;  // hwndCallback
  bool notify = false, wait = false;
  std::vector<std::string> args;  // after the verb and the device, without wait/notify
  std::string ret;                // the return string
};

// A command with `notify` that completed: SUCCESSFUL to hwndCallback.
void mci_done(MciCall& m, uint16_t id) {
  if (m.notify && m.callback) post_cb(m.s, m.t, m.callback, kMmMciNotify, kNotifySuccessful, id);
}

// A new notify request replaces a pending one (SUPERSEDED).
void mci_supersede(MciCall& m, Mci16& d) {
  if (m.notify) end_notify(m.s, d, kNotifySuperseded, m.t);
}

uint32_t mci_open(MciCall& m, const std::vector<std::string>& w) {
  if (w.size() < 2) return kMciMissingDeviceName;
  std::string spec = w[1], type, element, alias;
  for (size_t i = 2; i < w.size(); i++) {
    std::string k = lower(w[i]);
    if (k == "wait") m.wait = true;
    else if (k == "notify") m.notify = true;
    else if (k == "shareable") continue;
    else if ((k == "alias" || k == "type") && i + 1 < w.size()) (k == "alias" ? alias : type) = w[++i];
    else if (k == "alias" || k == "type") return kMciMissingParameter;
    else return kMciUnrecognizedKeyword;
  }
  if (size_t bang = spec.find('!'); bang != std::string::npos) {
    type = spec.substr(0, bang);
    element = spec.substr(bang + 1);
  } else if (!type.empty()) {
    element = spec;
  } else if (lower(spec) == "sequencer") {
    type = spec;
  } else {
    // An element alone: its extension names the device (WIN.INI [mci extensions]).
    std::string l = lower(spec);
    size_t dot = l.rfind('.');
    std::string ext = dot == std::string::npos ? "" : l.substr(dot + 1);
    if (ext != "mid" && ext != "rmi") return kMciDeviceNotInstalled;
    type = "sequencer";
    element = spec;
  }
  if (lower(type) != "sequencer") return kMciDeviceNotInstalled;
  std::string name = upper16(!alias.empty() ? alias : !element.empty() ? element : type);
  if (mci_find(m.s, name)) return alias.empty() ? kMciDeviceOpen : kMciDuplicateAlias;
  Mci16 d;
  d.name = name;
  if (!element.empty()) {
    std::vector<uint8_t> smf;
    if (!read_guest_file(m.rt, element, &smf)) return kMciFileNotFound;
    std::string why;
    d.song = m.e.load_song(smf, &why);
    if (!d.song) {
      trace("sound", "MCI: %s is not a playable MIDI file (%s)", element.c_str(), why.c_str());
      return kMciInvalidFile;
    }
    d.element = m.rt.vfs().full_path(element);
  }
  d.id = mci_new_id(m.s);
  m.s.mci[d.id] = d;
  m.ret = std::to_string(d.id);
  trace("sound", "MCI device %u: sequencer%s%s as \"%s\"%s", d.id, d.element.empty() ? "" : " with ",
        d.element.c_str(), d.name.c_str(),
        d.song ? (", " + std::to_string(m.e.song_length(d.song) / 1000) + " ms").c_str() : "");
  mci_done(m, d.id);
  return 0;
}

void mci_close_one(MciCall& m, Mci16& d) {
  end_notify(m.s, d, kNotifyAborted, m.t);
  if (d.song) m.e.close_song(d.song, m.t);
  trace("sound", "MCI device %u (%s) closed at %llu us", d.id, d.name.c_str(), (unsigned long long)m.t);
  m.s.mci.erase(d.id);
}

uint32_t mci_play(MciCall& m, Mci16& d) {
  if (m.wait) {
    // Not used by the corpus: a play that blocks until the song ends.
    trace("sound", "MCI: play ... wait is not supported");
    return kMciUnsupportedFunction;
  }
  bool from_set = false, to_set = false;
  uint64_t from = 0, to = 0;
  for (size_t i = 0; i < m.args.size(); i++) {
    std::string k = lower(m.args[i]);
    if (k != "from" && k != "to") return kMciUnrecognizedKeyword;
    if (i + 1 >= m.args.size()) return kMciMissingParameter;
    uint64_t v = 0;
    if (!mci_number(m.args[++i], &v)) return kMciBadInteger;
    (k == "from" ? from : to) = v;
    (k == "from" ? from_set : to_set) = true;
  }
  if (!d.song) return kMciNonapplicableFunction;
  uint64_t len_ms = m.e.song_length(d.song) / 1000;
  if ((from_set && from > len_ms) || (to_set && to > len_ms)) return kMciOutOfRange;
  if (from_set && to_set && to < from) return kMciOutOfRange;
  if (m.notify) end_notify(m.s, d, kNotifySuperseded, m.t);
  else if (from_set) end_notify(m.s, d, kNotifyAborted, m.t);  // playback restarts elsewhere
  if (from_set) m.e.song_seek(d.song, from * 1000, m.t);
  m.e.song_play(d.song, m.t);
  d.to_at = 0;
  if (to_set) {
    uint64_t pos = m.e.song_position(d.song, m.t);
    d.to_at = m.t + (to * 1000 > pos ? to * 1000 - pos : 0);
    d.to_pos = to * 1000;
    d.to_seq = ++m.s.seq;
    if (!d.to_at) d.to_at = 1;
  }
  if (m.notify) {
    d.notify = true;
    d.notify_hwnd = m.callback;
  }
  trace("sound", "MCI device %u (%s): play at %llu us from %llu ms%s%s", d.id, d.name.c_str(), (unsigned long long)m.t,
        (unsigned long long)(m.e.song_position(d.song, m.t) / 1000),
        to_set ? (" to " + std::to_string(to) + " ms").c_str() : "", m.notify ? ", notify" : "");
  return 0;
}

uint32_t mci_status(MciCall& m, Mci16& d) {
  if (m.args.empty()) return kMciMissingParameter;
  std::string item;
  for (const std::string& a : m.args) item += (item.empty() ? "" : " ") + lower(a);
  if (item == "mode") {
    m.ret = d.song && m.e.song_playing(d.song, m.t) ? "playing" : "stopped";
  } else if (item == "length") {
    m.ret = std::to_string(d.song ? m.e.song_length(d.song) / 1000 : 0);
  } else if (item == "position") {
    m.ret = std::to_string(d.song ? m.e.song_position(d.song, m.t) / 1000 : 0);
  } else if (item == "ready") {
    m.ret = "true";
  } else if (item == "number of tracks" || item == "current track") {
    m.ret = "1";
  } else if (item == "time format") {
    m.ret = "milliseconds";
  } else {
    return kMciUnrecognizedKeyword;
  }
  mci_supersede(m, d);
  return 0;
}

uint32_t mci_set(MciCall& m, Mci16& d) {
  if (m.args.empty()) return kMciMissingParameter;
  std::string k = lower(m.args[0]);
  if (k == "time" && m.args.size() >= 3 && lower(m.args[1]) == "format") {
    std::string f = lower(m.args[2]);
    if (f != "milliseconds" && f != "ms") return kMciBadTimeFormat;
  } else if (k == "port" && m.args.size() >= 2) {
    std::string p = lower(m.args[1]);
    if (p != "mapper" && p != "0") return kMciUnsupportedFunction;
  } else {
    return kMciUnrecognizedKeyword;
  }
  mci_supersede(m, d);
  return 0;
}

// A command string at `t`, which mciSendString makes peek_us() even when a
// procedure sends it, not its dated time (cb_time): MCI was no interrupt-time
// API on Win16 (a procedure could not call it), and peek_us() is never behind
// a time the engine has already taken, so a song starts at the command's own
// time and a `play … to` stop, and the notify at it, are reckoned from when
// the song really starts.
uint32_t mci_command(Runtime16& rt, Sound16& s, uint64_t t, const std::string& text, uint16_t callback,
                     std::string* ret) {
  std::vector<std::string> w;
  if (!mci_words(text, &w)) return kMciNoClosingQuote;
  if (w.empty()) return kMciMissingCommandString;
  MciCall m{rt, s, *s.engine, t, callback};
  std::string verb = lower(w[0]);
  uint32_t err = 0;
  if (verb == "open") {
    err = mci_open(m, w);
    *ret = m.ret;
    return err;
  }
  static const char* const kVerbs[] = {"close", "play", "stop", "seek", "status", "set"};
  if (std::find_if(std::begin(kVerbs), std::end(kVerbs), [&](const char* v) { return verb == v; }) == std::end(kVerbs)) {
    return kMciUnrecognizedCommand;
  }
  if (w.size() < 2) return kMciMissingDeviceName;
  for (size_t i = 2; i < w.size(); i++) {
    std::string k = lower(w[i]);
    if (k == "wait") m.wait = true;
    else if (k == "notify") m.notify = true;
    else m.args.push_back(w[i]);
  }
  if (lower(w[1]) == "all") {
    if (verb != "close") return kMciCannotUseAll;
    std::vector<uint16_t> ids;
    for (const auto& [id, d] : s.mci) ids.push_back(id);
    for (uint16_t id : ids) mci_close_one(m, s.mci.at(id));
    mci_done(m, kMciAllDevices);
    return 0;
  }
  Mci16* d = mci_find(s, w[1]);
  if (!d) return kMciInvalidDeviceName;
  uint16_t id = d->id;
  if (verb == "close") {
    if (!m.args.empty()) return kMciUnrecognizedKeyword;
    mci_close_one(m, *d);
  } else if (verb == "play") {
    err = mci_play(m, *d);
  } else if (verb == "stop") {
    if (!m.args.empty()) return kMciUnrecognizedKeyword;
    end_notify(s, *d, kNotifyAborted, m.t);
    d->to_at = 0;
    if (d->song) m.e.song_stop(d->song, m.t);
  } else if (verb == "seek") {
    if (m.args.size() != 2 || lower(m.args[0]) != "to") return m.args.empty() ? kMciMissingParameter : kMciUnrecognizedKeyword;
    std::string where = lower(m.args[1]);
    uint64_t len = d->song ? m.e.song_length(d->song) : 0, pos = 0;
    if (where == "start") {
      pos = 0;
    } else if (where == "end") {
      pos = len;
    } else if (mci_number(where, &pos)) {
      pos *= 1000;
      if (pos > len) return kMciOutOfRange;
    } else {
      return kMciBadInteger;
    }
    end_notify(s, *d, kNotifyAborted, m.t);
    d->to_at = 0;
    if (d->song) m.e.song_seek(d->song, pos, m.t);
  } else if (verb == "status") {
    err = mci_status(m, *d);
  } else {
    err = mci_set(m, *d);
  }
  // A play's notify waits for the song (mci_play); every other command is done now.
  if (!err && verb != "play") mci_done(m, id);
  *ret = m.ret;
  return err;
}

// ---- the calls the silent device never answered (an enabled engine only) ---------------------------------

void register_enabled_only(Runtime16& rt) {
  Shim16Registry& r = rt.shims();
  r.impl(M, "waveOutClose", [](Call16& c) {
    uint16_t h = c.w();
    Sound16& s = snd(c.rt);
    Op op(c.rt, s);
    WaveOut16* w = wave_of(s, h);
    if (!w) return c.ret(kMmInvalHandle);
    if (!w->queue.empty()) return c.ret(kWaveStillPlaying);
    op.e.close_stream(w->stream, op.t);
    WaveOut16 closed = *w;
    s.wave.erase(h);
    wave_notify(s, h, closed, kMmWomClose, 0, notify_at(s, op.t, s.in_callback));
    trace("sound", "waveOutClose(%04X) at %llu us", h, (unsigned long long)op.t);
    c.ret(kMmOk);
  });
  r.impl(M, "waveOutPrepareHeader", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t hdr = c.ptr();
    uint16_t size = c.w();
    Sound16& s = snd(c.rt);
    Op op(c.rt, s);
    if (!wave_of(s, h)) return c.ret(kMmInvalHandle);
    if (!hdr || size < kHdrSize) return c.ret(kMmInvalParam);
    c.rt.wr32(hdr + kHdrFlags, c.rt.rd32(hdr + kHdrFlags) | kWhdrPrepared);
    c.ret(kMmOk);
  });
  r.impl(M, "waveOutUnprepareHeader", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t hdr = c.ptr();
    uint16_t size = c.w();
    Sound16& s = snd(c.rt);
    Op op(c.rt, s);
    if (!wave_of(s, h)) return c.ret(kMmInvalHandle);
    if (!hdr || size < kHdrSize) return c.ret(kMmInvalParam);
    uint32_t f = c.rt.rd32(hdr + kHdrFlags);
    if (f & kWhdrInQueue) return c.ret(kWaveStillPlaying);
    c.rt.wr32(hdr + kHdrFlags, f & ~kWhdrPrepared);
    c.ret(kMmOk);
  });
  r.impl(M, "waveOutWrite", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t hdr = c.ptr();
    uint16_t size = c.w();
    Sound16& s = snd(c.rt);
    Op op(c.rt, s);
    WaveOut16* w = wave_of(s, h);
    if (!w) return c.ret(kMmInvalHandle);
    if (!hdr || size < kHdrSize) return c.ret(kMmInvalParam);
    uint32_t f = c.rt.rd32(hdr + kHdrFlags);
    if (!(f & kWhdrPrepared)) return c.ret(kWaveUnprepared);
    if (f & kWhdrInQueue) return c.ret(kWaveStillPlaying);
    if (f & (kWhdrBeginLoop | kWhdrEndLoop)) trace("sound", "waveOutWrite(%04X): WHDR_BEGINLOOP/ENDLOOP played once", h);
    uint32_t data = c.rt.rd32(hdr + kHdrData), len = c.rt.rd32(hdr + kHdrLength);
    if (len) c.rt.linear(data, len);  // a buffer past its segment faults, as MMSYSTEM's copy would have
    std::vector<uint8_t> bytes(len);
    if (len) c.rt.read_bytes(data, bytes.data(), len);  // copied at the call
    if (w->adpcm) {
      bytes = audio::decode(w->src, bytes);
    } else {
      bytes.resize(bytes.size() - bytes.size() % std::max<size_t>(w->pcm.block_align, 1));
    }
    c.rt.wr32(hdr + kHdrFlags, (f | kWhdrInQueue) & ~kWhdrDone);
    w->queue.push_back({hdr, s.in_callback ? s.deliveries : 0});
    op.e.stream_write(w->stream, bytes, hdr, op.t);
    c.ret(kMmOk);
  });
  r.impl(M, "waveOutPause", [](Call16& c) {
    uint16_t h = c.w();
    Sound16& s = snd(c.rt);
    Op op(c.rt, s);
    WaveOut16* w = wave_of(s, h);
    if (!w) return c.ret(kMmInvalHandle);
    op.e.stream_pause(w->stream, op.t);
    c.ret(kMmOk);
  });
  r.impl(M, "waveOutRestart", [](Call16& c) {
    uint16_t h = c.w();
    Sound16& s = snd(c.rt);
    Op op(c.rt, s);
    WaveOut16* w = wave_of(s, h);
    if (!w) return c.ret(kMmInvalHandle);
    op.e.stream_restart(w->stream, op.t);
    c.ret(kMmOk);
  });
  r.impl(M, "waveOutReset", [](Call16& c) {
    uint16_t h = c.w();
    Sound16& s = snd(c.rt);
    Op op(c.rt, s);
    WaveOut16* w = wave_of(s, h);
    if (!w) return c.ret(kMmInvalHandle);
    op.e.stream_reset(w->stream, op.t);
    sync(c.rt, s, op.t);  // every queued header is done when Reset returns
    c.ret(kMmOk);
  });
  r.impl(M, "waveOutGetPosition", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t p = c.ptr();
    uint16_t size = c.w();
    Sound16& s = snd(c.rt);
    Op op(c.rt, s);
    WaveOut16* w = wave_of(s, h);
    if (!w) return c.ret(kMmInvalHandle);
    if (!p || size < 6) return c.ret(kMmInvalParam);
    uint64_t pos = op.e.stream_position(w->stream, op.t);
    uint64_t frames = pos / std::max<uint16_t>(w->pcm.block_align, 1);
    uint16_t type = c.rt.rd16(p);
    uint64_t v = 0;
    if (type == kTimeMs) {
      v = frames * 1000 / std::max<uint32_t>(w->pcm.rate, 1);
    } else if (type == kTimeSamples) {
      v = frames;
    } else {
      type = kTimeBytes;
      v = w->adpcm ? frames * w->src.avg_bytes / std::max<uint32_t>(w->src.rate, 1) : pos;
    }
    c.rt.wr16(p, type);
    c.rt.wr32(p + 2, uint32_t(v));
    c.ret(kMmOk);
  });
}

// ---- midiOut: the MIDI device's raw port (§8.3), for a guest that sequences itself ------------------------
//
// SWSE opens it (midiOutOpen(MIDI_MAPPER, CALLBACK_NULL)) and MEMMIDI plays
// the song through it from a 4 ms timer event (timeSetEvent, system16.cc).
// Without an enabled engine there is no MIDI device (midiOutGetNumDevs 0):
// midiOutOpen fails honestly — MMSYSERR_NODRIVER for the mapper, BADDEVICEID
// for a device — with *lphMidiOut zeroed, never a success that leaves it
// unwritten, and every handle is invalid.
void register_midi_out(Runtime16& rt) {
  Shim16Registry& r = rt.shims();
  // midiOutOpen(lphMidiOut, uDeviceID, dwCallback, dwInstance, dwFlags).
  r.impl(M, "midiOutOpen", [](Call16& c) {
    uint32_t phmo = c.ptr();
    uint16_t dev = c.w();
    uint32_t callback = c.l(), instance = c.l(), flags = c.l();
    Sound16& s = snd(c.rt);
    if (phmo) c.rt.wr16(phmo, 0);  // a bad pointer faults, as MMSYSTEM's write would have
    if (!s.on) {
      trace("sound", "midiOutOpen(%04X): no MIDI device", dev);
      return c.ret(dev == kMapper ? kMmNoDriver : kMmBadDeviceId);
    }
    Op op(c.rt, s);
    if (dev != 0 && dev != kMapper) return c.ret(kMmBadDeviceId);
    uint32_t cb = flags & kCallbackMask;
    if (cb != 0 && cb != kCallbackWindow && cb != kCallbackTask && cb != kCallbackFunction) return c.ret(kMmInvalFlag);
    if (!phmo || (cb == kCallbackWindow && !user16_window_exists(c.rt, uint16_t(callback))) ||
        (cb == kCallbackFunction && !callback)) {
      return c.ret(kMmInvalParam);
    }
    // A Win16 MIDI output device (the mapper too) had one client at a time.
    if (!s.midi.empty()) return c.ret(kMmAllocated);
    uint16_t h = new_device_handle(s);
    if (!h) return c.ret(kMmNoMem);
    MidiOut16 m;
    m.device = dev;
    m.cb_type = cb;
    m.callback = callback;
    m.instance = instance;
    if (cb == kCallbackFunction) {
      Module16* mod = c.rt.modules().containing(uint16_t(callback >> 16));
      m.ds = mod && mod->dgroup ? mod->dgroup : caller_ds(c);
    }
    s.midi[h] = m;
    c.rt.wr16(phmo, h);
    midi_notify(s, h, m, kMmMomOpen, 0, notify_at(s, op.t, s.in_callback));
    trace("sound", "midiOutOpen(%04X, flags %08X) = %04X at %llu us", dev, flags, h, (unsigned long long)op.t);
    c.ret(kMmOk);
  });
  r.impl(M, "midiOutClose", [](Call16& c) {
    uint16_t h = c.w();
    Sound16& s = snd(c.rt);
    if (!midi_of(s, h)) return c.ret(kMmInvalHandle);
    Op op(c.rt, s);
    // Long messages are done when midiOutLongMsg returns: none can be pending
    // (MIDIERR_STILLPLAYING), and closing sends nothing to the synth.
    MidiOut16 closed = s.midi.at(h);
    s.midi.erase(h);
    midi_notify(s, h, closed, kMmMomClose, 0, notify_at(s, op.t, s.in_callback));
    trace("sound", "midiOutClose(%04X) at %llu us", h, (unsigned long long)op.t);
    c.ret(kMmOk);
  });
  r.impl(M, "midiOutPrepareHeader", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t hdr = c.ptr();
    uint16_t size = c.w();
    Sound16& s = snd(c.rt);
    if (!midi_of(s, h)) return c.ret(kMmInvalHandle);
    if (!hdr || size < kMidiHdrSize || !c.rt.rd32(hdr + kMidiHdrData)) return c.ret(kMmInvalParam);
    c.rt.wr32(hdr + kMidiHdrFlags, c.rt.rd32(hdr + kMidiHdrFlags) | kMhdrPrepared);
    c.ret(kMmOk);
  });
  r.impl(M, "midiOutUnprepareHeader", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t hdr = c.ptr();
    uint16_t size = c.w();
    Sound16& s = snd(c.rt);
    if (!midi_of(s, h)) return c.ret(kMmInvalHandle);
    if (!hdr || size < kMidiHdrSize) return c.ret(kMmInvalParam);
    uint32_t f = c.rt.rd32(hdr + kMidiHdrFlags);
    if (f & kMhdrInQueue) return c.ret(kMidiStillPlaying);
    c.rt.wr32(hdr + kMidiHdrFlags, f & ~kMhdrPrepared);
    c.ret(kMmOk);
  });
  // midiOutShortMsg(hMidiOut, dwMsg): MEMMIDI's timer procedure sends every song event through it.
  r.impl(M, "midiOutShortMsg", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t msg = c.l();
    Sound16& s = snd(c.rt);
    if (!midi_of(s, h)) return c.ret(kMmInvalHandle);
    Op op(c.rt, s);
    op.e.midi_short(msg, op.t);
    if (tracing("midi16")) trace("midi16", "midiOutShortMsg(%04X, %08X) at %llu us", h, msg, (unsigned long long)op.t);
    c.ret(kMmOk);
  });
  // midiOutLongMsg(hMidiOut, lpMidiOutHdr, uSize): the synth takes the buffer
  // at once (copied at the call), so MHDR_DONE is set when the call returns
  // and MM_MOM_DONE goes out at the next delivery point — from inside a
  // procedure, a later one than the delivery running it (notify_at).
  r.impl(M, "midiOutLongMsg", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t hdr = c.ptr();
    uint16_t size = c.w();
    Sound16& s = snd(c.rt);
    MidiOut16* m = midi_of(s, h);
    if (!m) return c.ret(kMmInvalHandle);
    Op op(c.rt, s);
    if (!hdr || size < kMidiHdrSize) return c.ret(kMmInvalParam);
    uint32_t f = c.rt.rd32(hdr + kMidiHdrFlags);
    if (!(f & kMhdrPrepared)) return c.ret(kMidiUnprepared);
    if (f & kMhdrInQueue) return c.ret(kMidiStillPlaying);
    uint32_t data = c.rt.rd32(hdr + kMidiHdrData), len = c.rt.rd32(hdr + kMidiHdrLength);
    if (len) c.rt.linear(data, len);  // a buffer past its segment faults, as MMSYSTEM's read would have
    std::vector<uint8_t> bytes(len);
    if (len) c.rt.read_bytes(data, bytes.data(), len);
    op.e.midi_long(bytes, op.t);
    c.rt.wr32(hdr + kMidiHdrFlags, (f | kMhdrDone) & ~kMhdrInQueue);
    midi_notify(s, h, *m, kMmMomDone, hdr, notify_at(s, op.t, s.in_callback));
    trace("sound", "midiOutLongMsg(%04X, %u bytes) at %llu us", h, len, (unsigned long long)op.t);
    c.ret(kMmOk);
  });
  // midiOutReset: the engine turns off every note left on (and sustain), MEMMIDI's
  // note-offs having gone before (SWSE ENDSONG 1:67de).
  r.impl(M, "midiOutReset", [](Call16& c) {
    uint16_t h = c.w();
    Sound16& s = snd(c.rt);
    if (!midi_of(s, h)) return c.ret(kMmInvalHandle);
    Op op(c.rt, s);
    op.e.midi_reset(op.t);
    trace("sound", "midiOutReset(%04X) at %llu us", h, (unsigned long long)op.t);
    c.ret(kMmOk);
  });
  // Patch caching: the device reports no MIDICAPS_CACHE (midiOutGetDevCaps), so
  // MEMMIDI never asks (1:01bf); asked anyway, MMSYSERR_NOTSUPPORTED, the answer
  // of every device without it (MODM_CACHEPATCHES: "must return
  // MMSYSERR_NOTSUPPORTED" unless an internal synth caches; the mapper, Wine's
  // midimap too, passes no cache support on).
  for (const char* n : {"midiOutCachePatches", "midiOutCacheDrumPatches"}) {
    r.impl(M, n, [](Call16& c) {
      uint16_t h = c.w();
      if (!midi_of(snd(c.rt), h)) return c.ret(kMmInvalHandle);
      c.ret(kMmNotSupported);
    });
  }
  // midiOutGetID(hMidiOut, lpuDeviceID): the id it was opened with (MIDI_MAPPER stays -1).
  r.impl(M, "midiOutGetID", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t p = c.ptr();
    MidiOut16* m = midi_of(snd(c.rt), h);
    if (!m) return c.ret(kMmInvalHandle);
    if (!p) return c.ret(kMmInvalParam);
    c.rt.wr16(p, m->device);
    c.ret(kMmOk);
  });
  r.impl(M, "midiOutMessage", [](Call16& c) {
    uint16_t h = c.w();
    if (!midi_of(snd(c.rt), h)) return c.ret32(kMmInvalHandle);
    c.ret32(kMmNotSupported);
  });
}

}  // namespace

// ---- the engine ------------------------------------------------------------------------------------------

void attach_audio16(Runtime16& rt, audio::Engine* engine) {
  Sound16& s = snd(rt);
  s.engine = engine;
  s.on = engine && engine->enabled();
  if (!s.on) return;
  register_enabled_only(rt);
  // AD_SND 3.2 (AD 3.2, Totally Twisted) sets volumes through the mixer when
  // GetProcAddress finds mixerGetNumDevs/GetLineInfo/GetLineControls/
  // SetControlDetails in MMSYSTEM (1:25d2), whatever mixerGetNumDevs then
  // says; with no mixer device that path sets nothing (1:269e), so the module
  // would play at full volume whatever ADVOLUME is. Without them it takes its
  // Windows 3.1 path (1:2560): midiOutSetVolume and waveOutSetVolume at the
  // module volume, the buses a Windows 95 mixer's synth and wave lines were,
  // as the other AD_SND builds do. Imports by ordinal still resolve.
  for (const char* n : {"mixerGetNumDevs", "mixerGetDevCaps", "mixerOpen", "mixerClose", "mixerMessage",
                        "mixerGetLineInfo", "mixerGetID", "mixerGetLineControls", "mixerGetControlDetails",
                        "mixerSetControlDetails"})
    if (Shim16Entry* e = rt.shims().find_name(M, n)) e->by_name = false;
  // The engines' music gate (AUDIO.md §2.9): LoadLibrary("MCISEQ.DRV") >= 32.
  if (!rt.shims().has_module("MCISEQ")) {
    rt.shims().add("MCISEQ", 1, "DriverProc", Conv16::pascal_, false, 16, [](Call16& c) { c.ret32(0); });
  }
  // Around every "play fred notify" the engines set system.ini [mciseq.drv]
  // disablewarning=true (MCISEQ's MIDI-mapper warning box) and write the old
  // value back (ADXPL310 4:eef1). A machine whose user had ticked "don't show
  // this again" has it true already, and then they write nothing: so no
  // SYSTEM.INI lands in a persistent state directory on every song.
  profiles16(rt).add_seed(rt.options().windows_dir + "\\SYSTEM.INI", "mciseq.drv", "disablewarning", "true");
  ensure_hook(rt, s);
  trace("sound", "MMSYSTEM: the host audio engine is on (volume %d)", engine->config().volume);
}

bool audio16_enabled(Runtime16& rt) { return snd(rt).on; }

void audio16_pump(Runtime16& rt) {
  Sound16& s = snd(rt);
  if (!s.hooked) return;  // sound off and no timer event ever set: nothing can be due
  if (rt.peek_us() >= rt.audio_due()) deliver(rt, s);
  if (s.on) user16_dispatch_host(rt);
}

void audio16_close(Runtime16& rt) {
  Sound16& s = snd(rt);
  s.timers.clear();
  s.midi.clear();
  if (s.hooked) rt.set_audio_due(UINT64_MAX);
  if (!s.on) return;
  uint64_t t = rt.peek_us();
  audio::Engine& e = *s.engine;
  stop_snd(s, t);
  for (auto& [h, w] : s.wave) e.close_stream(w.stream, t);
  s.wave.clear();
  for (auto& [id, d] : s.mci) {
    if (d.song) e.close_song(d.song, t);
  }
  s.mci.clear();
  s.pending.clear();
  rt.set_audio_due(UINT64_MAX);
}

// ---- multimedia timer events -----------------------------------------------------------------------------

uint16_t timer16_set(Runtime16& rt, uint16_t delay_ms, uint32_t proc, uint32_t user, bool periodic, uint16_t ds) {
  Sound16& s = snd(rt);
  if (!delay_ms || !proc || s.timers.size() >= kMaxTimers) return 0;
  uint16_t id;
  do {
    id = uint16_t(++s.timers_created);
  } while (!id || s.timers.count(id));
  Timer16 tm;
  tm.proc = proc;
  tm.user = user;
  tm.ds = ds;
  tm.periodic = periodic;
  tm.period = uint64_t(delay_ms) * 1000;
  // Set from a procedure: from its due time, so a one-shot event set again
  // each time keeps its schedule — but, as a periodic event's periods are, no
  // more than kTimerMaxLagUs behind (a chain after a long stall).
  const uint64_t now = rt.peek_us();
  uint64_t t = cb_time(rt, s);
  if (now - t > kTimerMaxLagUs) t = now - kTimerMaxLagUs;
  tm.due = t + tm.period;
  tm.seq = ++s.seq;
  s.timers[id] = tm;
  ensure_hook(rt, s);
  update_due(rt, s);
  trace("sound", "timeSetEvent(%u ms, %04X:%04X, %08X, %s) = %u at %llu us", delay_ms, proc >> 16, proc & 0xFFFF, user,
        periodic ? "TIME_PERIODIC" : "TIME_ONESHOT", id, (unsigned long long)t);
  return id;
}

bool timer16_kill(Runtime16& rt, uint16_t id) {
  Sound16& s = snd(rt);
  auto it = s.timers.find(id);
  if (it == s.timers.end()) return false;
  s.timers.erase(it);
  update_due(rt, s);
  trace("sound", "timeKillEvent(%u) at %llu us", id, (unsigned long long)rt.peek_us());
  return true;
}

Timer16Stats timer16_stats(Runtime16& rt) {
  Sound16& s = snd(rt);
  return Timer16Stats{s.timers.size(), s.timer_calls, s.timer_dropped, s.timer_insns};
}

// ---- MMSYSTEM --------------------------------------------------------------------------------------------

void register_sound16(Runtime16& rt) {
  Shim16Registry& r = rt.shims();
  register_midi_out(rt);

  // sndPlaySound(lpszSound, wFlags).
  r.impl(M, "sndPlaySound", [](Call16& c) {
    uint32_t name = c.ptr();
    uint16_t flags = c.w();
    bool dev = c.rt.options().sound_device;
    Sound16& s = snd(c.rt);
    if (!s.on) {
      trace("sound", "sndPlaySound(%08X, %04X)%s", name, flags, dev ? ": played silently" : ": no device");
      return c.ret(dev ? 1 : 0);
    }
    Op op(c.rt, s);
    if (!dev) {
      trace("sound", "sndPlaySound(%08X, %04X): no device", name, flags);
      return c.ret(0);
    }
    c.ret(play_sound(c.rt, s, name, flags, op.t) ? 1 : 0);
  });

  // ---- waveOut ----
  r.impl(M, "waveOutGetNumDevs", [](Call16& c) { c.ret(c.rt.options().sound_device ? 1 : 0); });
  r.impl(M, "waveOutGetDevCaps", [](Call16& c) {
    uint16_t dev = c.w();
    uint32_t caps = c.ptr();
    uint16_t size = c.w();
    if (!c.rt.options().sound_device || (dev != 0 && dev != 0xFFFF)) return c.ret(kMmBadDeviceId);
    // WAVEOUTCAPS (16-bit): wMid, wPid, vDriverVersion, szPname[32],
    // dwFormats, wChannels, dwSupport = 48 bytes.
    uint8_t w[48] = {};
    w[0] = 1;     // MM_MICROSOFT
    w[2] = 2;     // MM_WAVE_MAPPER-ish product id
    w[4] = 0x00;  // version 4.00
    w[5] = 0x04;
    write_caps_name(w + 6, snd(c.rt).on ? "Long After Dark" : "Long After Dark (silent)");
    uint32_t formats = 0x0FFF;  // 11/22/44 kHz, mono/stereo, 8/16-bit
    memcpy(w + 38, &formats, 4);
    w[42] = 2;                  // stereo
    uint32_t support = 0x0004 | 0x0008;  // WAVECAPS_VOLUME | WAVECAPS_LRVOLUME
    memcpy(w + 44, &support, 4);
    if (caps && size) c.rt.write_bytes(caps, w, std::min<size_t>(size, sizeof(w)));
    c.ret(0);
  });
  // waveOutOpen(lphWaveOut, uDeviceID, lpFormat, dwCallback, dwInstance, dwFlags).
  r.impl(M, "waveOutOpen", [](Call16& c) {
    uint32_t phwo = c.ptr();
    uint16_t dev = c.w();
    uint32_t pfmt = c.ptr();
    uint32_t callback = c.l(), instance = c.l();
    uint32_t flags = c.l();
    Sound16& s = snd(c.rt);
    if (!s.on) {
      if (!c.rt.options().sound_device || (dev != 0 && dev != 0xFFFF)) return c.ret(kMmBadDeviceId);
      // WAVE_FORMAT_QUERY (1): any PCM format is fine. A real open gets a
      // handle nothing is ever written to (AD_SND only queries).
      if (!(flags & 1) && phwo) c.rt.wr16(phwo, 0x0F10);
      return c.ret(0);
    }
    Op op(c.rt, s);
    if (!c.rt.options().sound_device || (dev != 0 && dev != kMapper)) return c.ret(kMmBadDeviceId);
    audio::WaveFormat f;
    if (!read_format(c.rt, pfmt, &f)) return c.ret(pfmt ? kWaveBadFormat : kMmInvalParam);
    // PCM as it is; IMA-/MS-ADPCM through the mapper, which converted them (§8.3).
    bool pcm = audio::playable(f);
    bool adpcm = !pcm && dev == kMapper && (f.tag == audio::kTagImaAdpcm || f.tag == audio::kTagMsAdpcm) &&
                 audio::decodable(f);
    trace("sound", "waveOutOpen(%04X, tag %u, %u Hz, %u ch, %u bit, flags %08X) %s", dev, f.tag, f.rate, f.channels,
          f.bits, flags, pcm || adpcm ? "ok" : "WAVERR_BADFORMAT");
    if (!pcm && !adpcm) return c.ret(kWaveBadFormat);
    if (flags & kWaveFormatQuery) return c.ret(kMmOk);
    uint32_t cb = flags & kCallbackMask;
    if (cb != 0 && cb != kCallbackWindow && cb != kCallbackTask && cb != kCallbackFunction) return c.ret(kMmInvalFlag);
    if (!phwo || (cb == kCallbackWindow && !user16_window_exists(c.rt, uint16_t(callback))) ||
        (cb == kCallbackFunction && !callback)) {
      return c.ret(kMmInvalParam);
    }
    uint16_t h = new_device_handle(s);
    if (!h) return c.ret(kMmNoMem);
    WaveOut16 w;
    w.src = f;
    w.adpcm = adpcm;
    w.pcm = adpcm ? audio::decoded_format(f) : f;
    w.stream = op.e.open_stream(w.pcm, audio::Bus::wave, op.t);
    if (!w.stream) return c.ret(kMmNoMem);
    w.cb_type = cb;
    w.callback = callback;
    w.instance = instance;
    if (cb == kCallbackFunction) {
      Module16* m = c.rt.modules().containing(uint16_t(callback >> 16));
      w.ds = m && m->dgroup ? m->dgroup : caller_ds(c);
    }
    s.wave[h] = w;
    c.rt.wr16(phwo, h);
    wave_notify(s, h, w, kMmWomOpen, 0, notify_at(s, op.t, s.in_callback));
    c.ret(kMmOk);
  });
  r.impl(M, "waveOutGetVolume", [](Call16& c) {
    c.w();
    uint32_t p = c.ptr();
    if (!c.rt.options().sound_device) return c.ret(kMmNoDriver);
    if (p) c.rt.wr32(p, snd(c.rt).wave_volume);
    c.ret(0);
  });
  r.impl(M, "waveOutSetVolume", [](Call16& c) {
    c.w();
    uint32_t v = c.l();
    if (!c.rt.options().sound_device) return c.ret(kMmNoDriver);
    Sound16& s = snd(c.rt);
    s.wave_volume = v;
    if (s.on) {
      Op op(c.rt, s);
      op.e.set_bus_gain(audio::Bus::wave, audio::gain_from_mm(v), op.t);
      trace("sound", "waveOutSetVolume(%08X) at %llu us", v, (unsigned long long)op.t);
    }
    c.ret(0);
  });

  // No mixer either (and with the engine on, the mixer API is not exported by
  // name at all: attach_audio16).
  r.impl(M, "mixerGetNumDevs", [](Call16& c) { c.ret(0); });
  r.impl(M, "mixerGetDevCaps", [](Call16& c) { c.ret(kMmBadDeviceId); });
  r.impl(M, "mixerOpen", [](Call16& c) { c.ret(kMmBadDeviceId); });

  // ---- midiOut, aux: one MIDI device (the engines' IsMusicAvail), two aux devices ----
  r.impl(M, "midiOutGetNumDevs", [](Call16& c) { c.ret(snd(c.rt).on ? 1 : 0); });
  r.impl(M, "auxGetNumDevs", [](Call16& c) { c.ret(snd(c.rt).on ? 2 : 0); });
  r.impl(M, "midiOutGetDevCaps", [](Call16& c) {
    uint16_t dev = c.w();
    uint32_t caps = c.ptr();
    uint16_t size = c.w();
    if (!snd(c.rt).on || (dev != 0 && dev != kMapper)) return c.ret(kMmBadDeviceId);
    // MIDIOUTCAPS (16-bit): wMid, wPid, vDriverVersion, szPname[32],
    // wTechnology, wVoices, wNotes, wChannelMask, dwSupport = 50 bytes.
    uint8_t w[50] = {};
    w[0] = 1;  // MM_MICROSOFT
    w[2] = 1;  // MM_MIDI_MAPPER
    w[5] = 0x04;
    write_caps_name(w + 6, "Long After Dark MIDI");
    w[38] = 5;  // MOD_MAPPER
    w[44] = 0xFF;
    w[45] = 0xFF;  // all 16 channels
    uint32_t support = 0x0001 | 0x0002;  // MIDICAPS_VOLUME | MIDICAPS_LRVOLUME
    memcpy(w + 46, &support, 4);
    if (caps && size) c.rt.write_bytes(caps, w, std::min<size_t>(size, sizeof(w)));
    c.ret(0);
  });
  r.impl(M, "auxGetDevCaps", [](Call16& c) {
    uint16_t dev = c.w();
    uint32_t caps = c.ptr();
    uint16_t size = c.w();
    if (!snd(c.rt).on || dev > 1) return c.ret(kMmBadDeviceId);
    // AUXCAPS (16-bit): wMid, wPid, vDriverVersion, szPname[32], wTechnology, dwSupport = 44 bytes.
    uint8_t w[44] = {};
    w[0] = 1;
    w[2] = uint8_t(dev + 1);
    w[5] = 0x04;
    write_caps_name(w + 6, dev == 0 ? "Long After Dark CD Audio" : "Long After Dark MIDI");
    w[38] = dev == 0 ? 1 : 2;  // AUXCAPS_CDAUDIO / AUXCAPS_AUXIN
    uint32_t support = 0x0001 | 0x0002;  // AUXCAPS_VOLUME | AUXCAPS_LRVOLUME
    memcpy(w + 40, &support, 4);
    if (caps && size) c.rt.write_bytes(caps, w, std::min<size_t>(size, sizeof(w)));
    c.ret(0);
  });
  // The MIDI bus: midiOut's volume and aux device 1's are one (§6.7).
  auto midi_volume = [](Call16& c, bool set, uint32_t v, uint32_t p) {
    Sound16& s = snd(c.rt);
    if (set) {
      s.midi_volume = v;
      Op op(c.rt, s);
      op.e.set_bus_gain(audio::Bus::midi, audio::gain_from_mm(v), op.t);
      trace("sound", "MIDI volume %08X at %llu us", v, (unsigned long long)op.t);
    } else if (p) {
      c.rt.wr32(p, s.midi_volume);
    }
  };
  r.impl(M, "midiOutGetVolume", [midi_volume](Call16& c) {
    uint16_t dev = c.w();
    uint32_t p = c.ptr();
    if (!snd(c.rt).on) return c.ret(kMmBadDeviceId);
    (void)dev;  // the device id, the mapper's, or a handle: one device
    midi_volume(c, false, 0, p);
    c.ret(0);
  });
  r.impl(M, "midiOutSetVolume", [midi_volume](Call16& c) {
    c.w();
    uint32_t v = c.l();
    if (!snd(c.rt).on) return c.ret(kMmBadDeviceId);
    midi_volume(c, true, v, 0);
    c.ret(0);
  });
  r.impl(M, "auxGetVolume", [midi_volume](Call16& c) {
    uint16_t dev = c.w();
    uint32_t p = c.ptr();
    Sound16& s = snd(c.rt);
    if (!s.on || dev > 1) return c.ret(kMmBadDeviceId);
    if (dev == 0) {
      if (p) c.rt.wr32(p, s.cd_volume);  // CD audio: stored (no disc plays)
    } else {
      midi_volume(c, false, 0, p);
    }
    c.ret(0);
  });
  r.impl(M, "auxSetVolume", [midi_volume](Call16& c) {
    uint16_t dev = c.w();
    uint32_t v = c.l();
    Sound16& s = snd(c.rt);
    if (!s.on || dev > 1) return c.ret(kMmBadDeviceId);
    if (dev == 0) s.cd_volume = v;
    else midi_volume(c, true, v, 0);
    c.ret(0);
  });

  // mciSendString(lpstrCommand, lpstrReturnString, uReturnLength, hwndCallback): §8.4.
  r.impl(M, "mciSendString", [](Call16& c) {
    uint32_t cmd_fp = c.ptr(), ret_fp = c.ptr();
    uint16_t ret_len = c.w(), callback = c.w();
    std::string cmd = c.rt.read_str(cmd_fp);
    Sound16& s = snd(c.rt);
    if (!s.on) {
      trace("sound", "mciSendString(\"%s\") refused: no MCI devices", cmd.c_str());
      return c.ret32(kMciDeviceNotInstalled);
    }
    Op op(c.rt, s, c.rt.peek_us());  // not cb_time: mci_command
    std::string ret;
    uint32_t err = mci_command(c.rt, s, op.t, cmd, callback, &ret);
    if (!err && ret.size() >= ret_len && ret_fp && ret_len) err = kMciParamOverflow;
    if (ret_fp && ret_len) c.rt.write_str(ret_fp, err && err != kMciParamOverflow ? "" : ret, ret_len);
    if (err == kMciUnrecognizedCommand || err == kMciUnsupportedFunction) {
      trace("sound", "mciSendString(\"%s\"): not supported (%u)", cmd.c_str(), err);
    }
    trace("sound", "mciSendString(\"%s\", hwnd %04X) at %llu us -> %u \"%s\"", cmd.c_str(), callback,
          (unsigned long long)op.t, err, ret.c_str());
    c.ret32(err);
  });

  // mciSendCommand(wDeviceID, wMessage, dwParam1, dwParam2): MCI_CLOSE (0x0804)
  // of a device mciSendString opened (MCI_ALL_DEVICE_ID: every one), as its
  // "close" — with MCI_NOTIFY (dwParam1 bit 0) to the MCI_GENERIC_PARMS'
  // dwCallback window. A device ID that is not open is MCIERR_INVALID_DEVICE_ID
  // whatever the command, as MMSYSTEM checked the ID first (Johnny Castaway's
  // one call, MCI_CLOSE of a song it never opens, 5:00ab); every other
  // command is MCIERR_UNSUPPORTED_FUNCTION.
  r.impl(M, "mciSendCommand", [](Call16& c) {
    const uint16_t id = c.w(), msg = c.w();
    const uint32_t flags = c.l(), parms = c.l();
    Sound16& s = snd(c.rt);
    constexpr uint16_t kMciClose = 0x0804;
    constexpr uint32_t kMciNotifyFlag = 0x00000001, kMciInvalidDeviceId = 256 + 1;
    uint32_t err = kMciInvalidDeviceId;
    if (id == kMciAllDevices || s.mci.count(id)) {
      if (msg != kMciClose) {
        err = kMciUnsupportedFunction;
      } else if (!s.on) {
        err = kMciInvalidDeviceId;
      } else {
        std::string cmd = "close " + (id == kMciAllDevices ? std::string("all") : s.mci.at(id).name);
        uint16_t callback = 0;
        if ((flags & kMciNotifyFlag) && parms) {
          callback = uint16_t(c.rt.rd32(parms));
          cmd += " notify";
        }
        Op op(c.rt, s, c.rt.peek_us());
        std::string ret;
        err = mci_command(c.rt, s, op.t, cmd, callback, &ret);
      }
    }
    trace("sound", "mciSendCommand(%u, %04X, %08X, %08X) -> %u", id, msg, flags, parms, err);
    c.ret32(err);
  });
}

}  // namespace adw::win16
