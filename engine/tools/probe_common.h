// SPDX-License-Identifier: GPL-3.0-or-later
// THE PROBES' SHARED PREAMBLE (`todo/code-readability-audit.md` 5): most
// probes here take `<gamedata> <tables> ...` and open the same three things -
// the opcode table out of the tables directory, the shipped `IAM` directory,
// and its new-game state (`IAM\START`). Sixty-odd of them wrote those lines out
// by hand; a new probe starts from this instead, and the old ones move onto it
// as they are touched.
//
//     const auto in = omk::probe::open(argc, argv, 3, "usage: my_probe <gamedata> <tables>");
//     if (!in) return in.code;
//     auto state = in.newGame();
//     omk::Session s(in.iam, state, in.table);
#pragma once

#include "script/gamestate.h"
#include "script/script.h"

#include <cstdio>
#include <string>

namespace omk::probe {

struct Inputs {
    std::string data;       // argv[1], the shipped tree's root
    std::string tables;     // argv[2], the lifted tables
    std::string iam;        // `<data>/IAM`
    OpcodeTable table;      // `<tables>/vm_opcodes.json`
    int code = 0;           // the exit code to return when this is false
    explicit operator bool() const { return code == 0; }
    // the new-game state, `IAM\START` - a fresh copy each call
    GameState newGame() const { return GameState::fromFile(iam + "/START"); }
};

// `usage` is printed (with a newline) and the code is 2 when there are fewer
// than `minArgs` arguments, argv[0] counted; the code is 1 when the opcode table
// does not load.
inline Inputs open(int argc, char** argv, int minArgs, const char* usage) {
    Inputs in;
    if (argc < minArgs || argc < 3) {
        std::fprintf(stderr, "%s\n", usage);
        in.code = 2;
        return in;
    }
    in.data = argv[1];
    in.tables = argv[2];
    in.iam = in.data + "/IAM";
    in.table = OpcodeTable::loadJson(in.tables + "/vm_opcodes.json");
    if (!in.table.valid()) in.code = 1;
    return in;
}

}  // namespace omk::probe
