// adw_win16_sound_tests: MMSYSTEM's sound half (win16/sound16.hh, AUDIO.md §8,
// the L16 tests of §10.2), with synthetic guest memory, synthetic callers and
// guest callbacks built as bytes:
//   * the silent device without an enabled engine (today's answers);
//   * device counts, caps and volumes (wave bus, MIDI bus = midiOut = aux 1);
//   * sndPlaySound's flag matrix (ASYNC, synchronous with the time charged,
//     LOOP, NOSTOP, NULL, a file and a WIN.INI [sounds] name), PCM and
//     MS-/IMA-ADPCM images, the copy made at the call;
//   * waveOut: format queries, WAVEHDR flags at the virtual end of a chunk,
//     pause/reset/close errors, MM_WOM_* to a window and to a function;
//   * the mciSendString grammar, MM_MCINOTIFY at a song's end, dispatched by
//     the lane's pump; SUPERSEDED/ABORTED before a command's own notify;
//   * CALLBACK_FUNCTION delivered at the next API call, never re-entrantly; a
//     procedure that answers each MM_MOM_DONE/MM_WOM_DONE with another request
//     takes one step per delivery point (ADMIPS=0, a 1.1 s late one);
//   * midiOut's raw port: open (one client, the mapper or device 0), short,
//     long (MIDIHDR flags), reset, close, MM_MOM_* to a window and to a
//     function, patch caching refused; honest failures without an engine;
//   * multimedia timer events: periodic and one-shot calls at their periods,
//     missed periods caught up (at most 250 ms of them), kill, the 16-event
//     table, re-entrancy, a one-shot set again from its own procedure keeping
//     its schedule, the MIDI a procedure sends dated at its periods, no engine
//     needed, determinism;
//   * the MCISEQ.DRV / TOOLHELP gates;
//   * the real engine (adw/core/audio.h make_engine): a synchronous sound's
//     charged time, MS-ADPCM, a real song's MM_MCINOTIFY, a timer-driven
//     sequencer's notes in the .mid log (plus what 250 callbacks a virtual
//     second cost the host), also in the ne16 lane's order (frames of work
//     without a call, the engine rendered up to the next due point), and a
//     timer procedure's `play … to` reckoned from the song's real start.
// A scripted engine (FakeEngine) stands in for most cases, so the lane's side
// is checked against exact, hand-computed times.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/clock.h"
#include "adw/core/log.h"
#include "win16/dos16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win16/sound16.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

using namespace adw;
using namespace adw::win16;

namespace {

int failures = 0, checks = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    checks++;                                     \
    if (!(cond)) {                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

// ---- a scripted engine ------------------------------------------------------------------------------------
//
// The Engine contract (audio.h) with exact, simple timing: a voice lasts
// frames / rate (rounded up to a µs), stream chunks play back to back at the
// format's byte rate, a song lasts song_length_us. Events come out of poll()
// in (time, issue) order.
class FakeEngine final : public audio::Engine {
 public:
  explicit FakeEngine(bool enabled = true) { cfg.guest_sound = enabled; }

  audio::Config cfg;
  uint64_t song_length_us = 2'000'000;
  std::vector<std::string> calls;

  struct Buf {
    audio::WaveFormat f;
    std::vector<uint8_t> bytes;
    bool released = false;
  };
  struct Voice {
    audio::BufferId b = 0;
    audio::Bus bus = audio::Bus::wave;
    bool on = false, loop = false;
    uint64_t t0 = 0, ev = 0;
    audio::Gain gain;
    uint32_t hz = 0;
  };
  struct Chunk {
    uint64_t cookie = 0, start = 0, end = 0, ev = 0;
    uint32_t bytes = 0;
  };
  struct Stream {
    audio::WaveFormat f;
    std::deque<Chunk> q;
    uint64_t done = 0, tail = 0, paused_at = 0;
    bool paused = false;
  };
  struct Song {
    uint64_t len = 0, pos = 0, t0 = 0, ev = 0;
    bool on = false;
  };
  struct Ev {
    audio::Event e;
    uint64_t seq;
  };
  std::map<uint32_t, Buf> bufs;
  std::map<uint32_t, Voice> voices;
  std::map<uint32_t, Stream> streams;
  std::map<uint32_t, Song> songs;
  std::vector<Ev> evs;
  audio::Gain bus[2];
  uint32_t next_id = 1;
  uint64_t seq = 0, latest = 0;

  uint64_t T(uint64_t t) { return latest = std::max(latest, t); }
  uint64_t add_ev(audio::Event::Kind k, uint32_t id, uint64_t cookie, uint64_t at) {
    audio::Event e;
    e.kind = k;
    e.id = id;
    e.cookie = cookie;
    e.at = at;
    evs.push_back({e, ++seq});
    return seq;
  }
  void cancel(uint64_t& s) {
    if (!s) return;
    uint64_t v = s;
    evs.erase(std::remove_if(evs.begin(), evs.end(), [&](const Ev& e) { return e.seq == v; }), evs.end());
    s = 0;
  }
  static uint64_t dur(uint64_t bytes, const audio::WaveFormat& f) {
    uint64_t frames = bytes / std::max<uint16_t>(f.block_align, 1);
    return (frames * 1'000'000 + f.rate - 1) / f.rate;
  }
  uint64_t voice_end(const Voice& v) { return v.t0 + dur(bufs.at(v.b).bytes.size(), bufs.at(v.b).f); }

  const audio::Config& config() const override { return cfg; }
  audio::BufferId create_buffer(const audio::WaveFormat& pcm, uint32_t bytes) override {
    if (!audio::playable(pcm) || !bytes) return 0;
    uint32_t id = next_id++;
    bufs[id] = Buf{pcm, std::vector<uint8_t>(bytes, 0), false};
    calls.push_back("create_buffer " + std::to_string(bytes));
    return id;
  }
  void write_buffer(audio::BufferId b, uint32_t off, std::span<const uint8_t> bytes, audio::Time t) override {
    T(t);
    auto& v = bufs.at(b).bytes;
    if (off >= v.size()) return;
    std::copy_n(bytes.begin(), std::min<size_t>(bytes.size(), v.size() - off), v.begin() + off);
  }
  void release_buffer(audio::BufferId b) override { bufs.at(b).released = true; }
  const audio::WaveFormat* buffer_format(audio::BufferId b) const override {
    auto it = bufs.find(b);
    return it == bufs.end() ? nullptr : &it->second.f;
  }
  uint32_t buffer_size(audio::BufferId b) const override {
    auto it = bufs.find(b);
    return it == bufs.end() ? 0 : uint32_t(it->second.bytes.size());
  }
  audio::VoiceId create_voice(audio::BufferId b, audio::Bus bus) override {
    if (!bufs.count(b)) return 0;
    uint32_t id = next_id++;
    voices[id] = Voice{b, bus};
    return id;
  }
  void destroy_voice(audio::VoiceId id, audio::Time t) override {
    T(t);
    auto it = voices.find(id);
    if (it == voices.end()) return;
    cancel(it->second.ev);
    voices.erase(it);
    calls.push_back("destroy_voice " + std::to_string(id));
  }
  void play(audio::VoiceId id, bool loop, audio::Time t) override {
    t = T(t);
    Voice& v = voices.at(id);
    v.on = true;
    v.loop = loop;
    v.t0 = t;
    cancel(v.ev);
    if (!loop) v.ev = add_ev(audio::Event::Kind::voice_end, id, 0, voice_end(v));
    calls.push_back("play " + std::to_string(id) + (loop ? " loop" : "") + " @" + std::to_string(t));
  }
  void stop(audio::VoiceId id, audio::Time t) override {
    T(t);
    Voice& v = voices.at(id);
    v.on = false;
    cancel(v.ev);
  }
  void set_cursor(audio::VoiceId, uint32_t, audio::Time) override {}
  uint32_t cursor(audio::VoiceId, audio::Time) override { return 0; }
  bool playing(audio::VoiceId id, audio::Time t) override {
    t = T(t);
    auto it = voices.find(id);
    return it != voices.end() && it->second.on && (it->second.loop || t < voice_end(it->second));
  }
  bool looping(audio::VoiceId id, audio::Time t) override { return playing(id, t) && voices.at(id).loop; }
  audio::Time end_time(audio::VoiceId id, audio::Time t) override {
    t = T(t);
    const Voice& v = voices.at(id);
    return v.on && !v.loop && t < voice_end(v) ? voice_end(v) : 0;
  }
  void set_gain(audio::VoiceId id, audio::Gain g, audio::Time) override { voices.at(id).gain = g; }
  void set_rate(audio::VoiceId id, uint32_t hz, audio::Time) override { voices.at(id).hz = hz; }
  uint32_t rate(audio::VoiceId id) const override { return voices.at(id).hz; }

  audio::StreamId open_stream(const audio::WaveFormat& pcm, audio::Bus, audio::Time t) override {
    if (!audio::playable(pcm)) return 0;
    uint32_t id = next_id++;
    streams[id] = Stream{pcm};
    streams[id].tail = T(t);
    calls.push_back("open_stream");
    return id;
  }
  void stream_write(audio::StreamId s, std::span<const uint8_t> bytes, uint64_t cookie, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    Chunk c;
    c.cookie = cookie;
    c.bytes = uint32_t(bytes.size());
    c.start = std::max(t, st.tail);
    c.end = c.start + dur(bytes.size(), st.f);
    st.tail = c.end;
    if (!st.paused) c.ev = add_ev(audio::Event::Kind::chunk_done, s, cookie, c.end);
    st.q.push_back(c);
    calls.push_back("stream_write " + std::to_string(bytes.size()));
  }
  void stream_pause(audio::StreamId s, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    if (st.paused) return;
    st.paused = true;
    st.paused_at = t;
    for (Chunk& c : st.q) {
      if (c.end > t) cancel(c.ev);
    }
  }
  void stream_restart(audio::StreamId s, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    if (!st.paused) return;
    uint64_t shift = t - st.paused_at;
    for (Chunk& c : st.q) {
      if (c.ev) continue;
      c.start += c.start >= st.paused_at ? shift : 0;
      c.end += shift;
      c.ev = add_ev(audio::Event::Kind::chunk_done, s, c.cookie, c.end);
    }
    st.tail += shift;
    st.paused = false;
  }
  void stream_reset(audio::StreamId s, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    for (Chunk& c : st.q) {
      cancel(c.ev);
      add_ev(audio::Event::Kind::chunk_done, s, c.cookie, t);
    }
    st.q.clear();
    st.done = 0;
    st.tail = t;
    st.paused = false;
  }
  uint64_t stream_position(audio::StreamId s, audio::Time t) override {
    t = T(t);
    Stream& st = streams.at(s);
    uint64_t pos = st.done;
    uint64_t now = st.paused ? st.paused_at : t;
    for (const Chunk& c : st.q) {
      if (c.end <= now) pos += c.bytes;
      else if (c.start < now) pos += (now - c.start) * st.f.avg_bytes / 1'000'000 / st.f.block_align * st.f.block_align;
    }
    return pos;
  }
  bool stream_paused(audio::StreamId s) const override { return streams.at(s).paused; }
  void set_stream_gain(audio::StreamId, audio::Gain, audio::Time) override {}
  void close_stream(audio::StreamId s, audio::Time t) override {
    stream_reset(s, t);
    streams.erase(s);
    calls.push_back("close_stream");
  }

  audio::SongId load_song(std::span<const uint8_t> smf, std::string* error) override {
    if (smf.size() < 4 || memcmp(smf.data(), "MThd", 4) != 0) {
      if (error) *error = "not an SMF";
      return 0;
    }
    uint32_t id = next_id++;
    songs[id] = Song{song_length_us};
    calls.push_back("load_song");
    return id;
  }
  void settle(Song& g, uint64_t t) {
    if (g.on && t >= g.t0 + (g.len - g.pos)) {
      g.pos = g.len;
      g.on = false;
    }
  }
  void song_play(audio::SongId s, audio::Time t) override {
    t = T(t);
    Song& g = songs.at(s);
    settle(g, t);
    if (g.on) return;
    g.on = true;
    g.t0 = t;
    g.ev = add_ev(audio::Event::Kind::song_end, s, 0, t + (g.len - g.pos));
    calls.push_back("song_play @" + std::to_string(t));
  }
  void song_stop(audio::SongId s, audio::Time t) override {
    t = T(t);
    Song& g = songs.at(s);
    settle(g, t);
    if (g.on) g.pos = std::min(g.len, g.pos + (t - g.t0));
    g.on = false;
    cancel(g.ev);
    calls.push_back("song_stop @" + std::to_string(t));
  }
  void song_seek(audio::SongId s, uint64_t pos, audio::Time t) override {
    song_stop(s, t);
    Song& g = songs.at(s);
    g.pos = std::min(pos, g.len);
  }
  uint64_t song_position(audio::SongId s, audio::Time t) override {
    t = T(t);
    Song& g = songs.at(s);
    settle(g, t);
    return g.on ? std::min(g.len, g.pos + (t - g.t0)) : g.pos;
  }
  uint64_t song_length(audio::SongId s) const override { return songs.at(s).len; }
  bool song_playing(audio::SongId s, audio::Time t) override {
    t = T(t);
    Song& g = songs.at(s);
    settle(g, t);
    return g.on;
  }
  void close_song(audio::SongId s, audio::Time t) override {
    T(t);
    auto it = songs.find(s);
    if (it == songs.end()) return;
    cancel(it->second.ev);
    songs.erase(it);
    calls.push_back("close_song");
  }
  // Raw MIDI: every call, with the time passed and the time the engine took
  // (never earlier than the latest seen, as the real one).
  struct Midi {
    std::string what;  // "short", "long", "reset"
    uint32_t msg = 0;
    std::vector<uint8_t> bytes;
    uint64_t asked = 0, t = 0;
  };
  std::vector<Midi> midi;
  void midi_short(uint32_t msg, audio::Time t) override { midi.push_back({"short", msg, {}, t, T(t)}); }
  void midi_long(std::span<const uint8_t> b, audio::Time t) override {
    midi.push_back({"long", 0, std::vector<uint8_t>(b.begin(), b.end()), t, T(t)});
  }
  void midi_reset(audio::Time t) override { midi.push_back({"reset", 0, {}, t, T(t)}); }
  void set_bus_gain(audio::Bus b, audio::Gain g, audio::Time t) override {
    T(t);
    bus[int(b)] = g;
  }
  audio::Gain bus_gain(audio::Bus b) const override { return bus[int(b)]; }
  void poll(audio::Time t, std::vector<audio::Event>& out) override {
    t = T(t);
    std::vector<Ev> due;
    for (const Ev& e : evs) {
      if (e.e.at <= t) due.push_back(e);
    }
    std::sort(due.begin(), due.end(), [](const Ev& a, const Ev& b) { return a.e.at != b.e.at ? a.e.at < b.e.at : a.seq < b.seq; });
    for (const Ev& e : due) {
      out.push_back(e.e);
      evs.erase(std::remove_if(evs.begin(), evs.end(), [&](const Ev& x) { return x.seq == e.seq; }), evs.end());
      if (e.e.kind == audio::Event::Kind::chunk_done) {
        auto it = streams.find(e.e.id);
        if (it == streams.end()) continue;
        auto& q = it->second.q;
        for (auto c = q.begin(); c != q.end(); ++c) {
          if (c->ev == e.seq) {
            it->second.done += c->bytes;
            q.erase(c);
            break;
          }
        }
      }
    }
  }
  audio::Time next_event_time() override {
    uint64_t best = 0;
    for (const Ev& e : evs) {
      if (!best || e.e.at < best) best = e.e.at;
    }
    return best;
  }
  void advance(audio::Time t) override { T(t); }
  void shutdown(audio::Time t) override { T(t); }
  audio::Stats stats() const override { return {}; }

  int live_voices() const { return int(voices.size()); }
  const Voice* only_voice() const { return voices.size() == 1 ? &voices.begin()->second : nullptr; }
};

// ---- a machine -------------------------------------------------------------------------------------------

struct Rig {
  // A 1 ms frame grid: time is moved by frames().
  VirtualClock clock{VirtualClock::Mode::fixed_step, 1000};
  Runtime16 rt;
  FakeEngine fake;
  // insns_per_us 0: no modeled instruction time, so peek_us() is the frame
  // grid exactly and times can be checked to the µs.
  explicit Rig(bool enabled = true, bool sound_device = true, audio::Engine* engine = nullptr,
               uint32_t insns_per_us = 100)
      : rt(options(sound_device, insns_per_us), clock), fake(enabled) {
    clock.set_read_step_us(0);
    clock.begin_frame();
    register_all16(rt);
    attach_audio16(rt, engine ? engine : &fake);
  }
  static Runtime16Options options(bool sound_device, uint32_t insns_per_us = 100) {
    Runtime16Options o;
    o.sound_device = sound_device;
    o.insns_per_us = insns_per_us;
    return o;
  }
  uint32_t api(const char* module, const char* name, std::initializer_list<Arg16> args) {
    Shim16Entry* e = rt.shims().find_name(module, name);
    if (!e) throw std::runtime_error(std::string("no shim ") + name);
    return rt.call_far(rt.thunk_far(*e), args);
  }
  uint16_t api16(const char* module, const char* name, std::initializer_list<Arg16> args) {
    return uint16_t(api(module, name, args));
  }
  uint16_t code(const std::vector<uint8_t>& bytes) {
    GlobalBlock* b = rt.global().alloc_block(uint32_t(bytes.size() + 16), true, 0, 0);
    rt.mem().memcpy(b->base, bytes.data(), bytes.size());
    return b->sel;
  }
  uint16_t data(uint32_t size) { return rt.global().alloc_block(size, false, 0, 0)->sel; }
  uint32_t bytes(const std::vector<uint8_t>& v) {
    uint16_t s = data(uint32_t(v.size() + 16));
    rt.write_bytes(uint32_t(s) << 16, v.data(), v.size());
    return uint32_t(s) << 16;
  }
  uint32_t str(const std::string& s) { return rt.static_bytes("test: " + s, s); }
  // Moves the frame grid on to at least `us`.
  void to(uint64_t us) {
    while (clock.now_us() < us) clock.begin_frame();
  }
  uint64_t now() const { return rt.peek_us(); }
  // Any API call: the delivery point of §8.6.
  void tick() { api("MMSYSTEM", "timeGetTime", {}); }
  uint32_t mci(const std::string& cmd, std::string* ret = nullptr, uint16_t hwnd = 0, uint16_t cap = 128) {
    uint32_t buf = uint32_t(data(256)) << 16;
    uint32_t r = api("MMSYSTEM", "mciSendString", {l16(str(cmd)), l16(buf), w16(cap), w16(hwnd)});
    if (ret) *ret = rt.read_str(buf);
    return r;
  }
};

void append(std::vector<uint8_t>& v, const std::vector<uint8_t>& w) { v.insert(v.end(), w.begin(), w.end()); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)), v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, uint16_t(x)), put16(v, uint16_t(x >> 16)); }

