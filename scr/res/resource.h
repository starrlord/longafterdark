// Resource IDs for LongAfterDark.scr (also included by the settings-dialog
// smoke test, which drives the dialog from outside the process by these IDs).
#pragma once

#ifndef IDC_STATIC
#define IDC_STATIC          (-1)
#endif

// Screen saver conventions: string 1 (IDS_DESCRIPTION in scrnsave.h) is the
// name the Screen Saver list shows, "Long After Dark"; icon 100 is ID_APP.
#define IDS_DESCRIPTION     1
#define IDI_APP             100

#define IDD_SETTINGS        200
#define IDD_PANEL           201

// The settings dialog. Its controls are listed in afterdark.rc in tab order;
// config_dialog.cc lays them out (ui_model.h: layout_window).
#define IDC_MODULE_LIST     1001
#define IDC_CHECK_ALL       1002   // "Select all" (Random)
#define IDC_CHECK_NONE      1003   // "Clear" (Random)
#define IDC_MODE_SINGLE     1004
#define IDC_MODE_RANDOM     1005
#define IDC_DURATION_LABEL  1006
#define IDC_DURATION        1007   // dropdown of DurationMin choices (item data = minutes)
#define IDC_SCALE           1012
#define IDC_MONITORS        1013
#define IDC_ASSETS_STATUS   1014
#define IDC_IMPORT          1015
#define IDC_PREVIEW         1016
#define IDC_PANEL           1017
#define IDC_MODULES_LABEL   1018
#define IDC_MODULES_COUNT   1019
#define IDC_ROTATION_SUMMARY 1020
#define IDC_LIVE_PREVIEW    1021
#define IDC_MODULE_TITLE    1022
#define IDC_MODULE_BADGE    1023   // the release's short title, then "Coming soon" / "File missing"
#define IDC_ABOUT           1024
#define IDC_CREDITS         1025
#define IDC_SCALE_LABEL     1026
#define IDC_MONITORS_LABEL  1027
#define IDC_WELCOME_IMPORT  1028   // "Import a release…" in the not-imported welcome
// The box-cover strip (COVERS.md §1.5): a container (first in the tab order)
// holding one toggle button per installed release, IDC_COVER_TILE_BASE +
// its index in the catalog's packages[] (the check state is the selection),
// and two chevrons that scroll the row. Beside it the status line (a polite
// live region) and the "Show all" link, or, while a release still shows a
// generated cover, the "Get the covers" link (adimport --gui --refresh-covers).
#define IDC_COVER_STRIP     1030
#define IDC_STRIP_SHOW_ALL  1031
#define IDC_STRIP_STATUS    1032
#define IDC_STRIP_PREV      1033   // (in the container) the left chevron
#define IDC_STRIP_NEXT      1034   // (in the container) the right chevron
#define IDC_STRIP_GET_COVERS 1035
// Sound (AUDIO.md §9), in the options card under Resolution and Monitors:
// "Primary monitor" / "Off" (Settings::sound), the Volume slider 0..100 with
// its readout, and the note on where sound plays.
#define IDC_SOUND_LABEL     1036
#define IDC_SOUND           1037
#define IDC_VOLUME_LABEL    1038
#define IDC_VOLUME          1039   // msctls_trackbar32, 0..100, page 10; accessible name "Volume"
#define IDC_VOLUME_VALUE    1040
#define IDC_SOUND_NOTE      1041
// The footer's credit (ui_model.h: layout_footer_credit): "Made With Love by
// StarrLord", one link that opens the project's page in the browser.
#define IDC_FOOTER_CREDIT   1042
// "A different module on each monitor" (Settings::different_per_monitor),
// under "Change module every" in Random on a PC with several monitors
// (ui_model.h: per_monitor_choice).
#define IDC_PER_MONITOR     1043
// "Stretch to fit the screen" (Settings::stretch_to_fit), across the options
// card under Resolution and Monitors (ui_model.h: WindowLayout::stretch).
#define IDC_STRETCH         1044
// The looks (looks.h), a row of two dropdowns under "Stretch to fit": "Look"
// (Settings::look: Sharp pixels / CRT monitor / Curved CRT monitor / Smooth,
// and "Shader preset: <file>" while ShaderPreset names one) and "Bars"
// (Settings::ambient_bars: Black / Ambient glow). ui_model.h: look_choices,
// WindowLayout::look and ::bars.
#define IDC_LOOK_LABEL      1045
#define IDC_LOOK            1046
#define IDC_BARS_LABEL      1047
#define IDC_BARS            1048
#define IDC_COVER_TILE_BASE 3000

// Runtime-built controls in the module-settings panel: one block of IDs per
// catalog control, IDC_PANEL_BASE + slot * IDC_PANEL_STRIDE + part. A module
// button is a read-only row: its name (LABEL) and a disabled note (INPUT).
#define IDC_PANEL_DEFAULTS  1901   // "Restore defaults" (in the dialog, under the panel)
#define IDC_PANEL_EMPTY     1902
#define IDC_PANEL_BASE      2000
#define IDC_PANEL_STRIDE    4
#define IDC_PART_LABEL      0
#define IDC_PART_INPUT      1
#define IDC_PART_VALUE      2
