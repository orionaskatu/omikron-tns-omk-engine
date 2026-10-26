// SPDX-License-Identifier: GPL-3.0-or-later
// OMK's own options pages (`ui/omkoptions.h`) and their texts (`ui/omktext.h`)
// as compiled in - the probe behind `verify.py: omk lang`. Prints every text id
// the pages can ask for (`NEED`), every id the compiled French section carries
// (`HAVE`), and each NEED's French and English text (`FR` / `EN`).
#include "ui/omkoptions.h"
#include "ui/omktext.h"

#include <cstdio>

int main() {
    omk::OmkText fr, en;
    fr.load("fr");
    en.load("en");
    const omk::OmkOptions pages(true);
    for (const auto& id : pages.textIds()) {
        std::printf("NEED %s\n", id.c_str());
        std::printf("FR %s = %s\n", id.c_str(), fr(id).c_str());
        std::printf("EN %s = %s\n", id.c_str(), en(id).c_str());
    }
    for (const auto& id : fr.ids()) std::printf("HAVE %s\n", id.c_str());
    return 0;
}
