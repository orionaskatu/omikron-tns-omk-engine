// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/options.h"
#include "platform/datafs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace omk {

namespace {
std::string lower(std::string v) {
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}
std::string trim(const std::string& v) {
    const auto a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const auto b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}
}  // namespace

const std::vector<std::string>& preferenceKeys() {
    static const std::vector<std::string> k = {
    "AmbientAttenuation", "DialogAttenuation", "FlagNoCD",
    "FxAttenuation", "GFXCard", "MouseSensX",
    "MouseSensY", "SoftwareMode", "add_perso1",
    "add_perso10", "add_perso2", "add_perso3",
    "add_perso4", "add_perso5", "add_perso6",
    "add_perso7", "add_perso8", "add_perso9",
    "autocameraplayer", "camera_free", "clipdistance",
    "collidescale", "confirm_exit", "ctlinfos",
    "debug_graph_info", "debug_maps", "debug_script_info",
    "debug_sound", "debug_text_info", "debug_zones",
    "default_animbank", "default_background", "default_character",
    "default_passersbank", "default_passerspath", "default_scx",
    "default_sfx", "default_sfxscx", "disable_fight_ai",
    "disable_fight_damages", "disablebodymotion", "display_colliding_faces",
    "display_doors", "display_nearestperso", "display_path",
    "display_zone", "displaycameracone", "displaycodecount",
    "displayfightcol", "displayframerate", "displaypassersclock",
    "displayshadows", "displaysky", "displaytimetest",
    "exclusive_mouse", "lock_mouse", "music",
    "no_fight_guard", "nomotionfile", "screen_x",
    "screen_y", "ssdir", "tekkencam",
    "viewer", "window"
    };
    return k;
}

const std::string* OptionsFile::find(const std::string& section,
                                     const std::string& key) const {
    const auto s = sections.find(lower(section));
    if (s == sections.end()) return nullptr;
    const auto k = s->second.find(lower(key));
    return k == s->second.end() ? nullptr : &k->second;
}

int OptionsFile::integer(const std::string& section, const std::string& key,
                         int fallback) const {
    const std::string* v = find(section, key);
    if (!v || v->empty()) return fallback;
    // `atoi`, which is what the engine calls on the returned string - a
    // trailing word or a bad value gives 0 rather than an error.
    return std::atoi(v->c_str());
}

bool OptionsFile::boolean(const std::string& section, const std::string& key,
                          bool fallback) const {
    const std::string* v = find(section, key);
    if (!v || v->empty()) return fallback;
    const std::string s = lower(trim(*v));
    if (s == "oui" || s == "yes" || s == "true" || s == "on") return true;
    if (s == "non" || s == "no" || s == "false" || s == "off") return false;
    return std::atoi(s.c_str()) != 0;
}

OptionsFile loadOptionsFile(const std::string& path) {
    OptionsFile o;
    o.path = path;
    if (fileSize(path) < 0) return o;   // absent is not an error
    o.loaded = true;
    std::string section;
    for (std::string line : splitLines(readTextFile(path))) {
        // a `;` or `#` comment, and the Win32 parser's own tolerance of blanks
        const auto cut = line.find_first_of(";#");
        if (cut != std::string::npos) line = line.substr(0, cut);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;
        o.sections[section][key] = val;
    }
    // a key under [Preferences] the engine never reads is almost certainly a
    // typo, and silence is the worst answer to that
    const auto pref = o.sections.find("preferences");
    if (pref != o.sections.end()) {
        for (const auto& kv : pref->second) {
            bool known = false;
            for (const auto& k : preferenceKeys())
                if (lower(k) == kv.first) { known = true; break; }
            if (!known) o.unknown.push_back(kv.first);
        }
        std::sort(o.unknown.begin(), o.unknown.end());
    }
    return o;
}


// ---- the writer (`options.h`) ---------------------------------------------

namespace {

// One line of the file: what is on it, and how it ended ("\r\n", "\n", or ""
// for a last line with none), so an edit gives every ending back.
struct IniLine {
    std::string body, eol;
};

std::vector<IniLine> iniLines(const std::string& text) {
    std::vector<IniLine> out;
    std::size_t a = 0;
    while (a < text.size()) {
        const std::size_t nl = text.find('\n', a);
        IniLine l;
        if (nl == std::string::npos) {
            l.body = text.substr(a);
            a = text.size();
        } else {
            l.body = text.substr(a, nl - a);
            l.eol = "\n";
            a = nl + 1;
        }
        if (!l.body.empty() && l.body.back() == '\r') {
            l.body.pop_back();
            l.eol = "\r" + l.eol;
        }
        out.push_back(std::move(l));
    }
    return out;
}

// What the READER makes of a line: a section header, an active `key = value`,
// or neither. `cut` is where its comment starts (the body's end without one).
struct IniParse {
    bool header = false;
    std::string name;            // the section (header) or the key, lower case
    std::size_t eq = std::string::npos, cut = 0;
};

IniParse iniParse(const std::string& body) {
    IniParse p;
    p.cut = std::min(body.find_first_of(";#"), body.size());
    const std::string line = trim(body.substr(0, p.cut));
    if (line.empty()) return p;
    if (line.front() == '[' && line.back() == ']') {
        p.header = true;
        p.name = lower(trim(line.substr(1, line.size() - 2)));
        return p;
    }
    const std::size_t eq = body.find('=');
    if (eq == std::string::npos || eq > p.cut) return p;
    p.name = lower(trim(body.substr(0, eq)));
    if (!p.name.empty()) p.eq = eq;
    return p;
}

// `body`, an active line whose `=` is at `eq` and comment at `cut`, with its
// value replaced and everything around the value kept
std::string iniSetValue(const std::string& body, std::size_t eq, std::size_t cut,
                        const std::string& value) {
    std::size_t start = eq + 1;
    while (start < cut && (body[start] == ' ' || body[start] == '\t')) ++start;
    std::size_t end = cut;
    while (end > start && (body[end - 1] == ' ' || body[end - 1] == '\t')) --end;
    if (start == end)   // no value: after the `=`, one space, the rest as it was
        return body.substr(0, eq + 1) + " " + value + body.substr(eq + 1);
    return body.substr(0, start) + value + body.substr(end);
}

bool iniWritable(const std::string& v, const char* refused) {
    return v.find_first_of(refused) == std::string::npos;
}

}  // namespace