// A RIFF WAVE image.
std::vector<uint8_t> riff(const std::vector<uint8_t>& fmt, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> body = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
  put32(body, uint32_t(fmt.size()));
  append(body, fmt);
  if (fmt.size() & 1) body.push_back(0);
  append(body, {'d', 'a', 't', 'a'});
  put32(body, uint32_t(data.size()));
  append(body, data);
  if (data.size() & 1) body.push_back(0);
  std::vector<uint8_t> out = {'R', 'I', 'F', 'F'};
  put32(out, uint32_t(body.size()));
  append(out, body);
  return out;
}
std::vector<uint8_t> pcm8(uint32_t rate, uint32_t n) {
  std::vector<uint8_t> d(n);
  for (uint32_t i = 0; i < n; i++) d[i] = uint8_t(0x80 + ((i * 7) & 0x3F));
  return riff(audio::waveformat_bytes(audio::pcm_format(rate, 1, 8)), d);
}

// A window procedure that appends (msg, wParam, lParam) to a record at
// DATA:0 (WORD count, then 8-byte entries) and returns 0.
std::vector<uint8_t> wndproc_recorder(uint16_t data) {
  return {0x55, 0x8B, 0xEC, 0x06, 0x53, 0xB8, uint8_t(data), uint8_t(data >> 8), 0x8E, 0xC0,  // push bp.. mov es,DATA
          0x26, 0x8B, 0x1E, 0x00, 0x00,                                                        // mov bx,es:[0]
          0xC1, 0xE3, 0x03, 0x83, 0xC3, 0x02,                                                  // shl bx,3; add bx,2
          0x8B, 0x46, 0x0C, 0x26, 0x89, 0x07,                                                  // msg
          0x8B, 0x46, 0x0A, 0x26, 0x89, 0x47, 0x02,                                            // wParam
          0x8B, 0x46, 0x06, 0x26, 0x89, 0x47, 0x04,                                            // lParam lo
          0x8B, 0x46, 0x08, 0x26, 0x89, 0x47, 0x06,                                            // lParam hi
          0x26, 0xFF, 0x06, 0x00, 0x00,                                                        // inc word es:[0]
          0x5B, 0x07, 0x31, 0xC0, 0x99, 0x5D, 0xCA, 0x0A, 0x00};                               // ... retf 10
}

// A waveOut CALLBACK_FUNCTION (hwo, msg, dwInstance, dwParam1, dwParam2),
// PASCAL: DATA:2 counts the nesting depth, entries from DATA:8 hold msg, hwo,
// dwParam1's low word and the depth at entry; it calls an API (timeGetTime)
// itself — a delivery point, where nothing may be delivered re-entrantly.
std::vector<uint8_t> function_recorder(uint16_t data, uint32_t api_thunk) {
  std::vector<uint8_t> v = {0x55, 0x8B, 0xEC, 0x06, 0x53, 0xB8, uint8_t(data), uint8_t(data >> 8), 0x8E, 0xC0,
                            0x26, 0xFF, 0x06, 0x02, 0x00,        // inc word es:[2] (depth)
                            0x26, 0x8B, 0x1E, 0x00, 0x00,        // mov bx,es:[0]
                            0xC1, 0xE3, 0x03, 0x83, 0xC3, 0x08,  // shl bx,3; add bx,8
                            0x8B, 0x46, 0x12, 0x26, 0x89, 0x07,  // msg
                            0x8B, 0x46, 0x14, 0x26, 0x89, 0x47, 0x02,  // hwo
                            0x8B, 0x46, 0x0A, 0x26, 0x89, 0x47, 0x04,  // dwParam1 lo
                            0x26, 0xA1, 0x02, 0x00, 0x26, 0x89, 0x47, 0x06,  // depth
                            0x26, 0xFF, 0x06, 0x00, 0x00};                  // inc word es:[0]
  append(v, {0x9A, uint8_t(api_thunk), uint8_t(api_thunk >> 8), uint8_t(api_thunk >> 16), uint8_t(api_thunk >> 24)});
  append(v, {0x26, 0xFF, 0x0E, 0x02, 0x00,  // dec word es:[2]
             0x5B, 0x07, 0x5D, 0xCA, 0x10, 0x00});
  return v;
}

struct Rec {
  uint16_t msg, wp;
  uint32_t lp;
};
std::vector<Rec> window_records(Rig& g, uint16_t data) {
  std::vector<Rec> out;
  uint32_t base = uint32_t(data) << 16;
  uint16_t n = g.rt.rd16(base);
  for (uint16_t i = 0; i < n; i++) {
    uint32_t e = base + 2 + 8u * i;
    out.push_back({g.rt.rd16(e), g.rt.rd16(e + 2), g.rt.rd32(e + 4)});
  }
  return out;
}
void clear_records(Rig& g, uint16_t data) { g.rt.wr16(uint32_t(data) << 16, 0); }

// A window of a class whose procedure records into `rec`.
uint16_t make_window(Rig& g, uint16_t rec) {
  uint16_t cs = g.code(wndproc_recorder(rec));
  uint16_t ds = g.data(256);
  uint32_t d = uint32_t(ds) << 16;
  g.rt.write_str(d, "SNDTEST", 16);
  g.rt.wr32(d + 0x20 + 2, uint32_t(cs) << 16);
  g.rt.wr32(d + 0x20 + 22, d);
  g.api("USER", "RegisterClass", {l16(d + 0x20)});
  uint16_t hwnd = uint16_t(g.api("USER", "CreateWindowEx", {l16(0), l16(d), l16(d), l16(0), w16(0), w16(0), w16(10),
                                                            w16(10), w16(0), w16(0), w16(0), l16(0)}));
  clear_records(g, rec);
  return hwnd;
}

std::string records_text(const std::vector<Rec>& r) {
  std::string s;
  for (const Rec& x : r) {
    char b[48];
    snprintf(b, sizeof(b), "%s%04X/%X/%X", s.empty() ? "" : " ", x.msg, x.wp, x.lp);
    s += b;
  }
  return s;
}

// ---- the silent device --------------------------------------------------------------------------------------

void test_disabled() {
  for (int pass = 0; pass < 2; pass++) {
    // No engine at all, then a disabled one (ADSOUND/ADAUDIOOUT unset).
    FakeEngine off(false);
    VirtualClock clock{VirtualClock::Mode::fixed_step, 1000};
    Runtime16 rt{Runtime16Options{}, clock};
    register_all16(rt);
    attach_audio16(rt, pass ? &off : nullptr);
    auto api = [&](const char* name, std::initializer_list<Arg16> args) {
      return rt.call_far(rt.thunk_far(*rt.shims().find_name("MMSYSTEM", name)), args);
    };
    uint32_t buf = uint32_t(rt.global().alloc_block(512, false, 0, 0)->sel) << 16;
    CHECK(!audio16_enabled(rt), "pass %d: not enabled", pass);
    CHECK((api("waveOutGetNumDevs", {}) & 0xFFFF) == 1, "pass %d: one (silent) wave device", pass);
    CHECK((api("waveOutGetDevCaps", {w16(0), l16(buf), w16(48)}) & 0xFFFF) == 0 &&
              rt.read_str(buf + 6) == "Long After Dark (silent)",
          "pass %d: the silent device's name (%s)", pass, rt.read_str(buf + 6).c_str());
    CHECK((api("midiOutGetNumDevs", {}) & 0xFFFF) == 0 && (api("auxGetNumDevs", {}) & 0xFFFF) == 0 &&
              (api("mixerGetNumDevs", {}) & 0xFFFF) == 0,
          "pass %d: no MIDI, aux or mixer devices", pass);
    CHECK((api("midiOutGetDevCaps", {w16(0), l16(buf), w16(50)}) & 0xFFFF) == 2 &&
              (api("auxGetVolume", {w16(1), l16(buf)}) & 0xFFFF) == 2,
          "pass %d: MMSYSERR_BADDEVICEID for MIDI and aux", pass);
    std::vector<uint8_t> img = pcm8(11025, 100);
    rt.write_bytes(buf, img.data(), img.size());
    CHECK((api("sndPlaySound", {l16(buf), w16(7)}) & 0xFFFF) == 1 && off.calls.empty(),
          "pass %d: sndPlaySound reports the sound played, the engine is not called", pass);
    uint32_t cmd = rt.static_bytes("open", "open sequencer");
    CHECK(api("mciSendString", {l16(cmd), l16(0), w16(0), w16(0)}) == 306, "pass %d: MCI refused (306)", pass);
    // Today's waveOutOpen: any format is fine.
    std::vector<uint8_t> ima = audio::waveformat_bytes(audio::ima_adpcm_format(22050, 1));
    rt.write_bytes(buf + 256, ima.data(), ima.size());
    CHECK((api("waveOutOpen", {l16(0), w16(0), l16(buf + 256), l16(0), l16(0), l16(1)}) & 0xFFFF) == 0,
          "pass %d: a format query succeeds as it always did", pass);
    CHECK(!rt.shims().find_name("MMSYSTEM", "waveOutWrite")->fn, "pass %d: waveOutWrite stays unimplemented", pass);
    // No MIDI device: midiOutOpen fails honestly and zeroes the handle (a
    // signature-only one returned 0 without writing it); no handle is valid.
    rt.wr16(buf, 0x1234);
    CHECK((api("midiOutOpen", {l16(buf), w16(0xFFFF), l16(0), l16(0), l16(0)}) & 0xFFFF) == 6 && rt.rd16(buf) == 0,
          "pass %d: midiOutOpen(MIDI_MAPPER) = MMSYSERR_NODRIVER, handle 0", pass);
    rt.wr16(buf, 0x1234);
    CHECK((api("midiOutOpen", {l16(buf), w16(0), l16(0), l16(0), l16(0)}) & 0xFFFF) == 2 && rt.rd16(buf) == 0,
          "pass %d: midiOutOpen(0) = MMSYSERR_BADDEVICEID, handle 0", pass);
    CHECK((api("midiOutShortMsg", {w16(0xE000), l16(0x00403C90)}) & 0xFFFF) == 5 &&
              (api("midiOutReset", {w16(0xE000)}) & 0xFFFF) == 5 && (api("midiOutClose", {w16(0xE000)}) & 0xFFFF) == 5,
          "pass %d: every midiOut handle is invalid", pass);
    CHECK(off.midi.empty(), "pass %d: the engine hears nothing", pass);
    uint16_t err = 0;
    CHECK(rt.modules().load("MCISEQ.DRV", &err) == nullptr, "pass %d: no MCISEQ.DRV", pass);
    Module16* mm = rt.modules().load("MMSYSTEM.DLL", &err);
    CHECK(mm && rt.modules().proc_address(mm, "mixerGetNumDevs") != 0 &&
              rt.modules().proc_address(mm, "mixerSetControlDetails") != 0,
          "pass %d: the mixer API is there by name, as always", pass);
  }
}

// ---- devices and volumes ------------------------------------------------------------------------------------

