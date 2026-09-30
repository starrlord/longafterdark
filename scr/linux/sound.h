// Which hosts make sound (AUDIO.md §9), as the Windows saver decides it
// (scr/src/sound.*): exactly one host may play, the saver's own on the
// primary monitor, with sound on in the settings (here --sound, the
// default, or --no-sound). That host gets ADSOUND=1 ADVOLUME=<volume>;
// every other host gets ADSOUND=0 with ADVOLUME and ADAUDIOOUT removed, so
// nothing inherited turns its sound on (a set ADAUDIOOUT alone would): a
// preview, and the --capabilities probe. AD_SCR_SOUND=0 (or off, no,
// false) turns sound off whatever the options say.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lad {

inline constexpr int kDefaultVolume = 50;   // After Dark's own default (AUDIO.md §4)

// How long a host is given to end after QUIT before it is terminated: the
// sound host needs longer, to silence its device and send MIDI
// all-notes-off (AUDIO.md §9 "Waking": at least 200 ms). The Windows
// saver's values (scr/src/sound.h).
inline constexpr int kHostStopGraceMs = 150;
inline constexpr int kSoundHostStopGraceMs = 400;

enum class HostRole {
  saver,     // the full-screen saver, a window, or XScreenSaver's window
  preview,   // a preview in someone else's window (always silent)
  tool,      // --capabilities
};

bool sound_forced_off();   // AD_SCR_SOUND

struct SoundChoice {
  bool on = false;
  int volume = kDefaultVolume;   // 0..100; meaningful only when `on`
};
// Pure. `owner`: the host is the one on the primary monitor (the input
// owner's).
SoundChoice sound_for(bool sound_setting, int volume, HostRole role, bool owner, bool forced_off);

// Environment changes: name -> value, where an empty value removes it.
using EnvChanges = std::vector<std::pair<std::string, std::string>>;
void add_sound_env(EnvChanges& env, const SoundChoice& c);

}  // namespace lad
