// Long After Dark for Linux: the X11 player. It runs one module at a time
// in adhostwin.exe under Wine, presents its frames, and follows the
// front-end rules the Windows saver follows (docs/INTERACTION.md,
// host/core/README.md, docs/DESIGN.md). See app.cc.
#include "app.h"

int main(int argc, char** argv) { return lad::player_main(argc, argv); }
