#include "sound.h"

#include <strings.h>

#include <algorithm>
#include <cstdlib>

namespace lad {

bool sound_forced_off() {
  const char* v = getenv("AD_SCR_SOUND");
  if (!v) return false;
  std::string s(v);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  for (const char* off : {"0", "off", "no", "false"}) {
    if (strcasecmp(s.c_str(), off) == 0) return true;
  }
  return false;
}

SoundChoice sound_for(bool sound_setting, int volume, HostRole role, bool owner, bool forced_off) {
  SoundChoice c;
  c.on = role == HostRole::saver && owner && sound_setting && !forced_off;
  c.volume = c.on ? std::clamp(volume, 0, 100) : kDefaultVolume;
  return c;
}

void add_sound_env(EnvChanges& env, const SoundChoice& c) {
  std::erase_if(env, [](const auto& kv) {
    return strcasecmp(kv.first.c_str(), "ADSOUND") == 0 || strcasecmp(kv.first.c_str(), "ADVOLUME") == 0 ||
           strcasecmp(kv.first.c_str(), "ADAUDIOOUT") == 0;
  });
  if (c.on) {
    env.emplace_back("ADSOUND", "1");
    env.emplace_back("ADVOLUME", std::to_string(std::clamp(c.volume, 0, 100)));
    // ADAUDIOOUT (a capture) is left as inherited: a test or a person
    // chasing a problem can have the sound host write one.
  } else {
    env.emplace_back("ADSOUND", "0");
    env.emplace_back("ADVOLUME", "");
    env.emplace_back("ADAUDIOOUT", "");   // it would turn sound on by itself
  }
}

}  // namespace lad