void test_devices() {
  Rig g;
  uint32_t buf = uint32_t(g.data(512)) << 16;
  CHECK(audio16_enabled(g.rt), "enabled");
  CHECK(g.api16("MMSYSTEM", "waveOutGetNumDevs", {}) == 1, "one wave device");
  CHECK(g.api16("MMSYSTEM", "waveOutGetDevCaps", {w16(0), l16(buf), w16(48)}) == 0 &&
            g.rt.read_str(buf + 6) == "Long After Dark",
        "the wave device's name (%s)", g.rt.read_str(buf + 6).c_str());
  CHECK((g.rt.rd32(buf + 44) & 0x0C) == 0x0C && !(g.rt.rd32(buf + 44) & 0x0010), "VOLUME|LRVOLUME, not SYNC");
  CHECK(g.api16("MMSYSTEM", "midiOutGetNumDevs", {}) == 1, "one MIDI device");
  CHECK(g.api16("MMSYSTEM", "midiOutGetDevCaps", {w16(0), l16(buf), w16(50)}) == 0 &&
            g.rt.read_str(buf + 6) == "Long After Dark MIDI" && g.rt.rd32(buf + 46) == 3 && g.rt.rd16(buf + 44) == 0xFFFF,
        "the MIDI device's caps (%s)", g.rt.read_str(buf + 6).c_str());
  CHECK(g.api16("MMSYSTEM", "midiOutGetDevCaps", {w16(0xFFFF), l16(buf), w16(50)}) == 0 &&
            g.api16("MMSYSTEM", "midiOutGetDevCaps", {w16(1), l16(buf), w16(50)}) == 2,
        "MIDI_MAPPER answers, device 1 does not exist");
  CHECK(g.api16("MMSYSTEM", "auxGetNumDevs", {}) == 2, "two aux devices");
  CHECK(g.api16("MMSYSTEM", "auxGetDevCaps", {w16(0), l16(buf), w16(44)}) == 0 && g.rt.rd16(buf + 38) == 1 &&
            g.api16("MMSYSTEM", "auxGetDevCaps", {w16(1), l16(buf), w16(44)}) == 0 && g.rt.rd16(buf + 38) == 2 &&
            g.api16("MMSYSTEM", "auxGetDevCaps", {w16(2), l16(buf), w16(44)}) == 2,
        "aux 0 CD audio, aux 1 the MIDI bus, no aux 2");
  CHECK(g.api16("MMSYSTEM", "mixerGetNumDevs", {}) == 0, "no mixer");
  // Nor the mixer API by name, so AD_SND 3.2 takes its wave/MIDI volume path
  // (attach_audio16) instead of a mixer path that sets nothing.
  uint16_t mm = g.api16("KERNEL", "LoadLibrary", {l16(g.str("MMSYSTEM.DLL"))});
  CHECK(mm >= 32 && g.api("KERNEL", "GetProcAddress", {w16(mm), l16(g.str("mixerGetNumDevs"))}) == 0 &&
            g.api("KERNEL", "GetProcAddress", {w16(mm), l16(g.str("MIXERSETCONTROLDETAILS"))}) == 0 &&
            g.api("KERNEL", "GetProcAddress", {w16(mm), l16(g.str("waveOutSetVolume"))}) != 0,
        "the mixer API is not exported by name (MMSYSTEM %u)", mm);
  // The wave bus.
  CHECK(g.api16("MMSYSTEM", "waveOutSetVolume", {w16(0), l16(0x7FFF7FFF)}) == 0 &&
            g.fake.bus[0] == audio::gain_from_mm(0x7FFF7FFF),
        "waveOutSetVolume -> the wave bus gain");
  CHECK(g.api16("MMSYSTEM", "waveOutGetVolume", {w16(0), l16(buf)}) == 0 && g.rt.rd32(buf) == 0x7FFF7FFF,
        "waveOutGetVolume reads it back");
  // The MIDI bus: midiOut and aux 1 are one volume; aux 0 is its own.
  CHECK(g.api16("MMSYSTEM", "midiOutSetVolume", {w16(0), l16(0x40004000)}) == 0 &&
            g.fake.bus[1] == audio::gain_from_mm(0x40004000),
        "midiOutSetVolume -> the MIDI bus gain");
  CHECK(g.api16("MMSYSTEM", "auxGetVolume", {w16(1), l16(buf)}) == 0 && g.rt.rd32(buf) == 0x40004000,
        "aux 1 reads the MIDI volume");
  CHECK(g.api16("MMSYSTEM", "auxSetVolume", {w16(1), l16(0x20003000)}) == 0 &&
            g.fake.bus[1] == audio::gain_from_mm(0x20003000),
        "auxSetVolume(1) -> the MIDI bus gain");
  CHECK(g.api16("MMSYSTEM", "midiOutGetVolume", {w16(0), l16(buf)}) == 0 && g.rt.rd32(buf) == 0x20003000,
        "midiOutGetVolume reads aux 1's");
  CHECK(g.api16("MMSYSTEM", "auxSetVolume", {w16(0), l16(0x11112222)}) == 0 &&
            g.api16("MMSYSTEM", "auxGetVolume", {w16(0), l16(buf)}) == 0 && g.rt.rd32(buf) == 0x11112222 &&
            g.fake.bus[1] == audio::gain_from_mm(0x20003000),
        "aux 0 (CD) is stored and moves no bus");
  CHECK(g.api16("MMSYSTEM", "midiOutShortMsg", {w16(1), l16(0x90)}) == 5, "midiOutShortMsg on no handle: INVALHANDLE");

  // ADSOUNDDEV=0: no wave device, sound on or off; the MIDI device stays.
  Rig n(true, false);
  uint32_t nb = uint32_t(n.data(512)) << 16;
  std::vector<uint8_t> img = pcm8(11025, 100);
  n.rt.write_bytes(nb, img.data(), img.size());
  CHECK(n.api16("MMSYSTEM", "waveOutGetNumDevs", {}) == 0 && n.api16("MMSYSTEM", "sndPlaySound", {l16(nb), w16(7)}) == 0 &&
            n.fake.voices.empty() && n.api16("MMSYSTEM", "midiOutGetNumDevs", {}) == 1,
        "ADSOUNDDEV=0: no wave device, sndPlaySound FALSE, MIDI still there");
}

// ---- sndPlaySound -------------------------------------------------------------------------------------------

void test_snd() {
  Rig g;
  // 11025 frames of 8-bit mono at 22050 Hz: 500 ms.
  std::vector<uint8_t> img = pcm8(22050, 11025);
  const size_t at = size_t(std::search(img.begin(), img.end(), std::begin("data"), std::begin("data") + 4) - img.begin()) + 8;
  uint32_t p = g.bytes(img);
  uint64_t t0 = g.now();
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0007)}) == 1, "SND_ASYNC|SND_NODEFAULT|SND_MEMORY plays");
  const FakeEngine::Voice* v = g.fake.only_voice();
  CHECK(v && v->on && !v->loop && v->bus == audio::Bus::wave, "one voice on the wave bus, playing once");
  if (v) {
    const FakeEngine::Buf& b = g.fake.bufs.at(v->b);
    CHECK(b.f == audio::pcm_format(22050, 1, 8) && b.bytes.size() == 11025 &&
              std::equal(b.bytes.begin(), b.bytes.end(), img.begin() + at) && b.released,
          "the buffer holds the image's samples (%zu bytes) and is released to its voice", b.bytes.size());
    CHECK(g.fake.voice_end(*v) - v->t0 == 500000, "it lasts 500 ms");
  }
  CHECK(g.now() - t0 < 1000, "SND_ASYNC returns at once (%llu us)", (unsigned long long)(g.now() - t0));
  // The copy was made at the call: AD_SND unlocks (and may discard) right after.
  std::vector<uint8_t> junk(img.size(), 0x11);
  g.rt.write_bytes(p, junk.data(), junk.size());
  CHECK(v && g.fake.bufs.at(v->b).bytes[100] == img[at + 100], "the guest's later writes do not reach the sound");
  g.rt.write_bytes(p, img.data(), img.size());
  // SND_NOSTOP while it plays: FALSE, the sound goes on.
  uint32_t first = g.fake.voices.begin()->first;
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0017)}) == 0 && g.fake.voices.count(first),
        "SND_NOSTOP while playing: FALSE");
  g.to(t0 + 500000 + 1000);
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0017)}) == 1 && !g.fake.voices.count(first) &&
            g.fake.live_voices() == 1,
        "SND_NOSTOP after the end: plays, on a new voice");
  // A new call replaces the sound; NULL stops it.
  uint32_t second = g.fake.voices.begin()->first;
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x000F)}) == 1 && !g.fake.voices.count(second) &&
            g.fake.only_voice() && g.fake.only_voice()->loop,
        "SND_LOOP: the new sound replaces the old one and loops");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(0), w16(0)}) == 1 && g.fake.voices.empty(), "NULL stops (TRUE)");
  // Undecodable and not RIFF: FALSE.
  std::vector<uint8_t> mp3fmt = {0x55, 0x00, 0x01, 0x00, 0x22, 0x56, 0, 0, 0x00, 0x04, 0, 0, 0, 0, 0x04, 0, 0, 0};
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.bytes(riff(mp3fmt, std::vector<uint8_t>(256, 0)))), w16(7)}) == 0 &&
            g.fake.voices.empty(),
        "an undecodable format: FALSE");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.bytes(std::vector<uint8_t>(64, 'x'))), w16(7)}) == 0,
        "not a RIFF image: FALSE");

  // MS-ADPCM (the Totally Twisted banks) and IMA-ADPCM: decoded into one PCM16 buffer.
  for (audio::WaveFormat f : {audio::ms_adpcm_format(11025, 1), audio::ima_adpcm_format(22050, 1)}) {
    std::vector<uint8_t> data(size_t(f.block_align) * 3);
    for (size_t i = 0; i < data.size(); i++) data[i] = uint8_t(i * 37 + 11);
    for (size_t blk = 0; blk < 3; blk++) {
      uint8_t* h = data.data() + blk * f.block_align;
      if (f.tag == audio::kTagMsAdpcm) {
        h[0] = uint8_t(blk % 7);  // predictor
        h[1] = 0x10, h[2] = 0x00;  // delta 16
      } else {
        h[2] = uint8_t(blk * 11);  // step index
        h[3] = 0;
      }
    }
    std::vector<uint8_t> expect = audio::decode(f, data);
    CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.bytes(riff(audio::waveformat_bytes(f), data))), w16(7)}) == 1,
          "tag %u plays", f.tag);
    const FakeEngine::Voice* av = g.fake.only_voice();
    CHECK(av && g.fake.bufs.at(av->b).f == audio::decoded_format(f) && g.fake.bufs.at(av->b).bytes == expect &&
              !expect.empty(),
          "tag %u: the buffer is audio::decode's PCM16 (%zu bytes)", f.tag, expect.size());
  }
  g.api("MMSYSTEM", "sndPlaySound", {l16(0), w16(0)});

  // Synchronous (NOCTURNE: SND_NODEFAULT|SND_MEMORY): the call lasts the sound,
  // in virtual time, charged at once without a yield hook ...
  uint64_t s0 = g.now();
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0006)}) == 1, "synchronous plays");
  const FakeEngine::Voice* sv = g.fake.only_voice();
  CHECK(sv && g.now() >= g.fake.voice_end(*sv) && g.now() - s0 < 500000 + 1000,
        "the call returned at the sound's end (%llu us after %llu)", (unsigned long long)(g.now() - s0),
        (unsigned long long)s0);
  // ... or frame by frame through the lane's yield hook (a long call).
  int yields = 0;
  g.rt.set_yield_hook([&] {
    yields++;
    g.clock.begin_frame();
    return true;
  });
  g.to(g.now() + 10000);
  uint64_t y0 = g.now();
  g.api("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0006)});
  CHECK(yields >= 499 && yields <= 501 && g.now() >= y0 + 500000, "the lane ended %d 1-ms frames inside the call",
        yields);
  g.rt.set_yield_hook(nullptr);

  // By name: a file (the WINDOWS directory, ".WAV" assumed), a WIN.INI [sounds] entry.
  g.rt.vfs().add_virtual_file("C:\\WINDOWS\\DING.WAV", pcm8(11025, 1103));
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.str("DING.WAV")), w16(1)}) == 1 &&
            g.fake.bufs.at(g.fake.only_voice()->b).bytes.size() == 1103,
        "a file name in the WINDOWS directory");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.str("C:\\WINDOWS\\DING")), w16(1)}) == 1, "a path, .WAV assumed");
  profiles16(g.rt).add_seed("C:\\WINDOWS\\WIN.INI", "sounds", "SystemAsterisk", "ding.wav,Asterisk");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.str("SystemAsterisk")), w16(1)}) == 1,
        "a WIN.INI [sounds] alias");
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.str("NOSUCH.WAV")), w16(1)}) == 0, "a missing sound: FALSE");
}

// ---- waveOut ---------------------------------------------------------------------------------------------

// A WAVEHDR at `hdr` over `bytes` bytes of data.
void make_header(Rig& g, uint32_t hdr, uint32_t data, uint32_t bytes) {
  std::vector<uint8_t> z(32, 0);
  g.rt.write_bytes(hdr, z.data(), z.size());
  g.rt.wr32(hdr, data);
  g.rt.wr32(hdr + 4, bytes);
}

