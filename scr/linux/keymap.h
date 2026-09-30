// X keys to Windows virtual-key codes (the KEY lines of host/core/README.md
// carry Windows VKs, 0..255).
#pragma once

#include <X11/Xlib.h>

namespace lad {

// The VK for one keysym, 0 when it has none: the function keys, the
// editing and cursor keys, the modifiers and locks, the keypad (VK_NUMPAD0-9
// and the operators, or the editing keys the keypad gives with Num Lock
// off), and the US-layout punctuation (VK_OEM_*).
int vk_for_keysym(KeySym sym);

// The VK a key event has, as Windows gives one:
//  * letters and digits by whichever of the key's keysyms is one (the
//    event's group first, then the others; levels 0 and 1), so the top row
//    of an AZERTY keyboard gives VK_1..VK_0 and a Cyrillic group still gives
//    VK_A..VK_Z, as Windows' layouts do;
//  * the keypad by the keysym the Num Lock state selects (KP_1 is
//    VK_NUMPAD1 with Num Lock on, KP_End is VK_END with it off);
//  * everything else by its unshifted keysym.
// `xkb` says whether the Xkb extension is in use (XkbKeycodeToKeysym).
int vk_for_key(Display* d, XKeyEvent* ev, bool xkb);

// A Windows system key (WM_SYSKEYDOWN/UP): Alt or F10 itself, or any key
// while Alt is held. `sym0` is the key's unshifted keysym.
bool is_system_key(KeySym sym0, unsigned state);

}  // namespace lad
