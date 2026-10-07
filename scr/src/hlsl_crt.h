// The CRT look (looks.h, Look::crt and Look::crt_curved): a 1990s VGA
// monitor, as the modules were drawn for. Its passes follow present_d3d.h's
// pass contract.
#pragma once

#include <vector>

#include "present_d3d.h"

namespace adw::scr {

// `curved`: behind curved glass (Look::crt_curved).
std::vector<PassSpec> crt_passes(bool curved);

} // namespace adw::scr
