// SPDX-License-Identifier: GPL-3.0-or-later
// THE FRAME'S MODES PHASE - the pause's sound, the voices, dialogue mode, shoot mode, the quit and the pending load.
// Part of `main`'s loop body, split out by `todo/play-split.md`; see `playframe.h` for what the names below refer to.
#include "playframe.h"

int PlayState::phaseModes() {
    bool done = false;
    do {
        modesSound();
        modesDialogue();
        if (const int part = modesShoot(); part != -1) { if (part == -2) break; return part; }
        if (const int part = modesQuitLoad(); part != -1) { if (part == -2) break; return part; }
        done = true;
    } while (false);
    return done ? -1 : -2;
}
