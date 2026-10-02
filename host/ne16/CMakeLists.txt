# adw_lane_ne16 — the Classic lane: an After Dark 2.x/3.x module (NE .AD) run
# through the real OLDMOD16.DLL (+ AD_SND, ADXPL300, helpers) on the Win16
# guest runtime (host/win16), driven as AFTERDAR.SCR drove it through
# OLDMOD32 (docs/ABI.md §3), an Intermission module (NE .IMX) driven as
# INTERMIS.EXE drove it through IMIMXPLY.IMQ, or a Windows 3.1 screen saver
# (an NE .SCR application) run as Windows 3.1 ran it. See lane.hh; the module
# protocols are behind protocol.hh (ad3_protocol.cc, imx_protocol.cc,
# scr_protocol.cc).

add_library(adw_lane_ne16 STATIC
  lane.cc
  ad3_protocol.cc
  bridge.cc
  imx_protocol.cc
  imreader.cc
  scr_protocol.cc
  package.cc
)
target_include_directories(adw_lane_ne16 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(adw_lane_ne16 PUBLIC adw_win16 adw_core)
target_compile_features(adw_lane_ne16 PUBLIC cxx_std_20)

# adhostwin links whatever library defines adw::make_ne16_lane() (core/README.md).
set_property(GLOBAL PROPERTY ADW_LANE_NE16_TARGET adw_lane_ne16)

# ---- tests ------------------------------------------------------------------
add_executable(adw_ne16_tests tests/test_ne16.cc)
target_link_libraries(adw_ne16_tests PRIVATE adw_lane_ne16)
# test_paths.h: the installed assets, and hosts kept off the real %LOCALAPPDATA%.
target_include_directories(adw_ne16_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests)
add_test(NAME ne16.unit COMMAND adw_ne16_tests)
# Runs real Classic modules from the imported assets through adhostwin; skips (77) when absent.
add_test(NAME ne16.assets COMMAND adw_ne16_tests --assets $<TARGET_FILE:adhostwin>)
set_tests_properties(ne16.assets PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# The PACKAGES.md §4.4 package roots under AD_NE16_PKGROOTS (one root per
# package id: <root>\<id>\win\packages\<id>\…); skips (77) when unset or absent.
add_test(NAME ne16.pkg COMMAND adw_ne16_tests --pkg $<TARGET_FILE:adhostwin>)
set_tests_properties(ne16.pkg PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# INTERACTION.md: the games' status (Caps Lock, keyboard hooks, LUNATIC's
# queue), the DOS Shell regression (18000 frames), and the module buttons in
# configure mode (hidden real dialogs, scripted; state in a temp ADSTATE;
# Globe's map picked through [-h-] in a folder of the host's own, which a
# later run loads).
# Needs the imported assets (all five packages for every case; each absent
# module is skipped); skips (77) when there are none.
add_test(NAME ne16.interaction COMMAND adw_ne16_tests --interaction $<TARGET_FILE:adhostwin>)
set_tests_properties(ne16.interaction PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
# Star Wars Screen Entertainment's Intermission modules (lane.hh "Intermission
# (IMX)") from an imported package: AD_NE16_SWSE_ROOT (an assets root), else
# AD_ASSETS_DIR or the installed assets; skips (77) when none holds it. Every
# module twice, keys the saver does not wake on (BATTLES, VADER), SWTEXT's and
# TRENCH's passes a second, the reader oracle, a small screen, the desktop
# seed, a streamed start, sound (effects, and the music at the saver's
# volume), and VADER's button in configure mode (hidden, scripted; state in a
# temp ADSTATE).
add_test(NAME ne16.swse COMMAND adw_ne16_tests --swse $<TARGET_FILE:adhostwin>)
set_tests_properties(ne16.swse PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
# Star Trek: The Screen Saver's After Dark 2.0 modules (lane.hh "The AD3
# protocol") from an imported package: AD_NE16_STARTREK_ROOT (an assets root),
# else AD_ASSETS_DIR or the installed assets; skips (77) when none holds it.
# Every module twice (AD_SND 1.0, the AD_PREFS.INI seeds), sound captured
# twice, Scotty's Files' blueprints (masks from 1-bpp DIBs), the two buttons
# in configure mode (hidden, scripted; state in a temp ADSTATE; Sounder's
# folder list with H:, and a folder of the host's own chosen through it,
# which a later run plays), and a scripted Final Exam (Num Lock, answers, the
# mouse, the wake; headless and streamed).
add_test(NAME ne16.startrek COMMAND adw_ne16_tests --startrek $<TARGET_FILE:adhostwin>)
set_tests_properties(ne16.startrek PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
# Snoopy's Screen Savers' eight modules (PACKAGES.md §7.4: a package with no
# engine dir, run over the host's own AD_SND with After Dark 2.0's computed
# palettes) from an imported package: AD_NE16_SNOOPY_ROOT (an assets root),
# else AD_ASSETS_DIR or the installed assets; skips (77) when none holds it.
# Every module twice (no fault at close), sound captured twice (the three
# play flags; the two modules without AD_SND silent), and no wave device.
add_test(NAME ne16.snoopy COMMAND adw_ne16_tests --snoopy $<TARGET_FILE:adhostwin>)
set_tests_properties(ne16.snoopy PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
