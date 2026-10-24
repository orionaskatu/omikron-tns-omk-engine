// SPDX-License-Identifier: GPL-3.0-or-later
// The city grades - see colourgrade.h. Not a port of anything in the original.
// (the remaster's: compiled empty without OMK_REMASTER - `engine/Makefile`)
#if OMK_REMASTER

#include "o3de/colourgrade.h"

#include "platform/datafs.h"
#include "platform/hostfs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace omk {
namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

}  // namespace

bool parseCube(const std::string& text, ColourLut& out, std::string* why) {
    out = ColourLut{};
    const auto fail = [&](const char* w) { if (why) *why = w; return false; };
    for (const auto& raw : splitLines(text)) {
        const std::string ln = trim(raw);
        if (ln.empty() || ln[0] == '#') continue;
        if (ln.rfind("TITLE", 0) == 0) continue;
        if (ln.rfind("LUT_1D_SIZE", 0) == 0) return fail("a 1D LUT, not a 3D one");
        if (ln.rfind("LUT_3D_SIZE", 0) == 0) {
            out.n = std::atoi(ln.c_str() + 11);
            if (out.n < 2 || out.n > 64) return fail("LUT_3D_SIZE outside 2..64");
            out.rgb.reserve(static_cast<std::size_t>(out.n) * out.n * out.n * 3);
            continue;
        }
        // Resolve's exports name the input range: 0..1 alone is read
        if (ln.rfind("LUT_3D_INPUT_RANGE", 0) == 0) {
            float a = 0, b = 0;
            if (std::sscanf(ln.c_str() + 18, "%f %f", &a, &b) != 2 || a != 0.0f || b != 1.0f)
                return fail("a LUT_3D_INPUT_RANGE other than 0 1");
            continue;
        }
        if (ln.rfind("DOMAIN_MIN", 0) == 0 || ln.rfind("DOMAIN_MAX", 0) == 0) {
            float a = 0, b = 0, c = 0;
            const bool max = ln[7] == 'M' && ln[8] == 'A';
            if (std::sscanf(ln.c_str() + 10, "%f %f %f", &a, &b, &c) != 3) return fail("a bad DOMAIN line");
            const float want = max ? 1.0f : 0.0f;
            if (a != want || b != want || c != want) return fail("a DOMAIN other than 0..1");
            continue;
        }
        float r, g, b;
        if (std::sscanf(ln.c_str(), "%f %f %f", &r, &g, &b) != 3) return fail("a line that is not three numbers");
        // `%f` reads "nan" and "inf" too, and `lutStrip` would cast a NaN to a
        // byte (the audit's item 6, 2026-10-23)
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) return fail("a value that is not finite");
        if (out.n == 0) return fail("table data before LUT_3D_SIZE");
        out.rgb.push_back(r); out.rgb.push_back(g); out.rgb.push_back(b);
    }
    if (out.n == 0) return fail("no LUT_3D_SIZE");
    if (out.rgb.size() != static_cast<std::size_t>(out.n) * out.n * out.n * 3)
        return fail("the table is not LUT_3D_SIZE^3 entries");
    return true;
}

void lutStrip(const float* rgb, int n, std::vector<unsigned char>& out) {
    out.assign(static_cast<std::size_t>(n) * n * n * 4, 255);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                const float* src = rgb + 3 * ((static_cast<std::size_t>(b) * n + g) * n + r);
                unsigned char* d = &out[4 * (static_cast<std::size_t>(g) * n * n + b * n + r)];
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<unsigned char>(std::clamp(src[c], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
}

bool CityGrades::open(const std::string& dir) {
    luts_.clear(); cityOf_.clear(); night_.clear(); opened_ = false;
    if (!hostfs::isDirectory(dir)) {
        std::fprintf(stderr, "city grades: %s is not a directory - no grade\n", dir.c_str());
        return false;
    }
    for (const auto& name : hostfs::listNames(dir)) {
        if (name.size() > 5 && upper(name.substr(name.size() - 5)) == ".CUBE") {
            ColourLut lut;
            std::string why;
            if (!parseCube(readTextFile(dir + "/" + name), lut, &why)) {
                std::fprintf(stderr, "city grades: %s - %s; skipped\n", name.c_str(), why.c_str());
                continue;
            }
            luts_[upper(name.substr(0, name.size() - 5))] = std::move(lut);
        }
    }
    for (const auto& raw : splitLines(readTextFile(dir + "/map.txt"))) {
        const std::string ln = trim(raw);
        if (ln.empty() || ln[0] == '#') continue;
        const auto eq = ln.find('=');
        if (eq == std::string::npos) continue;
        cityOf_[upper(trim(ln.substr(0, eq)))] = upper(trim(ln.substr(eq + 1)));
    }
    for (const auto& raw : splitLines(readTextFile(dir + "/night.txt"))) {
        const std::string ln = trim(raw);
        if (ln.empty() || ln[0] == '#') continue;
        const auto eq = ln.find('=');
        if (eq == std::string::npos) continue;
        const float d = static_cast<float>(std::atof(trim(ln.substr(eq + 1)).c_str()));
        if (d > 0.0f && d <= 1.0f) night_[upper(trim(ln.substr(0, eq)))] = d;
    }
    opened_ = true;
    return true;
}

float CityGrades::nightOf(const std::string& setName) const {
    const std::string s = upper(setName);
    if (const auto it = night_.find(s); it != night_.end()) return it->second;
    if (const auto c = cityOf_.find(s); c != cityOf_.end())
        if (const auto it = night_.find(c->second); it != night_.end()) return it->second;
    return 1.0f;
}

const ColourLut* CityGrades::forSet(const std::string& setName) const {
    const std::string s = upper(setName);
    if (const auto it = luts_.find(s); it != luts_.end()) return &it->second;
    if (const auto c = cityOf_.find(s); c != cityOf_.end())
        if (const auto it = luts_.find(c->second); it != luts_.end()) return &it->second;
    return nullptr;
}

std::string CityGrades::sourceFor(const std::string& setName) const {
    const std::string s = upper(setName);
    if (luts_.count(s)) return s + ".cube";
    if (const auto c = cityOf_.find(s); c != cityOf_.end() && luts_.count(c->second))
        return c->second + ".cube (map.txt)";
    return "";
}

CityGrades& cityGrades() {
    static CityGrades g;
    return g;
}

}  // namespace omk

#endif  // OMK_REMASTER
