# adw_ui — the shared Windows 11 look

`adw_ui` is the static library that gives Long After Dark's two windowed
programs, `LongAfterDark.scr`'s settings dialog and `adimport`'s windows,
one look (`docs/COVERS.md` §3): the Fluent
palettes for light and dark app mode and high contrast, the user's accent
colour, the Segoe UI Variable type ramp at each DPI, the custom-drawn stock
controls, the header band, cover drawing and off-screen capture. Namespace
`adw::ui`; include `adw/ui/*.h`.

| Header | What is in it |
|---|---|
| `adw/ui/theme.h` | `Palette`, `Accent`, `ThemeMode`, `Fonts`, `Theme`; `set_test_hc_scheme` (tests and screenshots); window chrome (`apply_window_chrome`, `theme_native_control`); GDI+ drawing (rounded fills and strokes, control bodies, focus rings, fades, gradients, glows); the night sky (`draw_sparkle`, `draw_crescent`, `draw_flying_toaster`); text. Additions: `is_theme_change`, `allow_dark_menus`, `paint_card`, `paint_header`, and `draw_app_mark`; at integration `fade_in_left` and `fill_round` / `stroke_ellipse` with an alpha (the settings dialog's cover strip; its edge fades have since given way to whole covers and chevrons, so `fade_in_left` has no caller now) |
| `adw/ui/widgets.h` | Stock controls painted in the Windows 11 style: push buttons in `ButtonRole`s (standard, accent, subtle, segments, checkbox, and the new `card`), trackbars, owner-drawn dropdowns, badges, the thin overlay scroll bar. Additions: `subclass_progress` (a Fluent bar over `msctls_progress32`), `set_card_cover`/`CardCover` and `card_height`; `set_accessible_name` (the name screen readers give a control whose visible text repeats, through `IAccPropServices`); `init_slider`/`SliderSpec` (the settings dialog's Volume, `docs/AUDIO.md` §9: a stock trackbar set up whole, range, page, position, the subclass and its accessible name, with Windows 11 keys: Right and Up raise it, Left and Down lower it, Page Up/Down by the page, Home/End; drawn by `custom_draw_trackbar` in light, dark and high contrast, greyed when disabled) |
| `adw/ui/capture.h` | `park_offscreen`, `capture_window_png`, `save_png_bgr` (the screenshot hooks). Addition: `settle_for_capture` |
| `adw/ui/image.h` | `Image` (premultiplied BGRA with a cache of scaled copies), `load_image` (WIC), `draw_image` (high-quality bicubic, rounded corners, opacity), `draw_cover`, `draw_generated_cover`. Additions: `image_from_bgra`, `generated_cover_image` |

The first three are the scr's former `ui_theme`, `ui_widgets` and
`ui_capture`, moved with the same names, signatures and behaviour. The API
listed in COVERS.md §3.2 is frozen: it may grow, never change.

Notes for users of the library:

* **GDI+** must be started (`gdiplus_startup()`) before drawing, and
  **COM** initialized on the thread that calls `load_image`.
* The subclass API behind the trackbar, dropdown, overlay scroll bar and
  progress bar is exported by name only from **Common Controls 6**: an
  executable that uses them needs the comctl6 manifest (both apps and the
  tests have one).
* A window parked with `park_offscreen` should go through
  `settle_for_capture` before `capture_window_png`: a cloaked window that
  never rendered through a plain `PrintWindow` captures black.
* `capture_window_png` reads the window's picture from DWM
  (`PrintWindow(PW_RENDERFULLCONTENT)`), and DWM's copy of a cloaked window
  can lag what it drew, most of all on a busy machine: a capture once came
  back as the bare background with its control missing. So it grabs,
  has the window draw itself again and DWM compose, and grabs again, until
  two grabs agree (at most about a second; then the latest). No messages
  are pumped meanwhile, so nothing animates between grabs. A test that
  knows what must show can still check for it and capture again (`ui.capture`
  and `ui.slider` do, for up to 3 s).
* `draw_generated_cover` is theme-independent, like box art: the night
  gradient `#262B4F → #12152A`, the crescent at (0.72 w, 0.22 h), five
  sparkles and the title in white Segoe UI Variable Display Semibold,
  left at 0.1 w and bottom at 0.9 h, at most three lines, 0.14 w tall and
  shrunk until it fits. `draw_cover` caches generated covers by size and
  title.
* `allow_dark_menus` uses uxtheme's undocumented ordinals 135 and 136 and
  does nothing where they are missing.
* **Screenshots under a real contrast theme.** Forcing high contrast
  (`ThemeMode::high_contrast`, both apps' `theme=hc`) takes the system
  colours of whatever theme the machine has, which outside high contrast
  are the ordinary ones. `set_test_hc_scheme(L"nightsky")` (or `aquatic`,
  `desert`, `dusk`; `nullptr` to undo) stands the colours of one of Windows
  11's contrast themes (approximately their defaults) in for
  `GetSysColor`'s in `high_contrast_palette`, for tests and screenshots
  only. The library reads no environment variable for it (it links into the
  programs users run): `LongAfterDark-test.scr`'s screenshot hook calls it
  for `AD_UI_TEST_HC_SCHEME=nightsky|aquatic|desert|dusk`.

## Tests

* `ui.theme`: the palettes (and `set_test_hc_scheme`'s Night sky and
  Desert, an unknown name changing nothing, the environment variable alone
  changing nothing), the theme-change filter, fonts at 100/150/200%,
  `paint_card` and `paint_header` drawn into memory in each mode (no stars
  under high contrast), `card_height`.
* `ui.image`: `image_from_bgra`, 1:1 and scaled drawing and the scale cache,
  a 13× prefiltered shrink, opacity and rounded corners, `load_image` on a
  PNG, a missing file and junk, the generated cover's gradient, moon and
  title placement at several sizes, and `draw_cover` in each mode.
* `ui.capture`: a parked, cloaked, never-activated window with a card
  button and a half-full progress bar captured to PNG in light, dark and
  high contrast (again until the bar shows, for up to 3 s), and its pixels
  checked.
* `ui.slider`: `init_slider` on a trackbar in a parked, cloaked window in
  light, dark and high contrast: its range, page and line; its name and
  value as MSAA (and the UIA proxy) report them; every key (Right, Up, Left,
  Down, Page Up, Page Down, End, Home, and at the ends) moving it as a
  Windows 11 slider does, each reported to the parent as one `WM_HSCROLL`
  with its code and a `TB_ENDTRACK` on release; a key sent to it while
  disabled moving nothing; and its picture, enabled and disabled (captured
  again until the rail shows on its centre line, for up to 3 s): the
  accent (or its disabled tone) up to the thumb, the rail after it, the
  thumb's ring and core.