void test_waveout() {
  Rig g;
  uint32_t mem = uint32_t(g.data(0x8000)) << 16;
  uint32_t fmt = mem, phwo = mem + 0x40, hdr1 = mem + 0x80, hdr2 = mem + 0xC0, mmt = mem + 0x100, data = mem + 0x200;
  auto put_fmt = [&](const audio::WaveFormat& f) {
    std::vector<uint8_t> b = audio::waveformat_bytes(f);
    g.rt.write_bytes(fmt, b.data(), b.size());
  };
  auto open = [&](uint16_t dev, uint32_t cb, uint32_t inst, uint32_t flags) {
    return g.api16("MMSYSTEM", "waveOutOpen", {l16(phwo), w16(dev), l16(fmt), l16(cb), l16(inst), l16(flags)});
  };
  // WAVE_FORMAT_QUERY: PCM anywhere; ADPCM on WAVE_MAPPER only.
  put_fmt(audio::pcm_format(22050, 1, 16));
  CHECK(open(0, 0, 0, 1) == 0, "PCM16 query on device 0");
  put_fmt(audio::ima_adpcm_format(22050, 1));
  CHECK(open(0, 0, 0, 1) == 32 && open(0xFFFF, 0, 0, 1) == 0, "IMA-ADPCM: device 0 refuses, the mapper accepts");
  put_fmt(audio::ms_adpcm_format(11025, 1));
  CHECK(open(0xFFFF, 0, 0, 1) == 0, "MS-ADPCM on the mapper");
  std::vector<uint8_t> mp3 = {0x55, 0x00, 0x01, 0x00, 0x22, 0x56, 0, 0, 0x00, 0x04, 0, 0, 0, 0, 0x04, 0, 0, 0};
  g.rt.write_bytes(fmt, mp3.data(), mp3.size());
  CHECK(open(0xFFFF, 0, 0, 1) == 32, "MPEG: WAVERR_BADFORMAT");
  CHECK(open(3, 0, 0, 1) == 2, "no device 3");
  CHECK(g.fake.streams.empty(), "queries open nothing");

  // A real stream: 22050 Hz 16-bit mono, CALLBACK_NULL.
  put_fmt(audio::pcm_format(22050, 1, 16));
  CHECK(open(0, 0, 0, 0) == 0, "open");
  uint16_t h = g.rt.rd16(phwo);
  CHECK(h >= 0xE000 && g.fake.streams.size() == 1, "a handle (%04X) and one engine stream", h);
  make_header(g, hdr1, data, 4410);         // 2205 frames: 100 ms
  make_header(g, hdr2, data + 4410, 4410);  // another 100 ms
  CHECK(g.api16("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr1), w16(32)}) == 34, "unprepared: WAVERR_UNPREPARED");
  for (uint32_t hd : {hdr1, hdr2}) g.api("MMSYSTEM", "waveOutPrepareHeader", {w16(h), l16(hd), w16(32)});
  CHECK(g.rt.rd32(hdr1 + 16) == 2, "WHDR_PREPARED");
  uint64_t t0 = g.now();
  CHECK(g.api16("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr1), w16(32)}) == 0 &&
            g.api16("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr2), w16(32)}) == 0,
        "two chunks written");
  CHECK((g.rt.rd32(hdr1 + 16) & 0x13) == 0x12, "INQUEUE|PREPARED, not DONE (%X)", g.rt.rd32(hdr1 + 16));
  CHECK(g.api16("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr1), w16(32)}) == 33, "a queued header: STILLPLAYING");
  CHECK(g.api16("MMSYSTEM", "waveOutUnprepareHeader", {w16(h), l16(hdr1), w16(32)}) == 33 &&
            g.api16("MMSYSTEM", "waveOutClose", {w16(h)}) == 33,
        "unprepare/close while queued: WAVERR_STILLPLAYING");
  // WHDR_DONE exactly when virtual time passes the chunk's end.
  g.to(t0 + 98000);
  g.tick();
  CHECK(!(g.rt.rd32(hdr1 + 16) & 1), "not done at 98 ms");
  g.to(t0 + 100000 + 1000);
  g.tick();
  CHECK((g.rt.rd32(hdr1 + 16) & 0x11) == 0x01 && (g.rt.rd32(hdr2 + 16) & 0x11) == 0x10,
        "the first done after 100 ms, the second still queued (%X %X)", g.rt.rd32(hdr1 + 16), g.rt.rd32(hdr2 + 16));
  g.rt.wr16(mmt, 4);
  CHECK(g.api16("MMSYSTEM", "waveOutGetPosition", {w16(h), l16(mmt), w16(8)}) == 0 && g.rt.rd16(mmt) == 4 &&
            g.rt.rd32(mmt + 2) >= 4410 && g.rt.rd32(mmt + 2) < 4410 + 200,
        "TIME_BYTES position (%u)", g.rt.rd32(mmt + 2));
  g.rt.wr16(mmt, 1);
  g.api("MMSYSTEM", "waveOutGetPosition", {w16(h), l16(mmt), w16(8)});
  CHECK(g.rt.rd16(mmt) == 1 && g.rt.rd32(mmt + 2) >= 100 && g.rt.rd32(mmt + 2) <= 102, "TIME_MS position (%u)",
        g.rt.rd32(mmt + 2));
  // Pause holds the queue; Reset completes everything at once.
  g.api("MMSYSTEM", "waveOutPause", {w16(h)});
  g.to(g.now() + 300000);
  g.tick();
  CHECK(g.rt.rd32(hdr2 + 16) & 0x10, "paused: the second chunk is still queued");
  CHECK(g.api16("MMSYSTEM", "waveOutReset", {w16(h)}) == 0 && (g.rt.rd32(hdr2 + 16) & 0x11) == 0x01,
        "reset: done when it returns");
  CHECK(g.api16("MMSYSTEM", "waveOutUnprepareHeader", {w16(h), l16(hdr2), w16(32)}) == 0 && g.rt.rd32(hdr2 + 16) == 1,
        "unprepared");
  CHECK(g.api16("MMSYSTEM", "waveOutClose", {w16(h)}) == 0 && g.fake.streams.empty(), "closed");
  CHECK(g.api16("MMSYSTEM", "waveOutClose", {w16(h)}) == 5, "a closed handle: MMSYSERR_INVALHANDLE");

  // ADPCM through the mapper: each chunk decoded as it is written.
  put_fmt(audio::ima_adpcm_format(22050, 1));
  CHECK(open(0xFFFF, 0, 0, 0) == 0, "an IMA-ADPCM stream on the mapper");
  uint16_t ha = g.rt.rd16(phwo);
  make_header(g, hdr1, data, 1024);  // two 512-byte blocks: 2034 frames
  g.api("MMSYSTEM", "waveOutPrepareHeader", {w16(ha), l16(hdr1), w16(32)});
  g.fake.calls.clear();
  g.api("MMSYSTEM", "waveOutWrite", {w16(ha), l16(hdr1), w16(32)});
  CHECK(!g.fake.calls.empty() && g.fake.calls.back() == "stream_write 4068", "the engine got PCM16 (%s)",
        g.fake.calls.empty() ? "" : g.fake.calls.back().c_str());
  g.api("MMSYSTEM", "waveOutReset", {w16(ha)});
  g.api("MMSYSTEM", "waveOutClose", {w16(ha)});
}

// MM_WOM_OPEN/DONE/CLOSE to a window: posted at their time, dispatched by the
// lane's pump unless the guest takes them itself.
void test_wom_window() {
  Rig g;
  uint16_t rec = g.data(512);
  uint16_t hwnd = make_window(g, rec);
  CHECK(hwnd != 0, "a window");
  uint32_t mem = uint32_t(g.data(0x4000)) << 16;
  uint32_t fmt = mem, phwo = mem + 0x40, hdr = mem + 0x80, msg = mem + 0xC0, data = mem + 0x200;
  std::vector<uint8_t> b = audio::waveformat_bytes(audio::pcm_format(11025, 1, 8));
  g.rt.write_bytes(fmt, b.data(), b.size());
  CHECK(g.api16("MMSYSTEM", "waveOutOpen", {l16(phwo), w16(0), l16(fmt), l16(hwnd), l16(0), l16(0x00010000)}) == 0,
        "CALLBACK_WINDOW open");
  uint16_t h = g.rt.rd16(phwo);
  audio16_pump(g.rt);
  make_header(g, hdr, data, 1103);  // 100 ms
  g.api("MMSYSTEM", "waveOutPrepareHeader", {w16(h), l16(hdr), w16(32)});
  uint64_t t0 = g.now();
  g.api("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr), w16(32)});
  audio16_pump(g.rt);
  CHECK(records_text(window_records(g, rec)) == "03BB/" + [&] {
          char s[8];
          snprintf(s, sizeof(s), "%X", h);
          return std::string(s);
        }() + "/0",
        "MM_WOM_OPEN dispatched by the pump: %s", records_text(window_records(g, rec)).c_str());
  g.to(t0 + 99000);
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).size() == 1, "nothing more before the chunk's end");
  g.to(t0 + 101000);
  audio16_pump(g.rt);
  std::vector<Rec> r = window_records(g, rec);
  CHECK(r.size() == 2 && r[1].msg == 0x3BD && r[1].wp == h && r[1].lp == hdr, "MM_WOM_DONE(hwo, lpWaveHdr) at 100 ms: %s",
        records_text(r).c_str());
  // The guest pumping its own queue takes the message there.
  g.api("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr), w16(32)});
  g.to(g.now() + 101000);
  g.tick();
  CHECK(g.api16("USER", "PeekMessage", {l16(msg), w16(hwnd), w16(0x3BB), w16(0x3BD), w16(1)}) == 1 &&
            g.rt.rd16(msg + 2) == 0x3BD && g.rt.rd32(msg + 6) == hdr,
        "PeekMessage(PM_REMOVE) takes MM_WOM_DONE");
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).size() == 2, "the pump does not dispatch it again");
  CHECK(g.api16("MMSYSTEM", "waveOutClose", {w16(h)}) == 0, "close");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 3 && r[2].msg == 0x3BC, "MM_WOM_CLOSE last: %s", records_text(r).c_str());
}

// CALLBACK_FUNCTION: called at the first API call at or after the event's
// time, in order, never re-entrantly.
void test_wom_function() {
  Rig g;
  uint16_t rec = g.data(512);
  uint32_t tgt = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "timeGetTime"));
  uint16_t cs = g.code(function_recorder(rec, tgt));
  uint32_t mem = uint32_t(g.data(0x4000)) << 16;
  uint32_t fmt = mem, phwo = mem + 0x40, hdr1 = mem + 0x80, hdr2 = mem + 0xC0, data = mem + 0x200;
  std::vector<uint8_t> b = audio::waveformat_bytes(audio::pcm_format(11025, 1, 8));
  g.rt.write_bytes(fmt, b.data(), b.size());
  auto recs = [&] {
    std::vector<std::array<uint16_t, 4>> out;
    uint32_t base = uint32_t(rec) << 16;
    for (uint16_t i = 0; i < g.rt.rd16(base); i++) {
      uint32_t e = base + 8 + 8u * i;
      out.push_back({g.rt.rd16(e), g.rt.rd16(e + 2), g.rt.rd16(e + 4), g.rt.rd16(e + 6)});
    }
    return out;
  };
  CHECK(g.api16("MMSYSTEM", "waveOutOpen",
                {l16(phwo), w16(0), l16(fmt), l16(uint32_t(cs) << 16), l16(0x1234), l16(0x00030000)}) == 0,
        "CALLBACK_FUNCTION open");
  uint16_t h = g.rt.rd16(phwo);
  CHECK(recs().empty(), "nothing is called inside waveOutOpen");
  make_header(g, hdr1, data, 551);  // 50 ms
  make_header(g, hdr2, data, 551);
  uint64_t t0 = g.now();
  for (uint32_t hd : {hdr1, hdr2}) {
    g.api("MMSYSTEM", "waveOutPrepareHeader", {w16(h), l16(hd), w16(32)});
    g.api("MMSYSTEM", "waveOutWrite", {w16(h), l16(hd), w16(32)});
  }
  auto r = recs();
  CHECK(r.size() == 1 && r[0][0] == 0x3BB && r[0][1] == h, "WOM_OPEN at the first API call after the open (%zu)",
        r.size());
  // Both chunks end while no API call happens: both DONEs at the next one, in order.
  g.to(t0 + 120000);
  CHECK(recs().size() == 1, "nothing without an API call");
  g.tick();
  r = recs();
  CHECK(r.size() == 3 && r[1][0] == 0x3BD && r[1][2] == uint16_t(hdr1) && r[2][0] == 0x3BD && r[2][2] == uint16_t(hdr2),
        "WOM_DONE for each header, in order (%zu)", r.size());
  bool flat = true;
  for (const auto& x : r) flat &= x[3] == 1;
  CHECK(flat, "never re-entered: the callback's own API call delivered nothing");
  // The instance and the DS: the callback ran with its module's (none here: the caller's).
  g.api("MMSYSTEM", "waveOutClose", {w16(h)});
  g.tick();
  r = recs();
  CHECK(r.size() == 4 && r[3][0] == 0x3BC, "WOM_CLOSE after the DONEs");
}

// ---- midiOut: the raw port ----------------------------------------------------------------------------------

void make_midi_header(Rig& g, uint32_t hdr, uint32_t data, uint32_t bytes) {
  std::vector<uint8_t> z(28, 0);
  g.rt.write_bytes(hdr, z.data(), z.size());
  g.rt.wr32(hdr, data);
  g.rt.wr32(hdr + 4, bytes);
}

std::vector<const FakeEngine::Midi*> midi_calls(const FakeEngine& f, const char* what) {
  std::vector<const FakeEngine::Midi*> v;
  for (const FakeEngine::Midi& m : f.midi) {
    if (m.what == what) v.push_back(&m);
  }
  return v;
}

void test_midi_out() {
  Rig g(true, true, nullptr, 0);
  uint16_t rec = g.data(512);
  uint16_t hwnd = make_window(g, rec);
  uint32_t mem = uint32_t(g.data(0x1000)) << 16;
  uint32_t phmo = mem, idp = mem + 0x10, hdr = mem + 0x40, data = mem + 0x100;
  auto open = [&](uint32_t p, uint16_t dev, uint32_t cb, uint32_t flags) {
    return g.api16("MMSYSTEM", "midiOutOpen", {l16(p), w16(dev), l16(cb), l16(0), l16(flags)});
  };
  // Refusals, each leaving the handle zeroed.
  g.rt.wr16(phmo, 0x1111);
  CHECK(open(phmo, 1, 0, 0) == 2 && g.rt.rd16(phmo) == 0, "device 1: MMSYSERR_BADDEVICEID");
  CHECK(open(phmo, 0xFFFF, 0, 0x00040000) == 10, "an unknown callback type: MMSYSERR_INVALFLAG");
  CHECK(open(0, 0xFFFF, 0, 0) == 11, "no handle pointer: MMSYSERR_INVALPARAM");
  CHECK(open(phmo, 0xFFFF, 0x1234, 0x00010000) == 11, "CALLBACK_WINDOW to no window: MMSYSERR_INVALPARAM");
  CHECK(open(phmo, 0xFFFF, 0, 0x00030000) == 11, "CALLBACK_FUNCTION NULL: MMSYSERR_INVALPARAM");
  // SWSE's open is the mapper's with CALLBACK_NULL; here a window callback, to see MM_MOM_*.
  CHECK(open(phmo, 0xFFFF, hwnd, 0x00010000) == 0, "midiOutOpen(MIDI_MAPPER, CALLBACK_WINDOW)");
  uint16_t h = g.rt.rd16(phmo);
  CHECK(h >= 0xE000 && (h & 3) == 0, "a handle (%04X)", h);
  g.rt.wr16(mem + 2, 0x2222);
  CHECK(open(mem + 2, 0, 0, 0) == 4 && g.rt.rd16(mem + 2) == 0, "one client at a time: MMSYSERR_ALLOCATED");
  CHECK(g.api16("MMSYSTEM", "midiOutGetID", {w16(h), l16(idp)}) == 0 && g.rt.rd16(idp) == 0xFFFF,
        "midiOutGetID: MIDI_MAPPER, as opened");
  CHECK(g.api16("MMSYSTEM", "midiOutGetID", {w16(h), l16(0)}) == 11, "midiOutGetID(NULL): MMSYSERR_INVALPARAM");
  audio16_pump(g.rt);
  std::vector<Rec> r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].msg == 0x3C7 && r[0].wp == h && r[0].lp == 0, "MM_MOM_OPEN(hmo, 0): %s",
        records_text(r).c_str());
  // Short messages reach the engine as they are, dated now.
  g.to(5000);
  CHECK(g.api16("MMSYSTEM", "midiOutShortMsg", {w16(h), l16(0x00643C90)}) == 0, "midiOutShortMsg");
  CHECK(g.api16("MMSYSTEM", "midiOutShortMsg", {w16(h), l16(0x0000503E)}) == 0, "running status goes through");
  auto sh = midi_calls(g.fake, "short");
  CHECK(sh.size() == 2 && sh[0]->msg == 0x00643C90 && sh[0]->asked == 5000 && sh[1]->msg == 0x0000503E,
        "the engine got both, at 5 ms (%zu)", sh.size());
  // Long messages: the header must be prepared; the buffer is taken at once.
  const std::vector<uint8_t> sysex = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
  g.rt.write_bytes(data, sysex.data(), sysex.size());
  make_midi_header(g, hdr, data, uint32_t(sysex.size()));
  CHECK(g.api16("MMSYSTEM", "midiOutLongMsg", {w16(h), l16(hdr), w16(28)}) == 64, "unprepared: MIDIERR_UNPREPARED");
  CHECK(g.api16("MMSYSTEM", "midiOutPrepareHeader", {w16(h), l16(hdr), w16(27)}) == 11,
        "a MIDIHDR shorter than 28 bytes: MMSYSERR_INVALPARAM");
  g.rt.wr32(hdr, 0);
  CHECK(g.api16("MMSYSTEM", "midiOutPrepareHeader", {w16(h), l16(hdr), w16(28)}) == 11, "no data: MMSYSERR_INVALPARAM");
  g.rt.wr32(hdr, data);
  CHECK(g.api16("MMSYSTEM", "midiOutPrepareHeader", {w16(h), l16(hdr), w16(28)}) == 0 &&
            g.rt.rd32(hdr + 16) == 0x02,
        "prepared: MHDR_PREPARED");
  g.to(6000);
  CHECK(g.api16("MMSYSTEM", "midiOutLongMsg", {w16(h), l16(hdr), w16(28)}) == 0 && g.rt.rd32(hdr + 16) == 0x03,
        "sent: MHDR_DONE when the call returns (flags %X)", g.rt.rd32(hdr + 16));
  auto lg = midi_calls(g.fake, "long");
  CHECK(lg.size() == 1 && lg[0]->bytes == sysex && lg[0]->asked == 6000, "the engine got the SysEx at 6 ms");
  g.rt.wr8(data, 0x00);
  CHECK(lg.size() == 1 && lg[0]->bytes[0] == 0xF0, "copied at the call");
  CHECK(window_records(g, rec).size() == 1, "MM_MOM_DONE waits for a delivery point");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 2 && r[1].msg == 0x3C9 && r[1].wp == h && r[1].lp == hdr, "MM_MOM_DONE(hmo, lpMidiHdr): %s",
        records_text(r).c_str());
  CHECK(g.api16("MMSYSTEM", "midiOutUnprepareHeader", {w16(h), l16(hdr), w16(28)}) == 0 && g.rt.rd32(hdr + 16) == 0x01,
        "unprepared again");
  g.rt.wr32(hdr + 16, 0x06);
  CHECK(g.api16("MMSYSTEM", "midiOutUnprepareHeader", {w16(h), l16(hdr), w16(28)}) == 65 &&
            g.api16("MMSYSTEM", "midiOutLongMsg", {w16(h), l16(hdr), w16(28)}) == 65,
        "MHDR_INQUEUE: MIDIERR_STILLPLAYING");
  // No patch caching (the caps have no MIDICAPS_CACHE), no driver messages.
  CHECK(g.api16("MMSYSTEM", "midiOutCachePatches", {w16(h), w16(0), l16(data), w16(2)}) == 8 &&
            g.api16("MMSYSTEM", "midiOutCacheDrumPatches", {w16(h), w16(0), l16(data), w16(2)}) == 8,
        "patch caching: MMSYSERR_NOTSUPPORTED");
  CHECK((g.api("MMSYSTEM", "midiOutMessage", {w16(h), w16(0x800), l16(0), l16(0)}) & 0xFFFF) == 8,
        "midiOutMessage: MMSYSERR_NOTSUPPORTED");
  // Reset, close.
  g.to(7000);
  CHECK(g.api16("MMSYSTEM", "midiOutReset", {w16(h)}) == 0, "midiOutReset");
  auto rs = midi_calls(g.fake, "reset");
  CHECK(rs.size() == 1 && rs[0]->asked == 7000, "the engine resets at 7 ms");
  CHECK(g.api16("MMSYSTEM", "midiOutClose", {w16(h)}) == 0, "midiOutClose");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 3 && r[2].msg == 0x3C8 && r[2].wp == h, "MM_MOM_CLOSE last: %s", records_text(r).c_str());
  CHECK(g.api16("MMSYSTEM", "midiOutClose", {w16(h)}) == 5 &&
            g.api16("MMSYSTEM", "midiOutShortMsg", {w16(h), l16(0x90)}) == 5 &&
            g.api16("MMSYSTEM", "midiOutCachePatches", {w16(h), w16(0), l16(data), w16(2)}) == 5,
        "a closed handle: MMSYSERR_INVALHANDLE");
  // Free again: device 0 with no callback.
  CHECK(open(phmo, 0, 0, 0) == 0 && g.api16("MMSYSTEM", "midiOutGetID", {w16(g.rt.rd16(phmo)), l16(idp)}) == 0 &&
            g.rt.rd16(idp) == 0,
        "reopened as device 0");
  CHECK(g.fake.midi.size() == 4, "nothing else reached the engine (%zu)", g.fake.midi.size());
}

