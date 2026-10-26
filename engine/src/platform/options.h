// SPDX-License-Identifier: GPL-3.0-or-later
// The game's own configuration file - `[Preferences]`, and the 65 keys the
// engine reads out of it.
//
// `GetPrivateProfileStringA` is not only the `.TAG` logger the golden-trace
// rig leans on. The engine reads a `[Preferences]` section through the same
// API, and also `[Debug]` and `[User]` - the last carrying an `id` and the
// developers' own names (ANTOINE, CHRISTOPHE, FABIEN, FRANCOIS, GUEST, MANU,
// OLIVIER), so a build could carry per-person overrides.
//
// **No `.ini` ships in `gamedata/`**: the file is written by the setup dialog,
// which is what `Runtime.exe CONFIG` opens - `WinMain`'s third switch, after
// WINDOW and NOFMV, running `DialogBoxParamA` on template 0x68. So there is
// nothing here to read off a real install; what the binary gives is the KEY
// NAMES, and those are what this file uses rather than anything invented.
//
// The names had to come out of the DATA section, not from IDA's labels, which
// truncate at 14 characters and mislead: `aDisplaypassers` is really
// `displaypassersclock` (a debug readout, NOT the crowd density) and
// `aAmbientattenua` is `AmbientAttenuation`.
//
// **Two of the graphical settings have no key here at all** - the crowd
// density (options row 6, *Niveau d'activité dans les rues*) and the level of
// detail (row 7). Those are menu-only: their apply-hooks write globals, and
// density's is the one `Slider_Init` reads as `39 * (5 - density) * h[3]`.
// This port carries them in its own `[Options]` section, which is marked as
// this port's addition wherever it appears.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace omk {

// The 65 `[Preferences]` keys, exactly as the binary spells them.
const std::vector<std::string>& preferenceKeys();

struct OptionsFile {
    bool loaded = false;
    std::string path;
    // section -> key -> value, both folded to lower case for lookup, because
    // the Win32 profile API matches case-insensitively.
    std::map<std::string, std::map<std::string, std::string>> sections;
    // keys under [Preferences] that the engine does NOT read - a typo in a
    // hand-written file is otherwise silent.
    std::vector<std::string> unknown;

    const std::string* find(const std::string& section, const std::string& key) const;
    int  integer(const std::string& section, const std::string& key, int fallback) const;
    bool boolean(const std::string& section, const std::string& key, bool fallback) const;
};

// Parse an ini. A missing file is not an error: `loaded` stays false and every
// lookup falls back, which is what the engine does when a key is absent.
OptionsFile loadOptionsFile(const std::string& path);
// ...and the same parse of a text already in memory (`ui/omktext.h` reads
// its embedded strings this way); `loaded` is true, `path` empty.
OptionsFile parseOptionsText(const std::string& text);

// ---- WRITING IT BACK - OMK's own options menu (`todo/graphics-menu.md` A1)
//
// The engine never writes its ini (the setup dialog does), so this is the
// port's. It is an EDIT, never a rewrite: a reader's file is commented by hand
// (`omk.ini.example`, the Vita's `omk.ini`), and everything an edit does not
// name - comments, blank lines, other sections, unknown keys, the line
// endings, the bytes of a cp1252 comment - comes back byte for byte.
//
//   * a key that is SET has its value replaced on its own line, its spelling,
//     the spaces around `=` and a trailing comment kept - on every active line
//     of it in that section, since the reader takes the last and a file with
//     two would otherwise still read the old one;
//   * a key that is only COMMENTED OUT in its section - the template's
//     `;language = fr` - is set on that line, uncommented, so its
//     documentation stays above it;
//   * otherwise it is added after the section's last setting, and a section
//     the file has not got is added at its end;
//   * an UNSET edit (the menu's "Valeurs par défaut") comments every active
//     line of the key out - `;key = value`, so the next set finds it there
//     and the reader falls back to its default; a key not set is left alone.
//
// A section seen twice is one section, as the reader merges them.
struct OptionsEdit {
    std::string section, key, value;
    bool unset = false;
};
// `text`, a whole ini, with `edits` applied in order. -> false (and `out`
// untouched) when a value or name cannot be written: a `;` or `#` would be
// read as a comment, a line break would split the line, a `]` or `=` in a
// name would be a different line.
bool editOptionsText(const std::string& text, const std::vector<OptionsEdit>& edits,
                     std::string& out);
// ...and on a file: read (absent is empty), edited, written whole through
// `safeOutputPath` - the save file's way (`writeSaveFile`), no rename, which
// not every port has. Its directory is made when missing. -> false on a
// refused edit or path, or a failed write, said on stderr.
bool writeOptionsFile(const std::string& path, const std::vector<OptionsEdit>& edits);

}  // namespace omk
