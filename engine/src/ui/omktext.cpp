// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/omktext.h"

#include "platform/datafs.h"
#include "platform/options.h"
#include "ui/options.h"

namespace omk {

bool OmkText::load(const std::string& language, const std::string& overridePath) {
    language_ = language == "en" ? "en" : "fr";
    fr_.clear();
    chosen_.clear();
    // the compiled file, then the reader's copy over it, section by section
    std::vector<OptionsFile> files{parseOptionsText(omkMenuStrings())};
    bool over = false;
    if (!overridePath.empty() && fileSize(overridePath) >= 0) {
        files.push_back(loadOptionsFile(overridePath));
        over = true;
    }
    for (const auto& f : files) {
        if (const auto s = f.sections.find("fr"); s != f.sections.end())
            for (const auto& [id, text] : s->second) fr_[id] = utf8ToLatin1(text);
        if (const auto s = f.sections.find(language_); s != f.sections.end())
            for (const auto& [id, text] : s->second) chosen_[id] = utf8ToLatin1(text);
    }
    return over;
}

const std::string& OmkText::operator()(const std::string& id) const {
    if (const auto t = chosen_.find(id); t != chosen_.end()) return t->second;
    if (const auto t = fr_.find(id); t != fr_.end()) return t->second;
    // a gap is SHOWN: the id itself, kept so the reference stays valid
    static std::map<std::string, std::string> missing;
    return missing.emplace(id, id).first->second;
}

bool OmkText::has(const std::string& id) const {
    return chosen_.count(id) != 0 || fr_.count(id) != 0;
}

std::vector<std::string> OmkText::ids() const {
    std::vector<std::string> out;
    for (const auto& kv : fr_) out.push_back(kv.first);
    return out;
}

}  // namespace omk
