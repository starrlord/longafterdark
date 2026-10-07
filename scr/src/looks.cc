#include "looks.h"

#include <algorithm>

#include "settings.h"

namespace adw::scr {

namespace {

struct Named {
  Look look;
  const char* name;
};
constexpr Named kNames[] = {{Look::sharp, "sharp"},
                            {Look::crt, "crt"},
                            {Look::crt_curved, "crt-curved"},
                            {Look::smooth, "smooth"},
                            {Look::preset, "preset"}};

} // namespace

const char* look_name(Look look) {
  for (const Named& n : kNames) {
    if (n.look == look) return n.name;
  }
  return "sharp";
}

bool parse_look(std::string_view text, Look* out) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  for (const Named& n : kNames) {
    if (iequals(text, n.name)) {
      if (out) *out = n.look;
      return true;
    }
  }
  return false;
}

double crt_scanline_strength(double k) {
  return std::clamp((k - 2.5) / 0.5, 0.0, 1.0);
}

} // namespace adw::scr