bool editOptionsText(const std::string& text, const std::vector<OptionsEdit>& edits,
                     std::string& out) {
    std::vector<IniLine> lines = iniLines(text);
    // the ending a new line takes: the file's first, else a plain "\n"
    const std::string eol = !lines.empty() && !lines.front().eol.empty() ? lines.front().eol
                                                                          : std::string("\n");
    for (const OptionsEdit& e : edits) {
        const std::string section = lower(trim(e.section)), key = lower(trim(e.key));
        const std::string value = trim(e.value);
        if (section.empty() || key.empty() || !iniWritable(e.section, ";#\r\n[]") ||
            !iniWritable(e.key, ";#\r\n[]=") || !iniWritable(value, ";#\r\n")) {
            std::fprintf(stderr, "ini: cannot write [%s] %s = %s - a `;` or `#` would be read as a "
                                 "comment, a line break would split the line\n",
                         e.section.c_str(), e.key.c_str(), e.value.c_str());
            return false;
        }
        std::string in;                         // the section each line is in
        std::vector<std::size_t> active;        // the key's active lines
        std::size_t commented = std::string::npos, lastKey = std::string::npos,
                    header = std::string::npos;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string& body = lines[i].body;
            const IniParse p = iniParse(body);
            if (p.header) {
                in = p.name;
                if (in == section) header = i;
                continue;
            }
            if (in != section) continue;
            if (p.eq != std::string::npos) {
                lastKey = i;
                if (p.name == key) active.push_back(i);
                continue;
            }
            // `;key = value`: the comment characters, then a line the reader
            // would take for this key - the LAST such, prose comes first
            const std::size_t lead = body.find_first_not_of(" \t");
            if (lead == std::string::npos || (body[lead] != ';' && body[lead] != '#')) continue;
            const std::size_t rest = body.find_first_not_of(";# \t", lead);
            if (rest == std::string::npos) continue;
            const IniParse q = iniParse(body.substr(rest));
            if (!q.header && q.eq != std::string::npos && q.name == key) commented = i;
        }
        if (!active.empty()) {
            for (const std::size_t i : active) {
                const IniParse p = iniParse(lines[i].body);
                lines[i].body = iniSetValue(lines[i].body, p.eq, p.cut, value);
            }
            continue;
        }
        if (commented != std::string::npos) {
            std::string& body = lines[commented].body;
            body = body.substr(body.find_first_not_of(";# \t"));
            const IniParse p = iniParse(body);
            body = iniSetValue(body, p.eq, p.cut, value);
            continue;
        }
        const std::string line = trim(e.key) + " = " + value;
        // after the section's last setting, else its header (the last time it
        // is opened) - and with neither, a new section at the end
        std::size_t after = lastKey != std::string::npos ? lastKey : header;
        std::vector<IniLine> add;
        if (after == std::string::npos) {
            after = lines.empty() ? std::string::npos : lines.size() - 1;
            if (!lines.empty()) add.push_back({"", eol});
            add.push_back({"[" + trim(e.section) + "]", eol});
        }
        add.push_back({line, eol});
        std::size_t at = 0;
        if (after != std::string::npos) {
            // a last line without its ending keeps none: the new one inherits it
            if (lines[after].eol.empty()) {
                lines[after].eol = eol;
                add.back().eol.clear();
            }
            at = after + 1;
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), add.begin(), add.end());
    }
    out.clear();
    for (const IniLine& l : lines) out += l.body + l.eol;
    return true;
}

bool writeOptionsFile(const std::string& path, const std::vector<OptionsEdit>& edits) {
    if (!safeOutputPath(path)) return false;
    const std::string before = fileSize(path) >= 0 ? readTextFile(path) : std::string();
    std::string after;
    if (!editOptionsText(before, edits, after)) return false;
    if (after == before && fileSize(path) >= 0) return true;   // nothing changed, nothing written
    const auto cut = path.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0) makeDirectories(path.substr(0, cut));
    if (!writeWholeFile(path, after.data(), after.size())) {
        std::fprintf(stderr, "ini: cannot write %s\n", path.c_str());
        return false;
    }
    return true;
}

}  // namespace omk
