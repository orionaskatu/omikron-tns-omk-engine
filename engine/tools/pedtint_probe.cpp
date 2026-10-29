// SPDX-License-Identifier: GPL-3.0-or-later
// pedtint_probe - the crowd's colour variation (`pedvariety`,
// `todo/enhancements.md` 29, `o3de/pedtint.h`) over the REAL pages: every
// palette of the twenty-one PERSOS models the street crowd wears.
//
//     pedtint_probe <gamedata> [--break] [--where]
//
// For each palette entry of each page (the shader tints the texel it samples,
// and a texel of a palettised page is one of 256 entries - so the entries ARE
// the colours the crowd shows) and for 4096 tint words:
//   * SKIN - hue 5..45 degrees, saturation 0.2..0.55, value over 0.35, the core
//     of the window the rule keeps - comes out exactly as it went in;
//   * near-WHITE (value 0.9, saturation 0.05) and near-BLACK (value under 0.08)
//     stay so - the eyes, the teeth, the boots;
//   * a colour the rule means to TURN (saturation over 0.2, value over 0.2, hue
//     100..330 degrees - green to magenta) is changed, in 90% of the words by
//     at least 6/255 in a channel, and comes out in many distinct colours;
// and the walkers' looks: `Sliders::load` of Anekbah twice with one seed gives
// the same word to every slot, another seed gives other words, and the 200
// walkers wear at least 150 distinct ones.
//
// `--break` turns the skin rule off (what a probe is for: SHOWN TO FAIL) and
// the skin check must then report violations. `--where` lists Anekbah's
// walkers' spawn points (slot, model, x y z) - where to put the camera for a
// picture of the crowd.
#include "actor/sliders.h"
#include "formats/iam.h"
#include "formats/opt.h"
#include "formats/tex3dt.h"
#include "o3de/pedtint.h"
#include "platform/datafs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

std::string headerName(std::span<const std::byte> c, std::size_t off, std::size_t n) {
    if (c.size() < off + n) return {};
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        const char ch = static_cast<char>(c[off + i]);
        if (!ch) break;
        s += ch;
    }
    return s;
}
std::uint32_t u32at(std::span<const std::byte> c, std::size_t o) {
    if (c.size() < o + 4) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(c[o + i]) << (8 * i);
    return v;
}
struct Hsv { float h, s, v; };   // h in degrees
Hsv hsvOf(const float c[3]) {
    const float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2]));
    const float d = mx - mn;
    Hsv o{0.0f, mx > 0.0f ? d / mx : 0.0f, mx};
    if (d > 0.0f) {
        float h;
        if (mx == c[0]) h = std::fmod((c[1] - c[2]) / d + 6.0f, 6.0f);
        else if (mx == c[1]) h = (c[2] - c[0]) / d + 2.0f;
        else h = (c[0] - c[1]) / d + 4.0f;
        o.h = h * 60.0f;
    }
    return o;
}

int byteOf(float v) { return static_cast<int>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f)); }

