// SPDX-License-Identifier: GPL-3.0-or-later
// THE CITY GRADES - the remaster's colour grade per city (`todo/remaster.md`
// step 2). NOT IN THE ORIGINAL: a `[Remaster]` option (`grade = DIR`,
// `--grade DIR`), off unless named.
//
// DIR holds one standard `.cube` 3D LUT per set, named by the set's name as
// the engine reports it (`ANEKBAH.cube`, `QALISAR.cube` ...), and `map.txt`,
// lines `SET = CITY` for the sets that take a city's grade (an interior takes
// its city's: `AAPKAYL = ANEKBAH`). A set with neither is ungraded.
// `tools/omktexpack.py grades DIR` writes the four the reader chose from the
// previews of 2026-10-20; any colour tool that exports .cube can replace them.
//
// Drawn by Vulkan and GLES (`Renderer::setGrade`); the software reference,
// GL1 and the 3DS refuse it. The viewer turns it off while the greyscale bank
// is on (a grade would tint a black-and-white cutscene).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace omk {

struct ColourLut {
    int n = 0;                 // the cube's side
    std::vector<float> rgb;    // n^3 RGB triples, red fastest, 0..1
};

// One .cube file's text -> its LUT. False (and `why`) on anything it cannot
// read: LUT_1D, a size outside 2..64, a short or long table, DOMAIN other
// than 0..1.
bool parseCube(const std::string& text, ColourLut& out, std::string* why = nullptr);

// A LUT as the GPU backends sample it (`Renderer::setGrade`): a STRIP, RGBA8,
// n*n texels across and n down - row GREEN, column blue * n + red - from
// `rgb` (n^3 triples, red fastest) into `out`, n*n*n*4 bytes.
void lutStrip(const float* rgb, int n, std::vector<unsigned char>& out);

class CityGrades {
public:
    // Read DIR's .cube files and map.txt. False, said, when DIR is not a
    // directory; an unreadable .cube is said and skipped.
    bool open(const std::string& dir);
    bool active() const { return opened_; }
    std::size_t size() const { return luts_.size(); }
    // The LUT a set is graded with (its own, then its city's), or null.
    const ColourLut* forSet(const std::string& setName) const;
    // ...and the name of the file that answered, "" for none - for the log.
    std::string sourceFor(const std::string& setName) const;
    // THE NIGHT (`night.txt`, lines `SET = D`, its own then its city's): how
    // much of a set's PAINTED light stays when the remaster's emitters light
    // it (`o3de/emitterlight.h`) - Qalisar's 0.5 makes its neons the night's
    // light, as the Blade Runner streets it is drawn from (the reader,
    // 2026-10-20). 1 for a set it does not name.
    float nightOf(const std::string& setName) const;

private:
    bool opened_ = false;
    std::map<std::string, ColourLut> luts_;        // by set name, upper case
    std::map<std::string, std::string> cityOf_;    // map.txt
    std::map<std::string, float> night_;           // night.txt
};

CityGrades& cityGrades();

}  // namespace omk
