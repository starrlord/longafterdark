# adw_import + adimport.exe — the Windows asset importer (DESIGN.md §6,
# PACKAGES.md) and the catalog generator (§6a), for the sixteen releases of its
# registry (packages.h). The library depends on Win32 (WinHTTP, CNG, shell),
# on adw_loader, whose PE/NE readers the catalog uses, and on zlib (raw
# inflate for the AD 3.x installers' PKZIP archives, CRC-32 for them and for
# Presage's ARJ archives, and for 7z archives); its own readers expand the ARJ,
# SZDD and KWAJ files and the InstallShield 2 compressed libraries (PKWARE DCL
# implode) of the floppy-based installers, and 7z archives (LZMA, LZMA2).
# phosg is used by the tests alone, to parse import.json and the catalog
# independently of the code writing them.
enable_language(RC)

# The catalog reads the modules with adw::loader. host/loader precedes the
# importer in the top-level component list; built on its own (AD_COMPONENTS
# without host/loader) the importer pulls it in.
if(NOT TARGET adw_loader)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../host/loader ${CMAKE_BINARY_DIR}/host/loader)
endif()

# zlib is already in third_party/win/local (phosg depends on it).
find_package(ZLIB CONFIG REQUIRED COMPONENTS static)

add_library(adw_import STATIC
  md5.cc
  minijson.cc
  iso9660.cc
  fat.cc
  zip.cc
  sevenzip.cc
  arj.cc
  szdd.cc
  kwaj.cc
  isz.cc
  source.cc
  packages.cc
  download.cc
  catalog.cc
  install.cc
  cover_image.cc
  covers.cc
  importer.cc)