// A DriverCallback(h, msg, dwInstance, dwParam1, dwParam2), FAR PASCAL, that
// answers every `msg` notification by sending its buffer again,
// fn(h, dwParam1, size) — midiOutLongMsg(hmo, lpMidiHdr, 28) from
// MM_MOM_DONE, waveOutWrite(hwo, lpWaveHdr, 32) from MM_WOM_DONE: the usual
// Win16 way to stream buffers. DATA:0 counts its calls; past `limit` it stops
// answering, so a runaway chain ends there instead of hanging the test.
std::vector<uint8_t> resender(uint16_t data, uint32_t fn, uint16_t msg, uint8_t size, uint16_t limit) {
  std::vector<uint8_t> v = {0x55, 0x8B, 0xEC, 0x06,                              // push bp; mov bp,sp; push es
                            0xB8, uint8_t(data), uint8_t(data >> 8), 0x8E, 0xC0,  // mov ax,DATA; mov es,ax
                            0x26, 0xFF, 0x06, 0x00, 0x00,                        // inc word es:[0]
                            0x26, 0x81, 0x3E, 0x00, 0x00, uint8_t(limit), uint8_t(limit >> 8),  // cmp word es:[0],limit
                            0x77, 0x00,                                          // ja done
                            0x81, 0x7E, 0x12, uint8_t(msg), uint8_t(msg >> 8),   // cmp word [bp+12h],msg
                            0x75, 0x00,                                          // jne done
                            0xFF, 0x76, 0x14,                                    // push h
                            0xFF, 0x76, 0x0C, 0xFF, 0x76, 0x0A,                  // push dwParam1 (the header)
                            0x6A, size,                                          // push size
                            0x9A, uint8_t(fn), uint8_t(fn >> 8), uint8_t(fn >> 16), uint8_t(fn >> 24)};
  const size_t ja = 22, jne = 29;
  v[ja] = uint8_t(v.size() - (ja + 1));
  v[jne] = uint8_t(v.size() - (jne + 1));
  append(v, {0x07, 0x5D, 0xCA, 0x10, 0x00});  // done: pop es; pop bp; retf 10h
  return v;
}

// Guest work with no API call: `outer` × 65535 LOOP iterations (15: about
// 10 ms at 100 instructions a µs).
std::vector<uint8_t> busy_work(uint8_t outer) {
  return {0xB3, outer,                // mov bl,outer
          0xB9, 0xFF, 0xFF,           // again: mov cx,0FFFFh
          0xE2, 0xFE,                 // loop $
          0xFE, 0xCB, 0x75, 0xF7,     // dec bl; jnz again
          0xCB};                      // retf
}

// A procedure that answers each notification with a request whose own
// notification is due at once takes one step per delivery point: what it
// causes waits for a later one, whatever its date, so a delivery ends even
// when virtual time stands still inside it (ADMIPS=0) or the delivery point
// comes more than the 1 s cap on uncharged instructions late.
void test_resend_chain() {
  for (uint32_t ips : {0u, 100u}) {
    // midiOutLongMsg from MM_MOM_DONE.
    Rig g(true, true, nullptr, ips);
    uint16_t cnt = g.data(64);
    auto calls = [&] { return int(g.rt.rd16(uint32_t(cnt) << 16)); };
    uint32_t lm = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "midiOutLongMsg"));
    uint16_t cs = g.code(resender(cnt, lm, 0x3C9, 28, 100));
    uint32_t mem = uint32_t(g.data(0x400)) << 16;
    uint32_t phmo = mem, hdr = mem + 0x40, data = mem + 0x100;
    CHECK(g.api16("MMSYSTEM", "midiOutOpen", {l16(phmo), w16(0xFFFF), l16(uint32_t(cs) << 16), l16(0), l16(0x00030000)}) ==
              0,
          "ips %u: midiOutOpen(CALLBACK_FUNCTION)", ips);
    uint16_t h = g.rt.rd16(phmo);
    g.rt.wr8(data, 0x90), g.rt.wr8(data + 1, 60), g.rt.wr8(data + 2, 64);
    make_midi_header(g, hdr, data, 3);
    g.api("MMSYSTEM", "midiOutPrepareHeader", {w16(h), l16(hdr), w16(28)});  // MM_MOM_OPEN called here
    g.to(10000);
    g.api("MMSYSTEM", "midiOutLongMsg", {w16(h), l16(hdr), w16(28)});
    const int c0 = calls();
    g.to(10000 + 1100000);  // 1.1 s late, at the lane's pump
    const uint64_t t1 = g.now();
    audio16_pump(g.rt);
    CHECK(calls() == c0 + 1, "ips %u: one MM_MOM_DONE call at a delivery point 1.1 s late; the answer's waits (%d)",
          ips, calls() - c0);
    g.tick();
    CHECK(calls() == c0 + 1 + (ips ? 1 : 0), "ips %u: the next API call is a later delivery point only if time moved (%d)",
          ips, calls() - c0);
    for (int k = 0; k < 3; k++) {
      g.to(g.now() + 1000);
      g.tick();
    }
    CHECK(calls() == c0 + 4 + (ips ? 1 : 0), "ips %u: one step per delivery point (%d)", ips, calls() - c0);
    // What each step sends is dated at its own call: the first answer at the
    // MM_MOM_DONE's time, the next ones at the delivery points after it.
    auto lg = midi_calls(g.fake, "long");
    CHECK(lg.size() >= 3 && lg[1]->asked >= 10000 && lg[1]->asked < 10100 && lg[2]->asked > t1,
          "ips %u: dated %llu, then %llu", ips, lg.size() >= 3 ? (unsigned long long)lg[1]->asked : 0ull,
          lg.size() >= 3 ? (unsigned long long)lg[2]->asked : 0ull);
  }
  for (uint32_t ips : {0u, 100u}) {
    // The same, empty, WAVEHDR written again from MM_WOM_DONE: the stream
    // finishes it at once (the real engine).
    audio::Config cfg;
    cfg.guest_sound = true;
    std::unique_ptr<audio::Engine> engine = audio::make_engine(cfg);
    Rig g(true, true, engine.get(), ips);
    uint16_t cnt = g.data(64);
    auto calls = [&] { return int(g.rt.rd16(uint32_t(cnt) << 16)); };
    uint32_t wr = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "waveOutWrite"));
    uint16_t cs = g.code(resender(cnt, wr, 0x3BD, 32, 100));
    uint32_t mem = uint32_t(g.data(0x400)) << 16;
    uint32_t phwo = mem, fmt = mem + 0x20, hdr = mem + 0x40, data = mem + 0x100;
    std::vector<uint8_t> f = audio::waveformat_bytes(audio::pcm_format(11025, 1, 8));
    g.rt.write_bytes(fmt, f.data(), f.size());
    CHECK(g.api16("MMSYSTEM", "waveOutOpen",
                  {l16(phwo), w16(0), l16(fmt), l16(uint32_t(cs) << 16), l16(0), l16(0x00030000)}) == 0,
          "ips %u: waveOutOpen(CALLBACK_FUNCTION), real engine", ips);
    uint16_t h = g.rt.rd16(phwo);
    make_header(g, hdr, data, 0);  // an empty buffer
    g.api("MMSYSTEM", "waveOutPrepareHeader", {w16(h), l16(hdr), w16(32)});  // MM_WOM_OPEN called here
    g.to(10000);
    g.api("MMSYSTEM", "waveOutWrite", {w16(h), l16(hdr), w16(32)});
    const int c0 = calls();
    g.to(10000 + 1100000);
    audio16_pump(g.rt);
    CHECK(calls() == c0 + 1, "ips %u: one MM_WOM_DONE call at a late delivery point (%d)", ips, calls() - c0);
    for (int k = 0; k < 3; k++) {
      g.to(g.now() + 1000);
      audio16_pump(g.rt);
    }
    CHECK(calls() == c0 + 4, "ips %u: then one per delivery point (%d)", ips, calls() - c0);
    CHECK((g.rt.rd32(hdr + 16) & 0x11) == 0x01, "ips %u: the header written last is done already (%X)", ips,
          g.rt.rd32(hdr + 16));
    engine->shutdown(g.now());
  }
}

// MM_MOM_* to a CALLBACK_FUNCTION: at the first API call at or after them, in order.
void test_midi_function() {
  Rig g;
  uint16_t rec = g.data(512);
  uint32_t tgt = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "timeGetTime"));
  uint16_t cs = g.code(function_recorder(rec, tgt));
  uint32_t mem = uint32_t(g.data(0x400)) << 16;
  uint32_t phmo = mem, hdr = mem + 0x40, data = mem + 0x100;
  auto recs = [&] {
    std::vector<std::array<uint16_t, 4>> out;
    uint32_t base = uint32_t(rec) << 16;
    for (uint16_t i = 0; i < g.rt.rd16(base); i++) {
      uint32_t e = base + 8 + 8u * i;
      out.push_back({g.rt.rd16(e), g.rt.rd16(e + 2), g.rt.rd16(e + 4), g.rt.rd16(e + 6)});
    }
    return out;
  };
  CHECK(g.api16("MMSYSTEM", "midiOutOpen", {l16(phmo), w16(0xFFFF), l16(uint32_t(cs) << 16), l16(0x55), l16(0x00030000)}) ==
            0,
        "CALLBACK_FUNCTION open");
  uint16_t h = g.rt.rd16(phmo);
  CHECK(recs().empty(), "nothing is called inside midiOutOpen");
  g.rt.wr8(data, 0xF8);
  make_midi_header(g, hdr, data, 1);
  g.api("MMSYSTEM", "midiOutPrepareHeader", {w16(h), l16(hdr), w16(28)});
  g.api("MMSYSTEM", "midiOutLongMsg", {w16(h), l16(hdr), w16(28)});
  g.api("MMSYSTEM", "midiOutClose", {w16(h)});
  auto r = recs();
  // MOM_OPEN came due at the open and went at the next call (PrepareHeader),
  // MOM_DONE at the call after LongMsg (Close); MOM_CLOSE waits for the next.
  CHECK(r.size() == 2 && r[0][0] == 0x3C7 && r[0][1] == h && r[1][0] == 0x3C9 && r[1][2] == uint16_t(hdr),
        "MOM_OPEN, then MOM_DONE(lpMidiHdr), each at the first API call after it (%zu)", r.size());
  g.tick();
  r = recs();
  CHECK(r.size() == 3 && r[2][0] == 0x3C8, "then MOM_CLOSE (%zu)", r.size());
  bool flat = true;
  for (const auto& x : r) flat &= x[3] == 1;
  CHECK(flat, "never re-entered");
}

// ---- multimedia timer events --------------------------------------------------------------------------------

