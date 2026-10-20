// SPDX-License-Identifier: GPL-3.0-or-later
// The city grades - see colourgrade.h. Not a port of anything in the original.
#include "o3de/colourgrade.h"

#include "platform/datafs.h"
#include "platform/hostfs.h"

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
        if (out.n == 0) return fail("table data before LUT_3D_SIZE");
        out.rgb.push_back(r); out.rgb.push_back(g); out.rgb.push_back(b);
    }
    if (out.n == 0) return fail("no LUT_3D_SIZE");
    if (out.rgb.size() != static_cast<std::size_t>(out.n) * out.n * out.n * 3)
        return fail("the table is not LUT_3D_SIZE^3 entries");
    return true;
}

bool CityGrades::open(const std::string& dir) {
    luts_.clear(); cityOf_.clear(); opened_ = false;
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
    opened_ = true;
    return true;
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