target_include_directories(adw_import PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# adw/core/data_root.h: where the data folder is, shared with adhostwin and the
# .scr (header-only, kernel32).
set(ADW_CORE_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../host/core/include)
target_include_directories(adw_import PRIVATE ${ADW_CORE_INCLUDE})
# uuid: FOLDERID_LocalAppData lives in libuuid, so every consumer needs it.
# windowscodecs: the covers' pictures are decoded and encoded with WIC (cover_image.h).
target_link_libraries(adw_import PUBLIC adw_loader ZLIB::ZLIBSTATIC winhttp bcrypt shell32 ole32 uuid windowscodecs)

# The importer's windows (COVERS.md §4, §8.2) are gui/'s adw_import_gui:
# adimport.cc hands it a gui::Request (gui/gui.h) and returns its exit code.
add_subdirectory(gui)
# The file's icon: the moon tile LongAfterDark.scr's icon is, drawn by the
# same build-time recipe (scr/res/gen_icon.cc, our own artwork; no binary art
# in the repository), built here too so the importer does not depend on the
# scr component being in the build.
add_executable(adimport_gen_icon ${CMAKE_CURRENT_SOURCE_DIR}/../scr/res/gen_icon.cc)
set(AD_IMPORT_ICON "${CMAKE_CURRENT_BINARY_DIR}/adimport.ico")
add_custom_command(OUTPUT "${AD_IMPORT_ICON}"
  COMMAND adimport_gen_icon "${AD_IMPORT_ICON}"
  DEPENDS adimport_gen_icon
  COMMENT "Drawing adimport.ico")
add_custom_target(adimport_icon DEPENDS "${AD_IMPORT_ICON}")
add_executable(adimport adimport.cc adimport.rc)
# The .rc takes adimport.ico from the build dir and adw_version.h from the
# generated headers (the top-level CMakeLists.txt).
target_include_directories(adimport PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${AD_GENERATED_DIR}")
set_source_files_properties(adimport.rc PROPERTIES
  OBJECT_DEPENDS "${AD_IMPORT_ICON};${CMAKE_CURRENT_SOURCE_DIR}/adimport.manifest;${AD_GENERATED_DIR}/adw_version.h")
add_dependencies(adimport adimport_icon)
target_link_libraries(adimport PRIVATE adw_import adw_import_gui)

# ---- tests ------------------------------------------------------------------
# All offline except import.e2e, which downloads the real ~400 MB image and
# only runs with AD_E2E=1, import.download_real, which fetches every
# package's Internet Archive copy (or reuses a verified local copy) and also
# only runs with AD_E2E=1, import.covers_real, which fetches every registry
# cover (AD_E2E=1), and import.pkg_real, which imports the real sources of the
# thirteen packages besides Deluxe and only runs with AD_E2E_PKG=1 (exit 77 =
# skipped otherwise).
function(adw_import_test name)
  add_executable(test_import_${name} tests/test_${name}.cc)
  # test_util.h finds the installed data read-only and sandboxes the data
  # folder (AD_LOCALAPPDATA) with data_root.h.
  target_include_directories(test_import_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${ADW_CORE_INCLUDE})
  target_link_libraries(test_import_${name} PRIVATE adw_import phosg::phosg ws2_32)
endfunction()

adw_import_test(md5)
adw_import_test(iso)
adw_import_test(import)
adw_import_test(download)
adw_import_test(cli)
adw_import_test(e2e)
adw_import_test(catalog)
adw_import_test(zip)
adw_import_test(sevenzip)
adw_import_test(arj)
adw_import_test(szdd)
adw_import_test(kwaj)
adw_import_test(isz)
adw_import_test(fat)
adw_import_test(packages)
adw_import_test(pkg_real)
adw_import_test(pkg_download)
adw_import_test(download_real)
adw_import_test(covers)
adw_import_test(covers_real)

set(_scratch ${CMAKE_CURRENT_BINARY_DIR}/test-scratch)
add_test(NAME import.md5 COMMAND test_import_md5 ${_scratch}/md5)
add_test(NAME import.iso COMMAND test_import_iso ${_scratch}/iso)
add_test(NAME import.import COMMAND test_import_import ${_scratch}/import)
add_test(NAME import.download COMMAND test_import_download ${_scratch}/download)
add_test(NAME import.cli COMMAND test_import_cli $<TARGET_FILE:adimport> ${_scratch}/cli)
add_test(NAME import.catalog COMMAND test_import_catalog unit ${_scratch}/catalog)
add_test(NAME import.zip COMMAND test_import_zip ${_scratch}/zip)
# 7z archives (PACKAGES.md §8.10): 7-Zip's own archives of made-up files
# (LZMA, LZMA2, Copy; header packed and plain; solid and not), kept as bytes,
# Copy archives written by the test, the refusals, every truncation and
# every single-bit flip of the small ones, and 7z sources (install files,
# DISK<n> folders, floppy images).
add_test(NAME import.sevenzip COMMAND test_import_sevenzip ${_scratch}/sevenzip)
# The user's 7z of Screen Antics: Johnny Castaway's floppy image (found by md5
# in AD_SOURCE_ISO_DIR or <repo>/source_iso): skipped (77) unless
# AD_E2E_PKG=1 and it is present.
add_test(NAME import.sevenzip_real COMMAND test_import_sevenzip real ${_scratch}/sevenzip-real
         ${AD_REPO_ROOT}/source_iso)
set_tests_properties(import.sevenzip_real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# Presage's ARJ archives and COMPRESS's SZDD files (Star Wars Screen
# Entertainment), and the Setup Toolkit's KWAJ files (Star Trek: The Screen
# Saver), on synthetic archives, the research encoders' fixed vectors and
# streams written token by token (nothing is compressed).
add_test(NAME import.arj COMMAND test_import_arj ${_scratch}/arj)
add_test(NAME import.szdd COMMAND test_import_szdd ${_scratch}/szdd)
add_test(NAME import.kwaj COMMAND test_import_kwaj ${_scratch}/kwaj)
# InstallShield 2 compressed libraries and PKWARE DCL explode (Marvel Comics
# Screen Posters, Snoopy's Screen Savers): InstallShield's own ICOMP vectors
# of made-up data, streams and split sets written token by token, every
# container rule broken, every single-bit flip of the small vectors.
add_test(NAME import.isz COMMAND test_import_isz ${_scratch}/isz)
# The same reader on the libraries of the user's Marvel and Snoopy ZIPs (found
# by md5 in AD_SOURCE_ISO_DIR or <repo>/source_iso, read as DISK<n> sources),
# every member against the InstallShield survey's md5s: skipped (77) unless
# AD_E2E_PKG=1 and one of them is present.
add_test(NAME import.isz_real COMMAND test_import_isz real ${_scratch}/isz-real ${AD_REPO_ROOT}/source_iso)
set_tests_properties(import.isz_real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
add_test(NAME import.fat COMMAND test_import_fat ${_scratch}/fat)
add_test(NAME import.packages COMMAND test_import_packages $<TARGET_FILE:adimport> ${_scratch}/packages)
add_test(NAME import.pkg_download COMMAND test_import_pkg_download $<TARGET_FILE:adimport> ${_scratch}/pkg-download)
# The box covers (COVERS.md §2.14), offline: synthetic pictures, synthetic
# registries and the loopback server.
add_test(NAME import.covers COMMAND test_import_covers $<TARGET_FILE:adimport> ${_scratch}/covers)
# Every registry cover download fetched and rendered, and (AD_E2E_PKG=1) the
# disc art of the real images; writes covers-sheet.png into the build dir.
# Skipped (77) unless AD_E2E=1.
add_test(NAME import.covers_real COMMAND test_import_covers_real $<TARGET_FILE:adimport> ${_scratch}/covers-real
         ${AD_REPO_ROOT}/source_iso ${CMAKE_BINARY_DIR}/covers-sheet.png)
# The C++ generator against the prototype's output over the real corpus
# (research/win/make_catalog.py -> research/win/catalog-win.json, gitignored):
# skipped (77) unless both the imported assets and that file are present.
add_test(NAME import.catalog_real COMMAND test_import_catalog real ${_scratch}/catalog-real
         $<TARGET_FILE:adimport> ${AD_REPO_ROOT}/research/win/catalog-win.json)
add_test(NAME import.e2e COMMAND test_import_e2e $<TARGET_FILE:adimport> ${CMAKE_CURRENT_BINARY_DIR}/e2e-assets)
# Every package's real source (AD_SOURCE_ISO_DIR, default <repo>/source_iso,
# and the folders directly in it; or a ';'-separated list): the disc and
# floppy images and the known ZIPs of install files, identified by md5; the
# islib packages' tables checked against their disks and every form of them;
# skipped (77) unless AD_E2E_PKG=1 and they are all present.
add_test(NAME import.pkg_real COMMAND test_import_pkg_real $<TARGET_FILE:adimport> ${_scratch}/pkg-real
         ${AD_REPO_ROOT}/source_iso)
# Every package's Internet Archive copy into a scratch downloads folder and a
# scratch root under the build tree; local copies with the published md5
# (AD_E2E_LOCAL_DIRS; default <repo>/source_iso, the installed data folder's
# downloads - %LOCALAPPDATA%/LongAfterDark, found read-only - and its verify
# folder) are linked or copied in instead of being downloaded again. Skipped
# (77) unless AD_E2E=1.
add_test(NAME import.download_real COMMAND test_import_download_real $<TARGET_FILE:adimport>
         ${_scratch}/download-real ${AD_REPO_ROOT}/source_iso)
set_tests_properties(import.download_real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 7200)
set_tests_properties(import.e2e PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 7200)
set_tests_properties(import.pkg_real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)
set_tests_properties(import.catalog_real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
set_tests_properties(import.download PROPERTIES TIMEOUT 300)
set_tests_properties(import.packages PROPERTIES TIMEOUT 600)
set_tests_properties(import.pkg_download PROPERTIES TIMEOUT 600)
set_tests_properties(import.covers PROPERTIES TIMEOUT 600)
set_tests_properties(import.covers_real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)
