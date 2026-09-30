# LongAfterDark.scr — the Long After Dark screen saver front-end
# (docs/DESIGN.md §1 host protocol, §6a catalog + settings). A plain
# Win32 GUI exe with a .scr suffix; it runs no module code itself, only
# spawns adhostwin.exe.

# Everything testable without a window: argument parsing, the P8/P6 stream
# parser, settings.ini, the catalog, geometry, and the host process plumbing.
add_library(adw_scr STATIC
  src/args.cc
  src/catalog.cc
  src/desktop_seed.cc
  src/dialog_support.cc
  src/frame_parser.cc
  src/geometry.cc
  src/host_process.cc
  src/input_rules.cc
  src/log.cc
  src/paths.cc
  src/present.cc
  src/releases.cc
  src/settings.cc
  src/sound.cc
  src/ui_model.cc
)
target_include_directories(adw_scr PUBLIC src)
# The host status record (INTERACTION.md §3.4) is read through core's
# header-only reader, so both sides share one layout, and the data folder
# is found by core's header-only data_root.h, as adhostwin and adimport find
# it; nothing of adw_core is linked.
target_include_directories(adw_scr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../host/core/include)
# Windows 10 APIs (per-monitor-v2 DPI, high-resolution waitable timers).
target_compile_definitions(adw_scr PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00)
# Direct2D and WIC: the saver windows' presentation (present.h).
target_link_libraries(adw_scr PUBLIC phosg::phosg dwmapi shell32 ole32 gdi32 user32 d2d1 windowscodecs)

# The icon is drawn by a build-time program (no binary art in the repo).
add_executable(scr_gen_icon res/gen_icon.cc)
set(AD_SCR_ICON "${CMAKE_CURRENT_BINARY_DIR}/afterdark.ico")
add_custom_command(OUTPUT "${AD_SCR_ICON}"
  COMMAND scr_gen_icon "${AD_SCR_ICON}"
  DEPENDS scr_gen_icon
  COMMENT "Drawing afterdark.ico")
add_custom_target(scr_icon DEPENDS "${AD_SCR_ICON}")

# The settings dialog's Windows 11 look, shared with adimport's windows:
# adw_ui (common/ui, docs/COVERS.md §3). A build whose AD_COMPONENTS
# leaves common/ui out pulls it in here.
if(NOT TARGET adw_ui)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/ui ${CMAKE_BINARY_DIR}/common/ui)
endif()

# The settings dialog shows each module's own icon, read from the module file
# with the loader (PE32 and NE alike). Without host/loader in the build the
# list falls back to monogram tiles.
set(_scr_gui_libs adw_scr adw_ui user32 gdi32 gdiplus comctl32 uxtheme windowscodecs ole32 oleaut32 dwmapi d2d1 wtsapi32)
if(TARGET adw_loader)
  list(APPEND _scr_gui_libs adw_loader)
endif()
# The GUI code without test hooks, compiled once for both programs below.
add_library(adw_scr_gui OBJECT
  src/main.cc
  src/cover_strip.cc
  src/live_preview.cc
  src/module_icons.cc
  src/thumbnails.cc)
target_link_libraries(adw_scr_gui PUBLIC ${_scr_gui_libs})
if(TARGET adw_loader)
  target_compile_definitions(adw_scr_gui PRIVATE AD_SCR_HAVE_LOADER=1)
endif()

# Two programs from one source (src/test_hooks.h):
#  * LongAfterDark.scr, the screen saver that is packaged and installed
#    (package.sh stages this one): AD_SCR_TEST_HOOKS=0, so it never reads the
#    smoke tests' AD_SCR_TEST_* levers. One left in an environment can't keep
#    the full-screen saver from ending, stand in for the monitors or the
#    input, or make it write files.
#  * LongAfterDark-test.scr, the same with the levers (AD_SCR_TEST_HOOKS=1):
#    what the smoke tests run. Never staged.
# Only saver.cc and config_dialog.cc read them, so only those are built twice.
function(ad_scr_program target output hooks)
  add_executable(${target} WIN32 src/saver.cc src/config_dialog.cc res/afterdark.rc $<TARGET_OBJECTS:adw_scr_gui>)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output} SUFFIX ".scr")
  target_compile_definitions(${target} PRIVATE AD_SCR_TEST_HOOKS=${hooks})
  # The .rc pulls afterdark.ico from the build dir, resource.h/manifest from
  # res/ and the version (adw_version.h) from the generated dir.
  target_include_directories(${target} PRIVATE res "${CMAKE_CURRENT_BINARY_DIR}" "${AD_GENERATED_DIR}")
  add_dependencies(${target} scr_icon)
  target_link_options(${target} PRIVATE -municode)
  target_link_libraries(${target} PRIVATE ${_scr_gui_libs})
