// Lane selection by header alone (the loader owns real parsing): an MZ stub
// whose e_lfanew points at "PE\0\0" + i386 is an After Dark 4 module (PE32),
// one pointing at "NE" is a Classic module (Win16). A file that starts
// "AniN" or "AniM" is a Delrina Intermission ASA animation: data, not code,
// which Intermission's ASA reader (IMASAPLY.IMQ, a Win16 DLL) plays, so it
// is a Classic module too (the ne16 lane's Intermission protocol; IMASAPLY
// itself checks for these two headers).
#include <cstdio>
#include <cstring>

#include "adw/core/lane.h"
#include "adw/core/text.h"

namespace adw {

bool asa_header(const void* first4) {
  return memcmp(first4, "AniN", 4) == 0 || memcmp(first4, "AniM", 4) == 0;
}

const char* lane_kind_name(LaneKind k) {
  switch (k) {
    case LaneKind::pe32: return "pe32";
    case LaneKind::ne16: return "ne16";
    case LaneKind::unsupported: return "unsupported";
    case LaneKind::unreadable: return "unreadable";
  }
  return "?";
}

ModuleProbe probe_module(const std::string& path_utf8) {
  ModuleProbe p;
  FILE* f = _wfopen(widen(path_utf8).c_str(), L"rb");
  if (!f) {
    p.detail = "cannot open file";
    return p;
  }
  auto read_at = [&](long off, void* buf, size_t n) {
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
  };
  uint8_t mz[64];
  if (read_at(0, mz, 4) && asa_header(mz)) {
    fclose(f);
    p.kind = LaneKind::ne16;
    p.detail = "Intermission ASA animation";
    return p;
  }
  if (!read_at(0, mz, sizeof(mz))) {
    fclose(f);
    p.kind = LaneKind::unsupported;
    p.detail = "too short for an MZ header";
    return p;
  }
  if (mz[0] != 'M' || mz[1] != 'Z') {
    fclose(f);
    p.kind = LaneKind::unsupported;
    p.detail = "not an MZ executable";
    return p;
  }
  uint32_t lfanew = uint32_t(mz[0x3C]) | uint32_t(mz[0x3D]) << 8 | uint32_t(mz[0x3E]) << 16 |
                    uint32_t(mz[0x3F]) << 24;
  uint8_t sig[6] = {};
  bool have = lfanew >= 0x40 && lfanew < 0x10000000 && read_at(long(lfanew), sig, sizeof(sig));
  fclose(f);
  p.kind = LaneKind::unsupported;
  if (!have) {
    p.detail = "plain DOS MZ executable (no new-format header)";
  } else if (memcmp(sig, "PE\0\0", 4) == 0) {
    uint16_t machine = uint16_t(sig[4] | sig[5] << 8);
    if (machine == 0x014C) {
      p.kind = LaneKind::pe32;
      p.detail = "PE32 i386";
    } else {
      char buf[48];
      snprintf(buf, sizeof(buf), "PE for machine 0x%04X (not i386)", machine);
      p.detail = buf;
    }
  } else if (sig[0] == 'N' && sig[1] == 'E') {
    p.kind = LaneKind::ne16;
    p.detail = "NE (Win16)";
  } else if ((sig[0] == 'L' && (sig[1] == 'E' || sig[1] == 'X'))) {
    p.detail = "LE/LX (VxD/OS2) executable";
  } else {
    p.detail = "MZ with unrecognized new-format header";
  }
  return p;
}

}  // namespace adw