// A TimeProc(wID, wMsg, dwUser, dw1, dw2), FAR PASCAL — MEMMIDI's
// MIDITIMERPROC shape (retf 10h). DATA:2 counts the nesting depth; entries
// from DATA:8 hold wID, wMsg, dwUser's low word and the depth at entry. With
// `midi` it sends midiOutShortMsg(DATA:4, 0x00403C90) — an API call, so a
// delivery point where nothing may be delivered re-entrantly; with `rearm`
// it sets itself again, once, DATA:6 ms on (timeSetEvent, TIME_ONESHOT).
std::vector<uint8_t> timeproc_recorder(uint16_t data, uint32_t midi = 0, uint32_t rearm = 0) {
  const uint8_t dl = uint8_t(data), dh = uint8_t(data >> 8);
  auto far_call = [](std::vector<uint8_t>& v, uint32_t fp) {
    append(v, {0x9A, uint8_t(fp), uint8_t(fp >> 8), uint8_t(fp >> 16), uint8_t(fp >> 24)});
  };
  std::vector<uint8_t> v = {0x55, 0x8B, 0xEC, 0x06, 0x53, 0xB8, dl, dh, 0x8E, 0xC0,  // push bp.. mov es,DATA
                            0x26, 0xFF, 0x06, 0x02, 0x00,                         // inc word es:[2] (depth)
                            0x26, 0x8B, 0x1E, 0x00, 0x00,                         // mov bx,es:[0]
                            0xC1, 0xE3, 0x03, 0x83, 0xC3, 0x08,                   // shl bx,3; add bx,8
                            0x8B, 0x46, 0x14, 0x26, 0x89, 0x07,                   // wID
                            0x8B, 0x46, 0x12, 0x26, 0x89, 0x47, 0x02,             // wMsg
                            0x8B, 0x46, 0x0E, 0x26, 0x89, 0x47, 0x04,             // dwUser lo
                            0x26, 0xA1, 0x02, 0x00, 0x26, 0x89, 0x47, 0x06,       // depth
                            0x26, 0xFF, 0x06, 0x00, 0x00};                        // inc word es:[0]
  if (midi) {
    append(v, {0x26, 0xFF, 0x36, 0x04, 0x00,  // push word es:[4] (hmo)
               0x68, 0x40, 0x00,              // push 0040h
               0x68, 0x90, 0x3C});            // push 3C90h: note on, key 60, velocity 64
    far_call(v, midi);
    append(v, {0xB8, dl, dh, 0x8E, 0xC0});
  }
  if (rearm) {
    append(v, {0x26, 0xFF, 0x36, 0x06, 0x00,  // push word es:[6] (wDelay)
               0x6A, 0x00,                    // wResolution
               0x0E, 0x6A, 0x00,              // lpFunction = CS:0000 (this procedure)
               0xFF, 0x76, 0x10, 0xFF, 0x76, 0x0E,  // dwUser
               0x6A, 0x00});                  // TIME_ONESHOT
    far_call(v, rearm);
    append(v, {0xB8, dl, dh, 0x8E, 0xC0});
  }
  append(v, {0x26, 0xFF, 0x0E, 0x02, 0x00,  // dec word es:[2]
             0x5B, 0x07, 0x5D, 0xCA, 0x10, 0x00});
  return v;
}

struct TimerRec {
  uint16_t id, msg, user, depth;
};
std::vector<TimerRec> timer_records(Rig& g, uint16_t data) {
  std::vector<TimerRec> out;
  uint32_t base = uint32_t(data) << 16;
  for (uint16_t i = 0; i < g.rt.rd16(base); i++) {
    uint32_t e = base + 8 + 8u * i;
    out.push_back({g.rt.rd16(e), g.rt.rd16(e + 2), g.rt.rd16(e + 4), g.rt.rd16(e + 6)});
  }
  return out;
}

uint16_t set_timer(Rig& g, uint16_t delay, uint16_t cs, uint32_t user, bool periodic) {
  return g.api16("MMSYSTEM", "timeSetEvent", {w16(delay), w16(delay), l16(uint32_t(cs) << 16), l16(user), w16(periodic)});
}

void test_timers() {
  // Exact times: no modeled instruction time, so peek_us() is the 1 ms frame grid.
  Rig g(true, true, nullptr, 0);
  uint16_t rec = g.data(4096);
  uint32_t midi = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "midiOutShortMsg"));
  uint16_t cs = g.code(timeproc_recorder(rec, midi));
  uint32_t mem = uint32_t(g.data(0x100)) << 16;
  CHECK(g.api16("MMSYSTEM", "midiOutOpen", {l16(mem), w16(0xFFFF), l16(0), l16(0), l16(0)}) == 0, "SWSE's midiOutOpen");
  g.rt.wr16((uint32_t(rec) << 16) + 4, g.rt.rd16(mem));
  // The timer's caps and periods.
  CHECK(g.api16("MMSYSTEM", "timeGetDevCaps", {l16(mem + 8), w16(4)}) == 0 && g.rt.rd16(mem + 8) == 1 &&
            g.rt.rd16(mem + 10) == 0xFFFF,
        "timeGetDevCaps: 1 .. 65535 ms");
  CHECK(g.api16("MMSYSTEM", "timeGetDevCaps", {l16(mem + 8), w16(2)}) == 129 &&
            g.api16("MMSYSTEM", "timeGetDevCaps", {l16(0), w16(4)}) == 129,
        "a short TIMECAPS or none: TIMERR_STRUCT");
  CHECK(g.api16("MMSYSTEM", "timeBeginPeriod", {w16(4)}) == 0 && g.api16("MMSYSTEM", "timeEndPeriod", {w16(4)}) == 0 &&
            g.api16("MMSYSTEM", "timeBeginPeriod", {w16(0)}) == 97 && g.api16("MMSYSTEM", "timeEndPeriod", {w16(0)}) == 97,
        "timeBeginPeriod/timeEndPeriod: TIMERR_NOERROR, 0 ms TIMERR_NOCANDO");
  CHECK(set_timer(g, 0, cs, 0, true) == 0 &&
            g.api16("MMSYSTEM", "timeSetEvent", {w16(4), w16(4), l16(0), l16(0), w16(1)}) == 0,
        "no delay or no procedure: no event");
  // MEMMIDI's: timeSetEvent(4, 4, MIDITIMERPROC, song, TIME_PERIODIC).
  g.to(10000);
  const uint64_t t0 = g.now();
  uint16_t id = set_timer(g, 4, cs, 0x12345678, true);
  CHECK(id != 0, "a periodic event (%u)", id);
  g.to(t0 + 3000);
  g.tick();
  CHECK(timer_records(g, rec).empty(), "nothing before its first period");
  // A delivery point every millisecond: one call per period, 4 ms apart.
  for (uint64_t ms = 4; ms <= 40; ms++) {
    g.to(t0 + ms * 1000);
    g.tick();
  }
  auto r = timer_records(g, rec);
  bool shape = r.size() == 10;
  for (const TimerRec& x : r) shape &= x.id == id && x.msg == 0 && x.user == 0x5678 && x.depth == 1;
  CHECK(shape, "TimeProc(wID, 0, dwUser, 0, 0) ten times in 40 ms, never nested (%zu)", r.size());
  auto sh = midi_calls(g.fake, "short");
  bool dated = sh.size() == 10;
  for (size_t k = 0; k < sh.size() && dated; k++) dated = sh[k]->asked == t0 + 4000 * (k + 1) && sh[k]->msg == 0x00403C90;
  CHECK(dated, "the MIDI it sent is dated at its periods (%zu)", sh.size());
  // No delivery point for 12 ms, then one: three calls there, in order, their
  // MIDI still dated on the 4 ms grid.
  g.to(t0 + 52000);
  g.tick();
  r = timer_records(g, rec);
  sh = midi_calls(g.fake, "short");
  CHECK(r.size() == 13 && sh.size() == 13 && sh[10]->asked == t0 + 44000 && sh[11]->asked == t0 + 48000 &&
            sh[12]->asked == t0 + 52000,
        "missed periods caught up at the next delivery point (%zu)", r.size());
  // The lane's pump is a delivery point too.
  g.to(t0 + 56000);
  audio16_pump(g.rt);
  CHECK(timer_records(g, rec).size() == 14, "delivered by the pump");
  // 2 s without a delivery point: 62 periods (250 ms of them) caught up, the
  // 438 before them dropped; the event keeps its grid.
  g.to(t0 + 2056000);
  g.tick();
  r = timer_records(g, rec);
  sh = midi_calls(g.fake, "short");
  CHECK(r.size() == 14 + 62 && sh.size() == r.size() && sh[14]->asked == t0 + 1812000 && sh.back()->asked == t0 + 2056000,
        "a long stall catches up 250 ms of periods (%zu, first at %llu)", r.size(),
        (unsigned long long)(sh.size() > 14 ? sh[14]->asked - t0 : 0));
  Timer16Stats st = timer16_stats(g.rt);
  CHECK(st.active == 1 && st.calls == 76 && st.dropped == 438, "stats: %zu active, %llu calls, %llu dropped", st.active,
        (unsigned long long)st.calls, (unsigned long long)st.dropped);
  // Killed: no more calls; a second kill finds nothing.
  CHECK(g.api16("MMSYSTEM", "timeKillEvent", {w16(id)}) == 0 && g.api16("MMSYSTEM", "timeKillEvent", {w16(id)}) == 97,
        "timeKillEvent: TIMERR_NOERROR, then TIMERR_NOCANDO");
  g.to(t0 + 2100000);
  g.tick();
  CHECK(timer_records(g, rec).size() == 76, "no call after the kill");
  // TIME_ONESHOT: one call, then the event is gone.
  clear_records(g, rec);
  uint64_t t1 = g.now();
  uint16_t one = set_timer(g, 10, cs, 0xABCD, false);
  CHECK(one != 0 && one != id, "a one-shot event (%u)", one);
  g.to(t1 + 9000);
  g.tick();
  CHECK(timer_records(g, rec).empty(), "not before 10 ms");
  g.to(t1 + 30000);
  g.tick();
  r = timer_records(g, rec);
  CHECK(r.size() == 1 && r[0].id == one && r[0].user == 0xABCD, "one call (%zu)", r.size());
  CHECK(g.api16("MMSYSTEM", "timeKillEvent", {w16(one)}) == 97, "a one-shot event is gone once called");
  // 16 events at most.
  std::vector<uint16_t> ids;
  for (int i = 0; i < 17; i++) ids.push_back(set_timer(g, 1000, cs, 0, true));
  CHECK(std::count(ids.begin(), ids.end(), uint16_t(0)) == 1 && ids[16] == 0, "16 events, the 17th refused");
  for (uint16_t x : ids) g.api("MMSYSTEM", "timeKillEvent", {w16(x)});
  CHECK(timer16_stats(g.rt).active == 0, "all killed");
}

// A one-shot event set again by its own procedure keeps its schedule however
// late the delivery points come: the procedure's calls are dated from its due time.
void test_timer_chain() {
  Rig g(true, true, nullptr, 0);
  uint16_t rec = g.data(1024);
  uint32_t set = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "timeSetEvent"));
  uint16_t cs = g.code(timeproc_recorder(rec, 0, set));
  g.rt.wr16((uint32_t(rec) << 16) + 6, 5);  // 5 ms
  g.to(1000);
  uint64_t t0 = g.now();
  CHECK(set_timer(g, 5, cs, 0x2222, false) != 0, "a one-shot event");
  g.to(t0 + 17000);
  g.tick();
  auto r = timer_records(g, rec);
  CHECK(r.size() == 3, "due at 5, 10 and 15 ms: three calls at 17 ms (%zu)", r.size());
  g.to(t0 + 19000);
  g.tick();
  CHECK(timer_records(g, rec).size() == 3, "the next is due at 20 ms, not 22");
  g.to(t0 + 20000);
  g.tick();
  r = timer_records(g, rec);
  bool ok = r.size() == 4;
  for (const TimerRec& x : r) ok &= x.user == 0x2222 && x.depth == 1;
  CHECK(ok, "the fourth at 20 ms (%zu)", r.size());
  CHECK(timer16_stats(g.rt).active == 1, "one event set at a time");
  // After a long stall such a chain is bounded like a periodic event: a 1 ms
  // chain 2 s behind catches up 250 ms of calls, not 2000.
  Rig h(true, true, nullptr, 0);
  uint16_t rec2 = h.data(4096);
  uint32_t set2 = h.rt.thunk_far(*h.rt.shims().find_name("MMSYSTEM", "timeSetEvent"));
  uint16_t cs2 = h.code(timeproc_recorder(rec2, 0, set2));
  h.rt.wr16((uint32_t(rec2) << 16) + 6, 1);  // 1 ms
  h.to(1000);
  uint64_t t1 = h.now();
  set_timer(h, 1, cs2, 0x3333, false);
  h.to(t1 + 2000000);
  h.tick();
  size_t n = timer_records(h, rec2).size();
  CHECK(n >= 250 && n <= 252, "a 1 ms chain 2 s behind: %zu calls", n);
}

// Timer events are the clock's, not sound's: without an enabled engine they
// still run (sound-off answers otherwise unchanged).
void test_timers_without_engine() {
  Rig g(false, true, nullptr, 0);
  uint16_t rec = g.data(512);
  uint16_t cs = g.code(timeproc_recorder(rec));
  CHECK(!audio16_enabled(g.rt) && g.rt.audio_due() == UINT64_MAX, "sound off, nothing due");
  audio16_pump(g.rt);
  CHECK(g.api16("MMSYSTEM", "midiOutGetNumDevs", {}) == 0, "still no MIDI device");
  g.to(2000);
  uint64_t t0 = g.now();
  uint16_t id = set_timer(g, 4, cs, 0x0707, true);
  CHECK(id != 0, "timeSetEvent without an engine");
  g.to(t0 + 8000);
  audio16_pump(g.rt);
  auto r = timer_records(g, rec);
  CHECK(r.size() == 2 && r[0].user == 0x0707 && r[1].id == id, "two periods, delivered by the pump (%zu)", r.size());
  g.to(t0 + 12000);
  g.tick();
  CHECK(timer_records(g, rec).size() == 3, "and at an API call");
  CHECK(g.fake.midi.empty() && g.fake.calls.empty(), "the disabled engine is never called");
  audio16_close(g.rt);
  g.to(t0 + 40000);
  g.tick();
  CHECK(timer_records(g, rec).size() == 3 && timer16_stats(g.rt).active == 0, "the run's end kills the events");
}

// The same script twice: the same calls, at the same virtual times.
void test_timer_determinism() {
  auto run = [] {
    Rig g;  // with modeled instruction time
    uint16_t rec = g.data(4096);
    uint32_t midi = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "midiOutShortMsg"));
    uint16_t cs = g.code(timeproc_recorder(rec, midi));
    uint32_t mem = uint32_t(g.data(0x40)) << 16;
    g.api("MMSYSTEM", "midiOutOpen", {l16(mem), w16(0xFFFF), l16(0), l16(0), l16(0)});
    g.rt.wr16((uint32_t(rec) << 16) + 4, g.rt.rd16(mem));
    set_timer(g, 4, cs, 1, true);
    for (int i = 1; i <= 300; i++) {
      g.to(uint64_t(i) * 1700);
      if (i % 3) g.tick();
      else audio16_pump(g.rt);
    }
    std::vector<uint64_t> out;
    for (const auto& m : g.fake.midi) out.push_back(m.asked);
    return out;
  };
  std::vector<uint64_t> a = run(), b = run();
  CHECK(!a.empty() && a == b, "identical runs (%zu calls)", a.size());
  // With instructions modeled each call is dated a few µs after its period
  // (the procedure's own work before it calls, rounded to the µs): 4 ms apart.
  bool grid = a.size() > 100;
  for (size_t k = 1; k < a.size() && grid; k++) grid = a[k] - a[k - 1] >= 3998 && a[k] - a[k - 1] <= 4002;
  CHECK(grid, "4000 us apart");
}

// ---- MCI -------------------------------------------------------------------------------------------------

