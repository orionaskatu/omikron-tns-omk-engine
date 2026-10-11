// SPDX-License-Identifier: GPL-3.0-or-later
// WHOLE CACHE LINES WRITTEN WITHOUT BEING READ FIRST - PowerPC's `dcbz`
// (`todo/handoff-classic-mac.md` 3f step 3o, 2026-10-11, for the iBook G3).
//
// A store to a line that is not in the cache makes a G3 READ the line from
// memory before it can write into it, so a pass that writes a frame whole -
// the menu's cloud, a clear - moves every byte twice on a bus that reads ~170
// MB/s. `dcbz` ("data cache block zero") allocates the line in the cache as
// zeros with no read; written over afterwards, the line leaves the cache once,
// as the write. Other CPUs either stream full-line writes themselves (the
// Vita's Cortex-A9, the host's) or have no such instruction, and there these
// are a `memset` and nothing.
//
// The line `dcbz` clears is MEASURED, not assumed (`zeroLineSize`): a G3 or
// a G4 clears 32 bytes, a G5 128 (or 32 in the compatibility mode Mac OS X
// may set) - a wrong guess would zero bytes outside the range.
#pragma once

#include <cstddef>

namespace omk {

// The bytes one `dcbz` clears, measured once on a scratch buffer; 0 where
// there is no `dcbz` (every CPU but PowerPC) or the measurement was refused.
std::size_t zeroLineSize();

// [p, p + n) set to zero: the whole lines by `dcbz`, the partial ones at
// either end by `memset`.
void zeroBytes(void* p, std::size_t n);

// The WHOLE lines inside [p, p + n) zeroed without being read, the partial
// ones at either end left as they are - for a range the caller is about to
// write every byte of. Nothing at all without `dcbz`. Only on ordinary
// (cacheable) memory: never on a card's or a device's.
void claimLines(void* p, std::size_t n);

}  // namespace omk
