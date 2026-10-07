// The looks' unit tests (scr_unit looks; looks.h, present_d3d.h): each part
// in a file of its own (looks_core_test.cc, looks_crt_test.cc,
// looks_smooth_test.cc, looks_preset_test.cc), counting failures here.
// Without Direct3D 11 (d3d_available) the drawing checks are skipped.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "host_process.h"

namespace adw::scr::looks_test {

extern int failures;

#define LCHECK(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++::adw::scr::looks_test::failures;                                    \
    }                                                                        \
  } while (0)

// An 8-bit frame w x h whose pixel (x, y) is index(x, y), through `palette`
// (unset entries black).
Frame make_frame8(int w, int h, const std::function<uint8_t(int, int)>& index,
                  const std::vector<RGBQUAD>& palette);
// The same picture as a 32-bit frame (as a P6 stream comes).
Frame to_frame32(const Frame& f8);
// A flat frame of one grey level.
Frame grey_frame(int w, int h, uint8_t level);
// BGR pictures (render_frame_bgr_d3d's): one pixel's luma (Rec. 601, 0..255)
// and a rectangle's mean luma.
int luma_at(const std::vector<uint8_t>& bgr, int w, int x, int y);
double mean_luma(const std::vector<uint8_t>& bgr, int w, int x0, int y0, int x1, int y1);

// The parts. Each returns having counted its failures; the drawing ones run
// only where d3d_available().
void logic_checks();
void core_checks();
void crt_checks();
void smooth_checks();
void preset_checks();

} // namespace adw::scr::looks_test

// scr_unit's "looks" suite: the number of failures.
int run_looks_tests();
