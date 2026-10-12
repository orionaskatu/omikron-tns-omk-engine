// SPDX-License-Identifier: GPL-3.0-or-later
// THE SNEAK'S ROW WINDOW - `sub_42AFF0`, and the tenth object.
//
//     row_window <tables>
//
// The nine row widgets of list 0x004DE6F0 are a WINDOW onto a list that may
// be longer. `sub_42AAE0(list, window)` binds them and `sub_42AFF0` moves the
// selection AND the window: a centred rule, where the cursor moves until it
// reaches the middle widget and then the window moves under it instead.
//
// With the window held at 0 a list longer than nine is truncated - carry ten
// things and the tenth cannot be reached at all. This drives DOWN until
// nothing moves and reports the furthest row reached, the number that shows
// it.
//
// One line per fact, `key ...`:
//   widgets   how many row widgets the list has, and the window's start
//   walk N    driving DOWN N times: the row reached, the window, the marks
//   back      driving UP the same number of times returns to row 0
#include "ui/widgets.h"
#include "actor/moves.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: row_window <tables>\n"); return 2; }
    const auto w = omk::UiWidgets::loadJson(std::string(argv[1]) + "/ui_widgets.json");
    if (!w.valid()) { std::fprintf(stderr, "no ui_widgets.json\n"); return 1; }

    const omk::UiList* rows = nullptr;
    if (const auto* p = w.at(omk::kPanelSneakInventory))
        for (const auto& l : p->lists)
            if (l.addr == omk::kListSneakRows) rows = &l;
    if (!rows) { std::fprintf(stderr, "no row list\n"); return 1; }
    const int widgets = static_cast<int>(rows->items.size());

    // Twelve rows in nine widgets - the case the old code could not reach.
    const int count = 12;
    omk::UiListState st;
    omk::UiWalk walk(w, st);
    walk.open(omk::screen::Sneak);
    walk.bindRows(omk::kListSneakRows, count, 0);
    std::printf("widgets %d window %d count %d\n", widgets,
                walk.rowWindow(omk::kListSneakRows), count);

    // DOWN until it stops moving, and the furthest ROW the selection reaches.
    int best = 0;
    for (int i = 0; i < 40; ++i) {
        walk.press(omk::kUiDown);
        walk.bindRows(omk::kListSneakRows, count,
                      walk.rowWindow(omk::kListSneakRows));
        const auto* sel = walk.selected();
        const int row = sel ? walk.rowOf(sel->addr) : -1;
        if (row > best) best = row;
    }
    // ...and THE VERB'S OWN READ. All three sneak verbs take
    // `selected_widget[+0x3C]` - the row TAG - not the SELECTION, which is
    // the widget 0..8. Scrolled to the end of a twelve-row list the two are 8
    // and 11, so a verb reading the selection would use the object three rows
    // above the one the player is looking at.
    std::printf("verb reads row %d, selection is %d\n",
                walk.selectedRow(omk::kListSneakRows),
                walk.selectionOf(omk::kListSneakRows));

    const std::uint32_t topMark = walk.rowArrows(rows->items.front().addr);
    const std::uint32_t botMark = walk.rowArrows(rows->items.back().addr);
    std::printf("walk down reached row %d of %d, window %d, top_mark %d bot_mark %d\n",
                best, count - 1, walk.rowWindow(omk::kListSneakRows),
                (topMark & 0x100000u) ? 1 : 0, (botMark & 0x200000u) ? 1 : 0);

    for (int i = 0; i < 40; ++i) {
        walk.press(omk::kUiUp);
        walk.bindRows(omk::kListSneakRows, count,
                      walk.rowWindow(omk::kListSneakRows));
    }
    const auto* sel = walk.selected();
    std::printf("walk up returned to row %d, window %d\n",
                sel ? walk.rowOf(sel->addr) : -1,
                walk.rowWindow(omk::kListSneakRows));
    return 0;
}
