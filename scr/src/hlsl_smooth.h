// The Smooth look (looks.h, Look::smooth): an edge-directed upscale, for flat
// cartoon art. Its passes follow present_d3d.h's pass contract.
#pragma once

#include <vector>

#include "present_d3d.h"

namespace adw::scr {

std::vector<PassSpec> smooth_passes();

} // namespace adw::scr