void test_mci() {
  Rig g;
  g.fake.song_length_us = 2'000'000;
  win32::Vfs& vfs = g.rt.vfs();
  vfs.mount_overlay("C:\\AFTERDRK", "", "");
  std::vector<uint8_t> smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96};
  vfs.add_virtual_file("C:\\AFTERDRK\\SONG.MID", smf);
  vfs.add_virtual_file("C:\\AFTERDRK\\JUNK.MID", std::vector<uint8_t>(32, 'j'));
  vfs.set_cwd("C:\\AFTERDRK");
  uint16_t rec = g.data(512);
  uint16_t hwnd = make_window(g, rec);
  std::string ret;
  // The engines' init probe: a device-only open, closed again.
  CHECK(g.mci("open sequencer", &ret) == 0 && ret == "1", "open sequencer -> device 1 (%s)", ret.c_str());
  CHECK(g.mci("open sequencer") == 265, "open sequencer again: MCIERR_DEVICE_OPEN");
  CHECK(g.mci("close sequencer") == 0 && g.fake.songs.empty(), "close sequencer");
  // "open sequencer!%s alias fred wait" (ADXPL310 DGROUP 0x477).
  CHECK(g.mci("open sequencer!C:\\AFTERDRK\\SONG.MID alias fred wait", &ret) == 0 && ret == "1" &&
            g.fake.songs.size() == 1,
        "open sequencer!path alias fred wait -> %s", ret.c_str());
  CHECK(g.mci("open sequencer!SONG.MID alias FRED") == 289, "a second fred: MCIERR_DUPLICATE_ALIAS");
  CHECK(g.mci("status fred length", &ret) == 0 && ret == "2000", "length 2000 ms (%s)", ret.c_str());
  CHECK(g.mci("status Fred mode", &ret) == 0 && ret == "stopped", "mode stopped (%s)", ret.c_str());
  CHECK(g.mci("status fred position", &ret) == 0 && ret == "0", "position 0");
  CHECK(g.mci("status fred ready", &ret) == 0 && ret == "true", "ready");
  CHECK(g.mci("status fred bogus") == 259 && g.mci("status fred") == 273, "unknown / missing status items");
  CHECK(g.mci("set fred time format milliseconds") == 0 && g.mci("set fred time format ms") == 0 &&
            g.mci("set fred time format frames") == 293,
        "time format ms only");
  CHECK(g.mci("play fred wait") == 274, "play ... wait: MCIERR_UNSUPPORTED_FUNCTION");
  // play ... notify: MM_MCINOTIFY(SUCCESSFUL, id) at the song's end, to hwndCallback.
  uint64_t t0 = g.now();
  CHECK(g.mci("play fred notify", &ret, hwnd) == 0 && ret.empty(), "play fred notify");
  CHECK(g.mci("status fred mode", &ret) == 0 && ret == "playing", "mode playing (%s)", ret.c_str());
  g.to(t0 + 1990000);
  g.tick();
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).empty(), "no notify before the end");
  g.to(t0 + 2000000 + 1000);
  g.tick();
  audio16_pump(g.rt);
  std::vector<Rec> r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].msg == 0x3B9 && r[0].wp == 1 && r[0].lp == 1,
        "MM_MCINOTIFY(SUCCESSFUL, device 1) at 2 s, dispatched by the pump: %s", records_text(r).c_str());
  CHECK(g.mci("status fred mode", &ret) == 0 && ret == "stopped" && g.mci("status fred position", &ret) == 0 &&
            ret == "2000",
        "stopped at the end (%s)", ret.c_str());
  // The engines' loop: seek to start, play notify. A second notify supersedes the first.
  clear_records(g, rec);
  CHECK(g.mci("seek fred to start wait") == 0 && g.mci("play fred notify", nullptr, hwnd) == 0 &&
            g.mci("play fred notify", nullptr, hwnd) == 0,
        "seek, play notify twice");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].wp == 2, "the first play's notify: SUPERSEDED: %s", records_text(r).c_str());
  // stop ... notify: the play's ABORTED first, then the stop's own SUCCESSFUL.
  clear_records(g, rec);
  CHECK(g.mci("stop fred notify", nullptr, hwnd) == 0, "stop notify");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 2 && r[0].wp == 4 && r[1].wp == 1, "ABORTED, then the stop's SUCCESSFUL: %s",
        records_text(r).c_str());
  // play from/to: SUCCESSFUL when the song reaches `to`, stopped there.
  clear_records(g, rec);
  uint64_t t1 = g.now();
  CHECK(g.mci("play fred from 500 to 1000 notify", nullptr, hwnd) == 0, "play from 500 to 1000 notify");
  g.to(t1 + 490000);
  g.tick();
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).empty(), "not yet at 490 ms");
  g.to(t1 + 501000);
  g.tick();
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].wp == 1 && g.mci("status fred mode", &ret) == 0 && ret == "stopped" &&
            g.mci("status fred position", &ret) == 0 && ret == "1000",
        "SUCCESSFUL at `to`, stopped at 1000 ms (%s)", ret.c_str());
  CHECK(g.mci("play fred from 3000") == 282, "from past the end: MCIERR_OUTOFRANGE");
  // close with a pending notify: ABORTED.
  clear_records(g, rec);
  g.mci("play fred notify", nullptr, hwnd);
  CHECK(g.mci("close fred") == 0 && g.fake.songs.empty(), "close fred");
  audio16_pump(g.rt);
  r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].wp == 4, "close: ABORTED: %s", records_text(r).c_str());
  // Errors.
  CHECK(g.mci("open NOSUCH.MID type sequencer alias x") == 275, "a missing file: MCIERR_FILE_NOT_FOUND");
  CHECK(g.mci("open JUNK.MID type sequencer alias x") == 296, "not an SMF: MCIERR_INVALID_FILE");
  CHECK(g.mci("open cdaudio alias qwanza wait") == 306 && g.mci("open waveaudio!x.wav") == 306,
        "other devices: MCIERR_DEVICE_NOT_INSTALLED");
  CHECK(g.mci("open") == 292, "open alone: MCIERR_MISSING_DEVICE_NAME");
  CHECK(g.mci("pause fred") == 261 && g.mci("") == 267, "unknown verb / empty: %u", g.mci(""));
  CHECK(g.mci("play nobody") == 263, "an unknown device: MCIERR_INVALID_DEVICE_NAME");
  CHECK(g.mci("open \"C:\\AFTERDRK\\SONG.MID") == 294, "an unclosed quote");
  // The other open forms, and close all.
  CHECK(g.mci("open SONG.MID alias a", &ret) == 0 && ret == "1", "open by extension");
  CHECK(g.mci("open \"C:\\AFTERDRK\\SONG.MID\" type Sequencer alias b", &ret) == 0 && ret == "2",
        "open <quoted path> type sequencer alias b");
  CHECK(g.mci("status b length", &ret, 0, 3) == 268 && ret == "20", "a short return buffer: overflow, NUL-terminated (%s)",
        ret.c_str());
  CHECK(g.mci("close all wait") == 0 && g.fake.songs.empty() && g.mci("status a mode") == 263, "close all");
}

// mciSendCommand(MCI_OPEN): the device MCI_OPEN_PARMS names (the ID is not
// looked at), as "open" of a command string: the sequencer by name or type
// number, with an element and an alias, its ID written to wDeviceID; any
// other type (DictaBird's "waveaudio" with a new element) is
// MCIERR_DEVICE_NOT_INSTALLED. Without an engine, the silent device's
// MCIERR_INVALID_DEVICE_ID. mciGetErrorString: TRUE and a text for the
// errors this machine answers, cut to the buffer; FALSE and "" otherwise.
void test_mci_open_command() {
  Rig g;
  g.fake.song_length_us = 2'000'000;
  win32::Vfs& vfs = g.rt.vfs();
  vfs.mount_overlay("C:\\SAVER", "", "");
  vfs.add_virtual_file("C:\\SAVER\\SONG.MID", {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96});
  vfs.set_cwd("C:\\SAVER");
  const uint32_t parms = uint32_t(g.data(64)) << 16;
  auto open = [&](uint16_t id, uint32_t flags, uint32_t type, uint32_t element, uint32_t alias) {
    for (uint32_t i = 0; i < 20; i += 4) g.rt.wr32(parms + i, 0);
    g.rt.wr32(parms + 8, type);
    g.rt.wr32(parms + 12, element);
    g.rt.wr32(parms + 16, alias);
    return g.api("MMSYSTEM", "mciSendCommand", {w16(id), w16(0x0803), l16(flags), l16(parms)});
  };
  auto close = [&](uint16_t id) { return g.api("MMSYSTEM", "mciSendCommand", {w16(id), w16(0x0804), l16(0), l16(0)}); };
  CHECK(open(0, 0x2002, g.str("sequencer"), 0, 0) == 0 && g.rt.rd16(parms + 4) == 1,
        "MCI_OPEN type \"sequencer\", wait: device 1 in wDeviceID (%u)", g.rt.rd16(parms + 4));
  CHECK(close(1) == 0, "MCI_CLOSE of it");
  CHECK(open(77, 0x3000, 523, 0, 0) == 0 && g.rt.rd16(parms + 4) == 1 && close(1) == 0,
        "MCI_OPEN_TYPE_ID 523 (MCI_DEVTYPE_SEQUENCER), any wDeviceID: the sequencer");
  CHECK(open(0, 0x2600, g.str("sequencer"), g.str("SONG.MID"), g.str("bob")) == 0 && g.fake.songs.size() == 1,
        "type, element and alias: the song opened");
  std::string ret;
  CHECK(g.mci("status bob length", &ret) == 0 && ret == "2000", "the alias names it to the strings (%s)", ret.c_str());
  CHECK(close(g.rt.rd16(parms + 4)) == 0 && g.fake.songs.empty(), "closed by its ID");
  CHECK(open(0, 0x2202, g.str("waveaudio"), g.str(""), 0) == 306,
        "DictaBird's open (waveaudio, a new element): MCIERR_DEVICE_NOT_INSTALLED");
  CHECK(open(0, 0x3000, 522, 0, 0) == 306, "MCI_DEVTYPE_WAVEFORM_AUDIO by number: the same");
  CHECK(open(0, 0x0200, 0, g.str("NOSUCH.MID"), 0) == 275, "an element alone: by its extension, as the strings");
  CHECK(open(0, 0x0000, 0, 0, 0) == 292, "nothing named: MCIERR_MISSING_DEVICE_NAME");
  CHECK(g.api("MMSYSTEM", "mciSendCommand", {w16(0), w16(0x0803), l16(0x2000), l16(0)}) == 273,
        "no MCI_OPEN_PARMS: MCIERR_MISSING_PARAMETER");
  {
    Rig off(false);
    const uint32_t p2 = uint32_t(off.data(64)) << 16;
    off.rt.wr32(p2 + 8, off.str("sequencer"));
    CHECK(off.api("MMSYSTEM", "mciSendCommand", {w16(0), w16(0x0803), l16(0x2000), l16(p2)}) == 257,
          "no engine: the silent device's MCIERR_INVALID_DEVICE_ID");
  }
  const uint32_t buf = uint32_t(g.data(128)) << 16;
  auto text = [&](uint32_t code, uint16_t len) {
    g.rt.write_str(buf, "junk", 8);
    const uint32_t r = g.api("MMSYSTEM", "mciGetErrorString", {l16(code), l16(buf), w16(len)}) & 0xFFFF;
    return std::to_string(r) + ":" + g.rt.read_str(buf);
  };
  CHECK(text(306, 128) == "1:The device is not installed.", "mciGetErrorString(306): %s", text(306, 128).c_str());
  CHECK(text(257, 128).rfind("1:The MCI device ID", 0) == 0, "257 too: %s", text(257, 128).c_str());
  CHECK(text(306, 8) == "1:The dev", "cut to the buffer, NUL-terminated: %s", text(306, 8).c_str());
  CHECK(text(12345, 128) == "0:", "an unknown code: FALSE and \"\" (%s)", text(12345, 128).c_str());
}

// The ADXPL3xx/40 music gates (AUDIO.md §2.9, §8.5).
void test_gates() {
  Rig g;
  uint16_t th = g.api16("KERNEL", "LoadLibrary", {l16(g.str("TOOLHELP.DLL"))});
  uint16_t mh = g.api16("KERNEL", "LoadLibrary", {l16(g.str("MCISEQ.DRV"))});
  CHECK(th >= 32 && mh >= 32, "TOOLHELP.DLL (%u) and MCISEQ.DRV (%u) load", th, mh);
  CHECK(g.api("KERNEL", "GetProcAddress", {w16(th), l16(g.str("GLOBALFIRST"))}) != 0 &&
            g.api("KERNEL", "GetProcAddress", {w16(th), l16(g.str("GLOBALNEXT"))}) != 0,
        "GetProcAddress(TOOLHELP, GLOBALFIRST/GLOBALNEXT)");
  CHECK((g.api("KERNEL", "GetModuleHandle", {l16(mh)}) & 0xFFFF) != 0, "GetModuleHandle(MAKELONG(hMciSeq, 0))");
  g.api("KERNEL", "FreeLibrary", {w16(mh)});
  CHECK(g.api16("KERNEL", "LoadLibrary", {l16(g.str("MCISEQ.DRV"))}) >= 32, "MCISEQ.DRV reloads (every 100 songs)");
  CHECK(g.api16("MMSYSTEM", "midiOutGetNumDevs", {}) >= 1, "IsMusicAvail: a MIDI output device");
}

// ---- the real engine ------------------------------------------------------------------------------------------

// A format-0 SMF: 480 PPQN, 500000 µs per quarter, a note from 0 to 480
// ticks, end of track at 960 ticks (1 s).
std::vector<uint8_t> one_second_smf() {
  std::vector<uint8_t> trk = {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,   // tempo 500000
                              0x00, 0x90, 60, 64,                       // note on
                              0x83, 0x60, 0x80, 60, 0,                   // +480: note off
                              0x83, 0x60, 0xFF, 0x2F, 0x00};             // +480: end of track
  std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k'};
  f.push_back(0), f.push_back(0), f.push_back(uint8_t(trk.size() >> 8)), f.push_back(uint8_t(trk.size()));
  append(f, trk);
  return f;
}