endfunction()
set_source_files_properties(res/afterdark.rc PROPERTIES
  OBJECT_DEPENDS "${AD_SCR_ICON};${CMAKE_CURRENT_SOURCE_DIR}/res/afterdark.manifest;${CMAKE_CURRENT_SOURCE_DIR}/res/resource.h;${AD_GENERATED_DIR}/adw_version.h")
# The .rc is UTF-8 (em dashes, ellipses); llvm-windres drops the
# code_page pragma in its preprocessing pass, so say it on the command line.
set_property(SOURCE res/afterdark.rc APPEND PROPERTY COMPILE_OPTIONS --codepage=65001)
ad_scr_program(LongAfterDark LongAfterDark 0)
ad_scr_program(LongAfterDarkTest LongAfterDark-test 1)

# ---- tests -----------------------------------------------------------------------
add_executable(scr_fakehost tests/fakehost.cc)
set_target_properties(scr_fakehost PROPERTIES OUTPUT_NAME fakehost)
target_include_directories(scr_fakehost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../host/core/include)
target_link_libraries(scr_fakehost PRIVATE user32)
# Stands in for adimport.exe behind the dialog's Import… button; a console
# program like the real one.
add_executable(scr_fakeimport tests/fakeimport.cc)
set_target_properties(scr_fakeimport PROPERTIES OUTPUT_NAME fakeimport)
target_link_options(scr_fakeimport PRIVATE -municode)

