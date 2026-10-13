// SPDX-License-Identifier: GPL-3.0-or-later
// `PlayState::step` - one turn of the viewer's loop, which is its six phases
// in order (`playframe_<phase>.cpp`, `todo/play-split.md`). Each phase is a
// stretch of what was `main`'s `for (;;)` body, moved byte for byte inside
// `do { ... } while (false)`, so a `break` in it still ends the loop.
#include "playframe.h"

int PlayState::step() {
    int r = phase::Continue;
    // each phase a profiler zone (`--profile`, todo/debug-tools.md)
    { OMK_ZONE("input"); if ((r = phaseInput()) != phase::Continue) return r; }
    { OMK_ZONE("control"); if ((r = phaseControl()) != phase::Continue) return r; }
    { OMK_ZONE("modes"); if ((r = phaseModes()) != phase::Continue) return r; }
    { OMK_ZONE("world"); if ((r = phaseWorld()) != phase::Continue) return r; }
    { OMK_ZONE("screens"); if ((r = phaseScreens()) != phase::Continue) return r; }
    { OMK_ZONE("present"); if ((r = phasePresent()) != phase::Continue) return r; }
    return phase::Continue;
}