void test_real_engine() {
  audio::Config cfg;
  cfg.guest_sound = true;
  std::unique_ptr<audio::Engine> engine = audio::make_engine(cfg);
  CHECK(engine && engine->enabled(), "make_engine(guest_sound)");
  if (!engine) return;
  Rig g(true, true, engine.get());
  // A 11025-frame 22050 Hz sound: 500 ms, charged in full by a synchronous call.
  uint32_t p = g.bytes(pcm8(22050, 11025));
  g.to(1000);
  uint64_t t0 = g.now();
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(p), w16(0x0006)}) == 1, "synchronous, real engine");
  CHECK(g.now() >= t0 + 500000 && g.now() < t0 + 501000, "returned 500 ms later (%llu us)",
        (unsigned long long)(g.now() - t0));
  // MS-ADPCM through the real decoder.
  audio::WaveFormat ms = audio::ms_adpcm_format(11025, 1);
  std::vector<uint8_t> data(size_t(ms.block_align) * 2, 0x5A);
  data[0] = 0, data[1] = 16, data[2] = 0, data[256] = 1, data[257] = 16, data[258] = 0;
  CHECK(g.api16("MMSYSTEM", "sndPlaySound", {l16(g.bytes(riff(audio::waveformat_bytes(ms), data))), w16(7)}) == 1,
        "MS-ADPCM plays on the real engine");
  g.api("MMSYSTEM", "sndPlaySound", {l16(0), w16(0)});
  // A real song: MM_MCINOTIFY at its 1 s end.
  g.rt.vfs().mount_overlay("C:\\AFTERDRK", "", "");
  g.rt.vfs().add_virtual_file("C:\\AFTERDRK\\ONE.MID", one_second_smf());
  uint16_t rec = g.data(512);
  uint16_t hwnd = make_window(g, rec);
  std::string ret;
  CHECK(g.mci("open sequencer!C:\\AFTERDRK\\ONE.MID alias fred wait", &ret) == 0 &&
            g.mci("status fred length", &ret) == 0 && ret == "1000",
        "a real SMF opens, 1000 ms (%s)", ret.c_str());
  uint64_t t1 = g.now();
  g.mci("play fred notify", nullptr, hwnd);
  g.to(t1 + 990000);
  g.tick();
  audio16_pump(g.rt);
  CHECK(window_records(g, rec).empty(), "no notify at 990 ms");
  g.to(t1 + 1001000);
  g.tick();
  audio16_pump(g.rt);
  std::vector<Rec> r = window_records(g, rec);
  CHECK(r.size() == 1 && r[0].msg == 0x3B9 && r[0].wp == 1, "MM_MCINOTIFY(SUCCESSFUL) at 1 s: %s",
        records_text(r).c_str());
  g.mci("close all");
  audio16_close(g.rt);
  engine->shutdown(g.now());
}

// The .mid event log's channel messages: (ms, bytes). Format 0, explicit status bytes (AUDIO.md §6.6).
std::vector<std::pair<uint64_t, std::vector<uint8_t>>> read_mid_log(const std::string& path) {
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> out;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return out;
  std::vector<uint8_t> b;
  for (int ch; (ch = fgetc(f)) != EOF;) b.push_back(uint8_t(ch));
  fclose(f);
  size_t i = 22;  // MThd (14) + MTrk header (8)
  uint64_t ms = 0;
  auto vlq = [&] {
    uint32_t v = 0;
    while (i < b.size()) {
      uint8_t x = b[i++];
      v = (v << 7) | (x & 0x7F);
      if (!(x & 0x80)) break;
    }
    return v;
  };
  while (i < b.size()) {
    ms += vlq();
    if (i >= b.size()) break;
    uint8_t st = b[i];
    if (st == 0xFF) {
      i += 2;
      i += vlq();
    } else if (st == 0xF0 || st == 0xF7) {
      i++;
      i += vlq();
    } else {
      size_t n = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 2 : 3;
      if (i + n > b.size()) break;
      out.push_back({ms, std::vector<uint8_t>(b.begin() + ptrdiff_t(i), b.begin() + ptrdiff_t(i + n))});
      i += n;
    }
  }
  return out;
}

// The real engine under a self-sequencing guest: a 4 ms timer procedure that
// plays a note each period, delivered once per 60 Hz frame (a module that
// makes no call between frames). The .mid log still has the notes 4 ms apart:
// the procedure's calls are dated at its periods. Also measures what 250
// callbacks a virtual second cost the host.
void test_real_engine_midi() {
  char tmp[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  const std::string wav = std::string(tmp) + "adw_sound16_" + std::to_string(GetCurrentProcessId()) + "_midi.wav";
  const std::string mid = wav.substr(0, wav.size() - 4) + ".mid";
  auto frames = [](Rig& g, audio::Engine& e, int from, int to) {
    for (int f = from; f <= to; f++) {
      g.to(uint64_t(f) * 16667);
      audio16_pump(g.rt);
      e.advance(g.now());
    }
  };
  {
    audio::Config cfg;
    cfg.guest_sound = true;
    cfg.capture_wav = wav;
    cfg.capture_mid = mid;
    std::unique_ptr<audio::Engine> engine = audio::make_engine(cfg);
    Rig g(true, true, engine.get());
    uint16_t rec = g.data(0x8000);
    uint32_t midi = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "midiOutShortMsg"));
    uint16_t cs = g.code(timeproc_recorder(rec, midi));
    uint32_t mem = uint32_t(g.data(0x40)) << 16;
    CHECK(g.api16("MMSYSTEM", "midiOutOpen", {l16(mem), w16(0xFFFF), l16(0), l16(0), l16(0)}) == 0, "real engine: open");
    uint16_t h = g.rt.rd16(mem);
    g.rt.wr16((uint32_t(rec) << 16) + 4, h);
    frames(g, *engine, 1, 1);
    uint16_t id = set_timer(g, 4, cs, 0, true);
    frames(g, *engine, 2, 61);  // one virtual second
    g.api("MMSYSTEM", "timeKillEvent", {w16(id)});
    g.api("MMSYSTEM", "midiOutReset", {w16(h)});
    g.api("MMSYSTEM", "midiOutClose", {w16(h)});
    audio16_close(g.rt);
    engine->shutdown(g.now());
    CHECK(engine->stats().midi_events >= 250, "MIDI events: %llu", (unsigned long long)engine->stats().midi_events);
  }
  auto log = read_mid_log(mid);
  std::vector<uint64_t> ons;
  for (const auto& [ms, m] : log) {
    if (m.size() == 3 && m[0] == 0x90 && m[2] > 0) ons.push_back(ms);
  }
  bool spaced = ons.size() >= 245 && ons.size() <= 251;
  for (size_t k = 1; k < ons.size() && spaced; k++) spaced = ons[k] - ons[k - 1] >= 3 && ons[k] - ons[k - 1] <= 5;
  CHECK(spaced, "%zu note-ons 4 ms apart in the log, though delivered once a frame", ons.size());
  size_t resets = 0;
  for (const auto& [ms, m] : log) resets += m.size() == 3 && m[0] == 0xB0 && m[1] == 123;
  CHECK(resets == 1 && !log.empty() && log.back().second == std::vector<uint8_t>({0xB0, 123, 0}),
        "midiOutReset ends it: note-off, sustain off, all notes off");
  DeleteFileA(wav.c_str());
  DeleteFileA(mid.c_str());

  // The cost of 250 callbacks a virtual second (each a nested call_far and a
  // midiOutShortMsg into the engine), against the same frames without the event.
  auto host_ms = [&](bool timer) {
    audio::Config cfg;
    cfg.guest_sound = true;
    std::unique_ptr<audio::Engine> engine = audio::make_engine(cfg);
    Rig g(true, true, engine.get());
    uint16_t rec = g.data(0x8000);
    uint32_t midi = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "midiOutShortMsg"));
    uint16_t cs = g.code(timeproc_recorder(rec, midi));
    uint32_t mem = uint32_t(g.data(0x40)) << 16;
    g.api("MMSYSTEM", "midiOutOpen", {l16(mem), w16(0xFFFF), l16(0), l16(0), l16(0)});
    g.rt.wr16((uint32_t(rec) << 16) + 4, g.rt.rd16(mem));
    if (timer) set_timer(g, 4, cs, 0, true);
    LARGE_INTEGER f0, a, b;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&a);
    for (int f = 1; f <= 600; f++) {  // 10 virtual seconds at 60 Hz, a call and the pump per frame
      g.to(uint64_t(f) * 16667);
      g.tick();
      audio16_pump(g.rt);
      engine->advance(g.now());
    }
    QueryPerformanceCounter(&b);
    uint64_t calls = timer16_stats(g.rt).calls;
    engine->shutdown(g.now());
    return std::make_pair(double(b.QuadPart - a.QuadPart) * 1000.0 / double(f0.QuadPart), calls);
  };
  auto base = host_ms(false), with = host_ms(true);
  CHECK(with.second >= 2490 && with.second <= 2500, "2500 callbacks in 10 virtual seconds (%llu)",
        (unsigned long long)with.second);
  printf("timer cost: %llu callbacks in 10 virtual s: %.1f ms host (%.1f without the event): %.2f us per callback\n",
         (unsigned long long)with.second, with.first, base.first,
         with.second ? (with.first - base.first) * 1000.0 / double(with.second) : 0.0);
}

// The same sequencer in the ne16 lane's order: the pump, then a DRAWFRAME of
// ~10 ms guest work that makes no API call (the periods due meanwhile wait for
// the next frame's pump), the frame's time settled, and the engine rendered —
// only up to Runtime16::audio_due(), the next due point, as the lane's step
// end does, so the late periods' notes keep their dates. Rendered to the
// guest's time instead, they would be clamped to the frame's end.
void test_real_engine_midi_lane_order() {
  char tmp[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  const std::string wav = std::string(tmp) + "adw_sound16_" + std::to_string(GetCurrentProcessId()) + "_lane.wav";
  const std::string mid = wav.substr(0, wav.size() - 4) + ".mid";
  {
    audio::Config cfg;
    cfg.guest_sound = true;
    cfg.capture_wav = wav;
    cfg.capture_mid = mid;
    std::unique_ptr<audio::Engine> engine = audio::make_engine(cfg);
    Rig g(true, true, engine.get());
    uint16_t rec = g.data(0x8000);
    uint32_t midi = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "midiOutShortMsg"));
    uint16_t cs = g.code(timeproc_recorder(rec, midi));
    uint16_t work = g.code(busy_work(15));
    uint32_t mem = uint32_t(g.data(0x40)) << 16;
    g.api("MMSYSTEM", "midiOutOpen", {l16(mem), w16(0xFFFF), l16(0), l16(0), l16(0)});
    uint16_t h = g.rt.rd16(mem);
    g.rt.wr16((uint32_t(rec) << 16) + 4, h);
    set_timer(g, 4, cs, 0, true);
    for (int f = 1; f <= 60; f++) {
      g.to(uint64_t(f) * 16667);
      audio16_pump(g.rt);
      g.rt.call_far(uint32_t(work) << 16, {});
      g.rt.settle_time();
      engine->advance(std::min(g.rt.peek_us(), g.rt.audio_due()));
    }
    g.api("MMSYSTEM", "midiOutReset", {w16(h)});
    audio16_close(g.rt);
    engine->shutdown(g.now());
  }
  std::vector<uint64_t> ons;
  for (const auto& [ms, m] : read_mid_log(mid)) {
    if (m.size() == 3 && m[0] == 0x90 && m[2] > 0) ons.push_back(ms);
  }
  std::map<uint64_t, int> gaps;
  for (size_t k = 1; k < ons.size(); k++) gaps[ons[k] - ons[k - 1]]++;
  bool spaced = ons.size() >= 245 && ons.size() <= 255;
  for (const auto& [gap, n] : gaps) spaced &= gap >= 3 && gap <= 5;
  std::string hist;
  for (const auto& [gap, n] : gaps) hist += " " + std::to_string(gap) + ":" + std::to_string(n);
  CHECK(spaced, "%zu note-ons 4 ms apart, frames of 10 ms work without a call (gaps ms:count%s)", ons.size(), hist.c_str());
  DeleteFileA(wav.c_str());
  DeleteFileA(mid.c_str());
}

// A timer procedure's `play … to n notify`: MCI is dated at the delivery
// point, not at the procedure's due time (no interrupt-time API), so the song
// plays its n ms before it stops and the notify goes — also when the engine
// was rendered past the procedure's due time before it was delivered.
void test_mci_from_procedure() {
  audio::Config cfg;
  cfg.guest_sound = true;
  std::unique_ptr<audio::Engine> engine = audio::make_engine(cfg);
  Rig g(true, true, engine.get());
  g.rt.vfs().mount_overlay("C:\\AFTERDRK", "", "");
  g.rt.vfs().add_virtual_file("C:\\AFTERDRK\\ONE.MID", one_second_smf());
  uint16_t rec = g.data(512);
  uint16_t hwnd = make_window(g, rec);
  CHECK(g.mci("open sequencer!C:\\AFTERDRK\\ONE.MID alias fred wait") == 0, "open fred");
  // TimeProc: mciSendString("play fred to 800 notify", NULL, 0, hwnd), FAR PASCAL.
  uint32_t cmd = g.str("play fred to 800 notify");
  uint32_t fn = g.rt.thunk_far(*g.rt.shims().find_name("MMSYSTEM", "mciSendString"));
  uint16_t cs = g.code({0x55, 0x8B, 0xEC,                                                  // push bp; mov bp,sp
                        0x68, uint8_t(cmd >> 16), uint8_t(cmd >> 24), 0x68, uint8_t(cmd), uint8_t(cmd >> 8),
                        0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00,                                // no return string
                        0x68, uint8_t(hwnd), uint8_t(hwnd >> 8),                           // hwndCallback
                        0x9A, uint8_t(fn), uint8_t(fn >> 8), uint8_t(fn >> 16), uint8_t(fn >> 24),
                        0x5D, 0xCA, 0x10, 0x00});                                          // pop bp; retf 10h
  uint16_t work = g.code(busy_work(15));
  g.to(16667);
  audio16_pump(g.rt);
  set_timer(g, 4, cs, 0, false);           // one-shot, due inside the work below
  g.rt.call_far(uint32_t(work) << 16, {});
  g.rt.settle_time();
  engine->advance(g.rt.peek_us());         // the engine rendered past the procedure's due time
  g.to(2 * 16667);
  audio16_pump(g.rt);                      // the procedure runs here: the song starts now
  const uint64_t start = g.now();
  std::string mode, pos;
  g.to(start + 790000);  // reckoned from the procedure's due time, the song would have stopped by now
  g.tick();
  audio16_pump(g.rt);
  CHECK(g.mci("status fred mode", &mode) == 0 && mode == "playing" && window_records(g, rec).empty(),
        "790 ms after the start: still playing, no notify (%s)", mode.c_str());
  g.to(start + 801000);
  g.tick();
  audio16_pump(g.rt);
  std::vector<Rec> r = window_records(g, rec);
  CHECK(g.mci("status fred mode", &mode) == 0 && mode == "stopped" && g.mci("status fred position", &pos) == 0 &&
            pos == "800",
        "801 ms after: stopped at 800 ms (%s, %s)", mode.c_str(), pos.c_str());
  CHECK(r.size() == 1 && r[0].msg == 0x3B9 && r[0].wp == 1, "MM_MCINOTIFY(SUCCESSFUL) at 800 ms of play: %s",
        records_text(r).c_str());
  g.mci("close all");
  audio16_close(g.rt);
  engine->shutdown(g.now());
}

}  // namespace

int main() {
  // AD_TEST_TRACE=sound,user16: the shims' trace lines.
  if (const char* t = getenv("AD_TEST_TRACE")) {
    std::set<std::string> cats;
    for (std::string c; *t; t++) {
      if (*t != ',') c.push_back(*t);
      if (*t == ',' || !t[1]) cats.insert(c), c.clear();
    }
    set_trace_categories(cats);
  }
  try {
    test_disabled();
    test_devices();
    test_snd();
    test_waveout();
    test_wom_window();
    test_wom_function();
    test_midi_out();
    test_midi_function();
    test_resend_chain();
    test_timers();
    test_timer_chain();
    test_timers_without_engine();
    test_timer_determinism();
    test_mci();
    test_mci_open_command();
    test_gates();
    test_real_engine();
    test_real_engine_midi();
    test_real_engine_midi_lane_order();
    test_mci_from_procedure();
  } catch (const std::exception& e) {
    printf("FAIL: exception %s\n", e.what());
    return 1;
  }
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
