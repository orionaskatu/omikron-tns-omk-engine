// SPDX-License-Identifier: GPL-3.0-or-later
// THE TEXTS OF OMK'S OWN MENU ROWS (`todo/graphics-menu.md` A2b).
//
// The game's rows read the installed copy's `IAM\Options`, whose language
// neither the data nor the exe names. OMK's rows carry their own:
// `engine/lang/omk-menu.ini`, a `[fr]` and an `[en]` section of the same ids,
// compiled in (`omktext_data.cpp`, `tools/omklang.py`) so every port has them,
// and `[OMK] language` picks one - French by default, as the shipped data is.
//
// A copy of the file beside the config (`omk-menu.ini`) is read over the
// compiled one, id by id: a reader can mend a text, or add one, without a
// build. A text missing in the chosen language falls back to French; missing
// in both, the id itself is shown, so a gap is seen rather than drawn empty.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace omk {

// `engine/lang/omk-menu.ini` as compiled in, comments dropped
const char* omkMenuStrings();

class OmkText {
public:
    // the compiled strings, then `overridePath`'s when it exists, in `language`
    // ("fr" or "en"; anything else is French). -> whether the override was read.
    bool load(const std::string& language, const std::string& overridePath = {});
    const std::string& language() const { return language_; }
    // the text of `id`, in the fonts' 8-bit codes (`utf8ToLatin1`)
    const std::string& operator()(const std::string& id) const;
    bool has(const std::string& id) const;
    // every id the French section carries - the reference set
    std::vector<std::string> ids() const;

private:
    std::string language_ = "fr";
    std::map<std::string, std::string> fr_, chosen_;
};

}  // namespace omk