add_executable(scr_unit tests/unit_tests.cc)
# adw_ui for the caption fonts: the strip's captions are measured in the real face.
target_link_libraries(scr_unit PRIVATE adw_scr adw_ui)
target_compile_definitions(scr_unit PRIVATE
  AD_SCR_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")

# The tests synthesize their cover tiles (adw_ui's save_png_bgr): never real art.
add_executable(scr_smoke tests/smoke_test.cc)
target_include_directories(scr_smoke PRIVATE res)
target_link_libraries(scr_smoke PRIVATE adw_scr adw_ui user32 gdi32 comctl32 windowscodecs ole32 oleaut32 dwmapi version)
target_link_options(scr_smoke PRIVATE -municode)

foreach(_t args parser settings catalog geometry rotation convert env layout input seed releases paths sound present)
  add_test(NAME scr_unit_${_t} COMMAND scr_unit ${_t})
endforeach()
# Starts fakeimport.exe the way the dialog starts adimport.exe.
add_test(NAME scr_unit_dialog COMMAND scr_unit dialog --fakeimport $<TARGET_FILE:scr_fakeimport>)
# The settings dialog's presentation: About tidying, choices, summaries, the
# window layout at several DPIs, and the lane probe (against fakehost.exe).
add_test(NAME scr_unit_ui COMMAND scr_unit ui --fakehost $<TARGET_FILE:scr_fakehost>)

# Smoke tests run the real LongAfterDark.scr against fakehost.exe and a
# fixture asset tree built under the test's work dir. Every process they
# start resolves %LOCALAPPDATA% to a scratch folder (AD_LOCALAPPDATA), so
# none of them can touch the user's real data folder. They open real (briefly
# full-screen) windows; label "gui" lets a headless run skip them
# (ctest -LE gui), and AD_SCR_SKIP_GUI_TESTS=1 makes them report SKIP.
# The smoke tests run LongAfterDark-test.scr (the test levers); `resources`
# looks at the LongAfterDark.scr that ships, and checks it has none of them.
set(_smoke_args
  --scr "$<TARGET_FILE:LongAfterDarkTest>"
  --fakehost "$<TARGET_FILE:scr_fakehost>"
  --fakeimport "$<TARGET_FILE:scr_fakeimport>"
  --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  --work "${CMAKE_CURRENT_BINARY_DIR}/smoke")
add_test(NAME scr_host_stream COMMAND scr_smoke host-stream ${_smoke_args})
# What Windows reads from the file (no window): the name Screen Saver Settings
# lists (string 1), the version resource and the manifest.
add_test(NAME scr_resources COMMAND scr_smoke resources ${_smoke_args} --shipped "$<TARGET_FILE:LongAfterDark>")
foreach(_t run preview preview-parent-gone not-imported host-missing respawn stall rotate rotate-lead
           start-failure display-off p6-garbage primary-only config config-lead config-live config-classic
           config-thumbs list-top import preview-settings display-change
           input-play input-alt input-wake input-rotate input-monitors input-display-change seed seed-screens
           config-buttons
           config-collections config-cover rotate-collections config-details data-root
           sound sound-wake config-sound present config-abi rotate-abi rotate-abi-wait screen-abi screen-field numlock
           config-credit config-twelve)
  add_test(NAME scr_smoke_${_t} COMMAND scr_smoke ${_t} ${_smoke_args})
  set_tests_properties(scr_smoke_${_t} PROPERTIES LABELS gui SKIP_RETURN_CODE 77 TIMEOUT 90)
endforeach()
# Opt-in (AD_E2E=1, AD_E2E_ASSETS=<assets root>): the real adhostwin on real
# modules (INTERACTION.md acceptance C7). Skipped (77) otherwise.
if(TARGET adhostwin)
  foreach(_t e2e-rodger e2e-dosshell e2e-sound)
    add_test(NAME scr_smoke_${_t} COMMAND scr_smoke ${_t} ${_smoke_args} --realhost "$<TARGET_FILE:adhostwin>")
    set_tests_properties(scr_smoke_${_t} PROPERTIES LABELS "gui;e2e" SKIP_RETURN_CODE 77 TIMEOUT 300)
  endforeach()
endif()
# 48 off-screen renders of the dialog.
set_tests_properties(scr_smoke_list-top PROPERTIES TIMEOUT 300)
# Six off-screen renders, two of them waiting on background thumbnails.
set_tests_properties(scr_smoke_config-abi PROPERTIES TIMEOUT 180)
# Four /s runs, a /p and the dialog with its thumbnails and a Preview (a fifth /s).
set_tests_properties(scr_smoke_screen-abi PROPERTIES TIMEOUT 180)
# The same for a module with a "screen" of its own (five /s runs, a /p, and the dialog twice:
# its thumbnails and Preview, then an import that adds the screen).
set_tests_properties(scr_smoke_screen-field PROPERTIES TIMEOUT 180)
# Four scripted /s runs of a few seconds each.
set_tests_properties(scr_smoke_numlock PROPERTIES TIMEOUT 180)
# The dialog driven by control ID twice (the second time into a Preview, a
# /s), then seven off-screen renders.
set_tests_properties(scr_smoke_config-credit PROPERTIES TIMEOUT 180)
# Seven off-screen renders with twelve releases, the dialog driven by control
# ID, then a /s rotation.
set_tests_properties(scr_smoke_config-twelve PROPERTIES TIMEOUT 180)
# Two /s runs, and a scanner may hold a fresh capture for a while.
set_tests_properties(scr_smoke_present PROPERTIES TIMEOUT 180)
set_tests_properties(scr_host_stream PROPERTIES TIMEOUT 60)

# Sound (docs/AUDIO.md §9) is on by default and the GUI smoke tests
# run "/s": every scr test runs with AD_SCR_SOUND=0, which turns sound off
# for every host whatever settings.ini says. The tests about sound itself
# (scr_unit_sound, scr_smoke_sound, sound-wake, config-sound) take it out of
# the environment of the processes they start, and those run fakehost, which
# makes no sound. The one opt-in exception with a real host is
# scr_smoke_e2e-sound (AD_E2E=1 AD_E2E_ASSETS=<root> AD_SCR_SOUND_E2E=1): it
# runs the real adhostwin with ADAUDIOLIVE=0, so it captures to a WAV and
# never opens the audio device either.
get_property(_scr_tests DIRECTORY PROPERTY TESTS)
foreach(_t IN LISTS _scr_tests)
  set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT "AD_SCR_SOUND=0")
endforeach()