// the loads of one seed: the walkers' looks, in slot order
std::vector<std::uint32_t> looksOf(const omk::OptTrack& track, const omk::PedClips& clips, std::uint32_t men,
                                   std::uint32_t women, std::uint32_t seed) {
    omk::Sliders pool;
    pool.load(track, clips, men, women, omk::kDefaultStreetActivity, seed);
    std::vector<std::uint32_t> out;
    for (const auto& m : pool.movers())
        if (m.live && m.vehicle < 0) out.push_back(m.look);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pedtint_probe <gamedata> [--break]\n");
        return 2;
    }
    const std::string root = argv[1];
    bool broken = false, where = false;
    for (int i = 2; i < argc; ++i) {
        broken |= std::strcmp(argv[i], "--break") == 0;
        where |= std::strcmp(argv[i], "--where") == 0;
    }
    const omk::DataFs fs(root);

    // ---- every palette of the crowd's models (distinct ones only)
    std::set<std::vector<std::uint8_t>> pals;
    int pages = 0, models = 0;
    for (int sex = 1; sex <= 2; ++sex)
        for (const std::string& name : omk::pedModelTable(sex)) {
            const auto d = fs.read("MESHES/PERSOS/" + name + ".3DO");
            const auto t = fs.read("MESHES/PERSOS/" + name + ".3DT");
            if (d.empty() || t.empty()) continue;
            ++models;
            for (const auto& tx : omk::textures(d, t)) {
                if (!tx.hasPixels()) continue;
                ++pages;
                pals.insert(std::vector<std::uint8_t>(tx.pal.data(), tx.pal.data() + 768));
            }
        }
    if (models == 0) { std::printf("pedtint: no crowd model found under %s\n", root.c_str()); return 1; }

    constexpr int kWords = 4096;
    std::vector<int> words;
    for (int i = 0; i < kWords; ++i) {
        std::uint32_t h = static_cast<std::uint32_t>(i) * 0x9E3779B1u + 12345u;
        h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
        words.push_back(omk::pedTintWord(h));
    }
    long entries = 0, skin = 0, white = 0, black = 0, turn = 0;
    long skinBad = 0, whiteBad = 0, blackBad = 0, turnBad = 0, turnWorst = kWords;
    long distinctMin = 1 << 30;
    for (const auto& pal : pals) {
        for (int e = 0; e < 256; ++e) {
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = static_cast<float>(pal[static_cast<std::size_t>(e) * 3 + static_cast<std::size_t>(k)]) / 255.0f;
            ++entries;
            const Hsv hv = hsvOf(c);
            const bool isSkin = hv.h >= 5.0f && hv.h <= 45.0f && hv.s >= 0.2f && hv.s <= 0.55f && hv.v > 0.35f;
            const bool isWhite = hv.v >= 0.9f && hv.s <= 0.05f;
            const bool isBlack = hv.v < 0.08f;
            const bool isTurn = hv.s > 0.2f && hv.v > 0.2f && hv.h >= 100.0f && hv.h <= 330.0f;
            if (!(isSkin || isWhite || isBlack || isTurn)) continue;
            skin += isSkin; white += isWhite; black += isBlack; turn += isTurn;
            long moved = 0;
            std::set<std::uint32_t> distinct;
            for (const int w : words) {
                float o[3];
                omk::pedTintRgb(c, w, o, !broken);
                int dmax = 0;
                for (int k = 0; k < 3; ++k) dmax = std::max(dmax, std::abs(byteOf(o[k]) - byteOf(c[k])));
                distinct.insert(static_cast<std::uint32_t>(byteOf(o[0]) | byteOf(o[1]) << 8 | byteOf(o[2]) << 16));
                if (isSkin && dmax != 0) ++skinBad;
                if (isWhite && dmax != 0) ++whiteBad;
                if (isBlack && std::max(o[0], std::max(o[1], o[2])) >= 0.2f) ++blackBad;
                if (isTurn && dmax >= 6) ++moved;
            }
            if (isTurn) {
                turnWorst = std::min(turnWorst, moved);
                if (moved * 10 < static_cast<long>(kWords) * 9) ++turnBad;
                distinctMin = std::min(distinctMin, static_cast<long>(distinct.size()));
            }
        }
    }
    std::printf("pedtint models %d pages %d palettes %zu entries %ld skin %ld white %ld black %ld turn %ld words %d\n",
                models, pages, pals.size(), entries, skin, white, black, turn, kWords);
    std::printf("pedtint skin_violations %ld white_violations %ld black_violations %ld\n", skinBad, whiteBad, blackBad);
    std::printf("pedtint turned_entries_moving_under_90pct %ld (worst %ld of %d) distinct_colours_min %ld\n",
                turnBad, turnWorst, kWords, distinctMin == (1 << 30) ? 0L : distinctMin);

    // ---- the walkers' looks: Anekbah (area 0), by the area header's masks
    const auto areaFile = fs.read("IAM/AREA");
    const auto arc = omk::IamArchive::open(areaFile);
    const auto chunk = arc.chunk(0);
    const std::string opt = headerName(chunk, 115, 9), ani = headerName(chunk, 124, 9);
    const std::uint32_t men = u32at(chunk, 164), women = u32at(chunk, 168);
    const auto track = omk::loadOpt(fs.read("TRAJECTOIRES/" + opt + ".OPT"));
    if (!track.valid) { std::printf("pedtint: area 0's circuit %s did not load\n", opt.c_str()); return 1; }
    const omk::PedClips clips = omk::pedClipsFrom(fs.read("ANIMS/" + ani + ".ANI"));
    if (where) {
        omk::Sliders pool;
        pool.load(track, clips, men, women, omk::kDefaultStreetActivity, 1u);
        int k = 0;
        for (const auto& m : pool.movers())
            if (m.live && m.vehicle < 0)
                std::printf("walker %d %s %.0f %.0f %.0f\n", k++, m.model.c_str(), static_cast<double>(m.body[0]),
                            static_cast<double>(m.body[1]), static_cast<double>(m.body[2]));
    }
    const auto a = looksOf(track, clips, men, women, 1u);
    const auto b = looksOf(track, clips, men, women, 1u);
    const auto c = looksOf(track, clips, men, women, 0x1234567u);
    std::set<int> distinctWords;
    for (const std::uint32_t l : a) distinctWords.insert(omk::pedTintWord(l));
    std::size_t differ = 0;
    for (std::size_t i = 0; i < a.size() && i < c.size(); ++i) differ += a[i] != c[i];
    std::printf("pedtint walkers %zu distinct_words %zu same_seed_equal %d other_seed_differs %zu\n",
                a.size(), distinctWords.size(), a == b ? 1 : 0, differ);
    return 0;
}
