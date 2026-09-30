#include "keymap.h"

#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

namespace lad {

int vk_for_keysym(KeySym sym) {
  if (sym >= XK_0 && sym <= XK_9) return 0x30 + int(sym - XK_0);
  if (sym >= XK_a && sym <= XK_z) return 0x41 + int(sym - XK_a);
  if (sym >= XK_A && sym <= XK_Z) return 0x41 + int(sym - XK_A);
  if (sym >= XK_KP_0 && sym <= XK_KP_9) return 0x60 + int(sym - XK_KP_0);   // VK_NUMPAD0..9
  if (sym >= XK_F1 && sym <= XK_F24) return 0x70 + int(sym - XK_F1);        // VK_F1..VK_F24
  switch (sym) {
    case XK_BackSpace: return 0x08;
    case XK_Tab:
    case XK_ISO_Left_Tab:
    case XK_KP_Tab: return 0x09;
    case XK_Clear:
    case XK_KP_Begin: return 0x0C;        // VK_CLEAR (keypad 5 with Num Lock off)
    case XK_Return:
    case XK_KP_Enter: return 0x0D;
    case XK_Shift_L:
    case XK_Shift_R: return 0x10;         // VK_SHIFT, as WM_KEYDOWN reports either
    case XK_Control_L:
    case XK_Control_R: return 0x11;       // VK_CONTROL
    case XK_Alt_L:
    case XK_Alt_R:
    case XK_Meta_L:
    case XK_Meta_R: return 0x12;          // VK_MENU
    case XK_Pause:
    case XK_Break: return 0x13;
    case XK_Caps_Lock: return 0x14;
    case XK_Escape: return 0x1B;
    case XK_space:
    case XK_KP_Space: return 0x20;
    case XK_Prior:
    case XK_KP_Prior: return 0x21;        // VK_PRIOR (Page Up)
    case XK_Next:
    case XK_KP_Next: return 0x22;         // VK_NEXT (Page Down)
    case XK_End:
    case XK_KP_End: return 0x23;
    case XK_Home:
    case XK_KP_Home: return 0x24;
    case XK_Left:
    case XK_KP_Left: return 0x25;
    case XK_Up:
    case XK_KP_Up: return 0x26;
    case XK_Right:
    case XK_KP_Right: return 0x27;
    case XK_Down:
    case XK_KP_Down: return 0x28;
    case XK_Select: return 0x29;
    case XK_Execute: return 0x2B;
    case XK_Print: return 0x2C;           // VK_SNAPSHOT
    case XK_Insert:
    case XK_KP_Insert: return 0x2D;
    case XK_Delete:
    case XK_KP_Delete: return 0x2E;
    case XK_Help: return 0x2F;
    case XK_Super_L: return 0x5B;         // VK_LWIN
    case XK_Super_R: return 0x5C;         // VK_RWIN
    case XK_Menu: return 0x5D;            // VK_APPS
    case XK_KP_Multiply: return 0x6A;
    case XK_KP_Add: return 0x6B;
    case XK_KP_Separator: return 0x6C;
    case XK_KP_Subtract: return 0x6D;
    case XK_KP_Decimal: return 0x6E;
    case XK_KP_Divide: return 0x6F;
    case XK_Num_Lock: return 0x90;
    case XK_Scroll_Lock: return 0x91;
    // The US layout's punctuation keys (VK_OEM_*), by either of their keysyms.
    case XK_semicolon:
    case XK_colon: return 0xBA;
    case XK_equal:
    case XK_plus: return 0xBB;
    case XK_comma: return 0xBC;
    case XK_minus:
    case XK_underscore: return 0xBD;
    case XK_period: return 0xBE;
    case XK_slash:
    case XK_question: return 0xBF;
    case XK_grave:
    case XK_asciitilde: return 0xC0;
    case XK_bracketleft:
    case XK_braceleft: return 0xDB;
    case XK_backslash:
    case XK_bar: return 0xDC;
    case XK_bracketright:
    case XK_braceright: return 0xDD;
    case XK_apostrophe:
    case XK_quotedbl: return 0xDE;
    case XK_less:
    case XK_greater: return 0xE2;         // VK_OEM_102, the extra key of ISO keyboards
    default: return 0;
  }
}

namespace {

int alnum_vk(KeySym s) {
  if (s >= XK_a && s <= XK_z) return 0x41 + int(s - XK_a);
  if (s >= XK_A && s <= XK_Z) return 0x41 + int(s - XK_A);
  if (s >= XK_0 && s <= XK_9) return 0x30 + int(s - XK_0);
  return 0;
}

bool keypad(KeySym s) { return s >= XK_KP_Space && s <= XK_KP_Equal; }

}  // namespace

int vk_for_key(Display* d, XKeyEvent* ev, bool xkb) {
  const KeyCode kc = (KeyCode)ev->keycode;
  KeySym sym0 = NoSymbol;
  if (xkb) {
    const int group = XkbGroupForCoreState(ev->state);
    sym0 = XkbKeycodeToKeysym(d, kc, group, 0);
    // Letters and digits: the event's group first, then every other one.
    for (int pass = 0; pass < 2; ++pass) {
      for (int g = 0; g < XkbNumKbdGroups; ++g) {
        if ((pass == 0) != (g == group)) continue;
        for (int level = 0; level < 2; ++level) {
          if (int vk = alnum_vk(XkbKeycodeToKeysym(d, kc, g, level))) return vk;
        }
      }
    }
  } else {
    sym0 = XLookupKeysym(ev, 0);
    for (int index = 0; index < 4; ++index) {
      if (int vk = alnum_vk(XLookupKeysym(ev, index))) return vk;
    }
  }
  // The keypad by what the modifiers (Num Lock) select.
  KeySym looked = NoSymbol;
  char buf[16];
  XLookupString(ev, buf, sizeof(buf), &looked, nullptr);
  if (keypad(looked) || keypad(sym0)) {
    if (int vk = vk_for_keysym(looked)) return vk;
  }
  return vk_for_keysym(sym0);
}

bool is_system_key(KeySym sym0, unsigned state) {
  switch (sym0) {
    case XK_Alt_L:
    case XK_Alt_R:
    case XK_Meta_L:
    case XK_Meta_R:
    case XK_F10:
      return true;
    default:
      return (state & Mod1Mask) != 0;
  }
}

}  // namespace lad
