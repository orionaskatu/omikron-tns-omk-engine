// SPDX-License-Identifier: GPL-3.0-or-later
// Read a config file and say what it holds - the probe behind
// `verify.py: options file`.
#include "platform/options.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    // `--edit FILE section key value [section key value ...]`: the WRITER
    // (`writeOptionsFile`), on FILE in place - `verify.py: options write`
    if (argc >= 2 && std::string(argv[1]) == "--edit") {
        if (argc < 6 || (argc - 3) % 3 != 0) {
            std::fprintf(stderr, "usage: options_probe --edit <file.ini> <section> <key> <value> ...\n");
            return 2;
        }
        std::vector<omk::OptionsEdit> edits;
        for (int i = 3; i + 2 < argc; i += 3) edits.push_back({argv[i], argv[i + 1], argv[i + 2]});
        const bool ok = omk::writeOptionsFile(argv[2], edits);
        std::printf("edit %s %zu: %s\n", argv[2], edits.size(), ok ? "written" : "REFUSED");
        return ok ? 0 : 1;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: options_probe <file.ini> | --edit <file.ini> <section> <key> <value> ...\n");
        return 2;
    }
    const omk::OptionsFile o = omk::loadOptionsFile(argv[1]);
    std::size_t keys = 0;
    for (const auto& s : o.sections) keys += s.second.size();
    std::printf("loaded %d  sections %zu  keys %zu  unknown %zu  known-keys %zu\n",
                o.loaded ? 1 : 0, o.sections.size(), keys, o.unknown.size(),
                omk::preferenceKeys().size());
    for (const auto& s : o.sections)
        for (const auto& kv : s.second)
            std::printf("  %s.%s = %s\n", s.first.c_str(), kv.first.c_str(), kv.second.c_str());
    for (const auto& u : o.unknown) std::printf("  UNKNOWN preferences.%s\n", u.c_str());
    // the port's own table, so a check can compare the SET and not only the
    // count - two tables of 65 different names would otherwise agree
    for (const auto& k : omk::preferenceKeys()) std::printf("KEY %s\n", k.c_str());
    std::printf("clipdistance %d  displaysky %d  displayshadows %d  music %d\n",
                o.integer("Preferences", "clipdistance", -1),
                o.boolean("Preferences", "displaysky", false) ? 1 : 0,
                o.boolean("Preferences", "displayshadows", true) ? 1 : 0,
                o.integer("Preferences", "music", -1));
    return 0;
}
