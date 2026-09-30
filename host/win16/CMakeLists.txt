# adw_win16 — the Win16 guest runtime for the Classic lane (host/ne16): the CPU
# in 16-bit protected mode over a host-owned LDT, the global and local heaps,
# far-call import thunks + the Win16 shim registry, the NE module table, the
# DOS/BIOS/VGA machine underneath, and the KERNEL/USER/GDI/MMSYSTEM/… shims.
# GDI draws with real GDI on the host-level display model of host/win32
# (display.hh). See host/win16/README.md.

add_library(adw_win16 STATIC
  ldt.cc
  global_heap.cc
  local_heap.cc
  shims16.cc
  signatures16.cc
  runtime16.cc
  modules16.cc
  keyboard16.cc
  dialogs16.cc
  dos16.cc
  # API families (register_<module>16(Runtime16&)); see shim_families16.hh.
  kernel16.cc
  user16.cc
  gdi16.cc
  gdi16_objects.cc
  system16.cc
  # MMSYSTEM's sound half over the host audio engine (sound16.hh, AUDIO.md §8).
  sound16.cc
  # The host's own AD_SND, for a package that ships none (register_host_ad_snd).
  adsnd16.cc
)
target_include_directories(adw_win16 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# adw_win32 for the host-level pieces only: Display (win32/display.hh),
# GuestHeap (win32/heap.hh) and Vfs (win32/vfs.hh), none of which need a
# Win32 guest runtime.
target_link_libraries(adw_win16 PUBLIC adw_core adw_cpu adw_loader adw_win32 gdi32 user32 comdlg32)
target_compile_features(adw_win16 PUBLIC cxx_std_20)

# ---- tests ------------------------------------------------------------------
add_executable(adw_win16_tests tests/test_win16.cc)
target_link_libraries(adw_win16_tests PRIVATE adw_win16)
# test_paths.h: where the installed assets are.
target_include_directories(adw_win16_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests)
add_test(NAME win16.unit COMMAND adw_win16_tests)
# MMSYSTEM's sound half (sound16.hh) against a scripted engine and the real
# one (adw/core/audio.h): sndPlaySound, waveOut, midiOut/aux, the MCI strings,
# callback delivery (AUDIO.md §10.2).
add_executable(adw_win16_sound_tests tests/test_sound16.cc)
target_link_libraries(adw_win16_sound_tests PRIVATE adw_win16)
add_test(NAME win16.sound COMMAND adw_win16_sound_tests)
# An adw::cpu regression this lane depends on (inc/dec must preserve DF; see
# run_cpu_df in tests/test_win16.cc): without it ADXPL300/AD_RSRC's backward
# memmove runs forward and 13 Classic modules fault.
add_test(NAME win16.cpu_df_regression COMMAND adw_win16_tests --cpu-df)
# OLDMOD16.DLL + AD_SND.DLL from the imported assets; skips (77) when they are absent.
add_test(NAME win16.assets COMMAND adw_win16_tests --assets)
set_tests_properties(win16.assets PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
