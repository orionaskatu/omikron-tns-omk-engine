// SPDX-License-Identifier: GPL-3.0-or-later
// MACHINE-READABLE EVENT LINES (`todo/code-readability-audit.md` §6).
//
// The checks read the viewer's log, and the log is English: rewording a
// `printf` turns a check red with no help from the compiler, and nothing at the
// `printf` says which check reads it. An EVENT is the same fact as a record -
//
//     omk::event("fight.begin", {{"opponent", id}, {"how", "attached"}});
//
// prints `@evt fight.begin opponent=572 how=attached` beside the prose line it
// mirrors, so a check asserts the record (`verify.py`'s `_events`) and the
// prose stays free to say what a person needs. Printed only when `OMK_EVENTS`
// is set: a run without it is byte for byte what it was, which is what every
// stdout-reading check and the refactor guard compare.
//
// An event's name and keys are its contract - renaming one is changing an
// interface, and `verify.py: events` asserts the ones the checks rely on.
#pragma once

#include <cstdio>
#include <initializer_list>
#include <string>

namespace omk {

bool envFlag(const char* name);

struct EventField {
    const char* key;
    std::string value;
    EventField(const char* k, long v) : key(k), value(std::to_string(v)) {}
    EventField(const char* k, int v) : key(k), value(std::to_string(v)) {}
    EventField(const char* k, unsigned v) : key(k), value(std::to_string(v)) {}
    EventField(const char* k, double v) : key(k) {
        char b[32];
        std::snprintf(b, sizeof b, "%.3f", v);
        value = b;
    }
    EventField(const char* k, float v) : EventField(k, static_cast<double>(v)) {}
    EventField(const char* k, bool v) : key(k), value(v ? "1" : "0") {}
    EventField(const char* k, const char* v) : key(k), value(v ? v : "") {}
    EventField(const char* k, const std::string& v) : key(k), value(v) {}
};

inline bool eventsOn() { return envFlag("OMK_EVENTS"); }

// `@evt <name> k=v ...` on stdout when `OMK_EVENTS` is set; a value with a
// space or an `=` in it is written in double quotes.
inline void event(const char* name, std::initializer_list<EventField> fields) {
    if (!eventsOn()) return;
    std::string line = std::string("@evt ") + name;
    for (const auto& f : fields) {
        const bool quote = f.value.find_first_of(" =\"") != std::string::npos || f.value.empty();
        line += ' ';
        line += f.key;
        line += '=';
        if (quote) {
            line += '"';
            for (char c : f.value) line += c == '"' ? '\'' : c;
            line += '"';
        } else {
            line += f.value;
        }
    }
    std::printf("%s\n", line.c_str());
}

}  // namespace omk
