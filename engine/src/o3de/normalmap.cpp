// SPDX-License-Identifier: GPL-3.0-or-later
#include "o3de/normalmap.h"

#include <algorithm>
#include <cmath>

namespace omk {

namespace {

// a box blur of radius r along x then y, WRAPPED (the texture tiles), each
// texel weighted by `wt` (a keyed texel weighs 0): out = blur(v wt) / blur(wt)
void boxBlur(const std::vector<float>& v, const std::vector<float>& wt, int w, int h, int r,
             std::vector<float>& out) {
    if (r <= 0) { out = v; return; }
    std::vector<float> a(v.size()), b(v.size()), aw(v.size()), bw(v.size());
    const auto wrap = [](int i, int n) { return ((i % n) + n) % n; };
    for (int y = 0; y < h; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * w;
        double s = 0, sw = 0;
        for (int k = -r; k <= r; ++k) {
            const std::size_t q = row + wrap(k, w);
            s += v[q] * wt[q]; sw += wt[q];
        }
        for (int x = 0; x < w; ++x) {
            a[row + x] = static_cast<float>(s); aw[row + x] = static_cast<float>(sw);
            const std::size_t add = row + wrap(x + r + 1, w), sub = row + wrap(x - r, w);
            s += v[add] * wt[add] - v[sub] * wt[sub];
            sw += wt[add] - wt[sub];
        }
    }
    for (int x = 0; x < w; ++x) {
        double s = 0, sw = 0;
        for (int k = -r; k <= r; ++k) {
            const std::size_t q = static_cast<std::size_t>(wrap(k, h)) * w + x;
            s += a[q]; sw += aw[q];
        }
        for (int y = 0; y < h; ++y) {
            const std::size_t q = static_cast<std::size_t>(y) * w + x;
            b[q] = static_cast<float>(s); bw[q] = static_cast<float>(sw);
            const std::size_t add = static_cast<std::size_t>(wrap(y + r + 1, h)) * w + x;
            const std::size_t sub = static_cast<std::size_t>(wrap(y - r, h)) * w + x;
            s += a[add] - a[sub]; sw += aw[add] - aw[sub];
        }
    }
    out.resize(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) out[i] = bw[i] > 1e-6f ? b[i] / bw[i] : 0.0f;
}

}  // namespace

void normalMap(const std::uint8_t* rgba, int w, int h, const NormalMapParams& p, std::vector<std::uint8_t>& out) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<float> lum(n), wt(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* c = rgba + 4 * i;
        lum[i] = (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 255.0f;
        wt[i] = c[3] >= 128 ? 1.0f : 0.0f;
    }
    // THE HEIGHT, band-passed: a light blur (the upscaler's noise) minus the wide one
    std::vector<float> fine, wide;
    boxBlur(lum, wt, w, h, std::max(1, w / 512), fine);
    boxBlur(lum, wt, w, h, std::max(2, p.wide * w / 1024), wide);
    std::vector<float> height(n);
    for (std::size_t i = 0; i < n; ++i) height[i] = wt[i] > 0.0f ? fine[i] - wide[i] : 0.0f;
    // THE SLOPE over four scales, each blur of the height differenced across
    // its own radius, the finer weighted more
    const auto wrap = [](int i, int m) { return ((i % m) + m) % m; };
    std::vector<float> gx(n, 0.0f), gy(n, 0.0f), level;
    const int scales[4] = {1, 2, 4, 8};
    const float weights[4] = {0.4f, 0.3f, 0.2f, 0.1f};
    const float unit = static_cast<float>(w) / 1024.0f;   // a pack texel at 1024 across
    for (int s = 0; s < 4; ++s) {
        const int r = std::max(1, static_cast<int>(std::lround(scales[s] * unit)));
        boxBlur(height, wt, w, h, s == 0 ? 0 : r, level);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const float dx = level[static_cast<std::size_t>(y) * w + wrap(x + r, w)] -
                                 level[static_cast<std::size_t>(y) * w + wrap(x - r, w)];
                const float dy = level[static_cast<std::size_t>(wrap(y + r, h)) * w + x] -
                                 level[static_cast<std::size_t>(wrap(y - r, h)) * w + x];
                // per texel at 1024 across, whatever this texture's size
                gx[i] += weights[s] * dx / (2.0f * r / unit);
                gy[i] += weights[s] * dy / (2.0f * r / unit);
            }
    }
    out.resize(4 * n);
    const float k = 10.0f * p.strength * 4.0f;   // 0.1 a texel -> ~45 degrees at strength 1 (x4: the scales' mean slope)
    // the BASE's slope (x / z, y / z), blurred: its large forms without its inventions
    std::vector<float> bx, by;
    if (p.base) {
        std::vector<float> sx(n), sy(n), one(n, 1.0f), t;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t* c = p.base + 4 * i;
            const float z = std::max(0.05f, c[2] / 127.5f - 1.0f);
            sx[i] = (c[0] / 127.5f - 1.0f) / z;
            sy[i] = (c[1] / 127.5f - 1.0f) / z;
        }
        const int r = std::max(1, static_cast<int>(std::lround(p.baseBlur * unit)));
        boxBlur(sx, one, w, h, r, t); boxBlur(t, one, w, h, r, bx);
        boxBlur(sy, one, w, h, r, t); boxBlur(t, one, w, h, r, by);
    }
    // the distance, in texels, to the nearest texel no face samples or on a
    // chart's border (wrapped,
    // four-connected, counted to `fade`): the slope's weight
    std::vector<float> keep(n, 1.0f);
    if (p.cover && p.cw > 0 && p.ch > 0) {
        const int fade = std::max(1, static_cast<int>(std::lround(p.fade * unit)));
        std::vector<int> dist(n, fade), q, next;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const int cx = static_cast<int>(static_cast<long long>(x) * p.cw / w);
                const int cy = static_cast<int>(static_cast<long long>(y) * p.ch / h);
                if (p.cover[static_cast<std::size_t>(cy) * p.cw + cx] != 1) { dist[i] = 0; q.push_back(static_cast<int>(i)); }
            }
        for (int d = 1; d < fade && !q.empty(); ++d) {
            next.clear();
            for (const int i : q) {
                const int x = i % w, y = i / w;
                const int nb[4] = {y * w + wrap(x + 1, w), y * w + wrap(x - 1, w),
                                   wrap(y + 1, h) * w + x, wrap(y - 1, h) * w + x};
                for (const int j : nb)
                    if (dist[static_cast<std::size_t>(j)] > d) { dist[static_cast<std::size_t>(j)] = d; next.push_back(j); }
            }
            q.swap(next);
        }
        for (std::size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(dist[i]) / fade;
            keep[i] = t * t * (3.0f - 2.0f * t);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        // brighter is HIGHER: the normal leans away from the rise. Image y runs
        // down, the map's G up - so +dy (brighter below) leans the normal up.
        float nx = -gx[i] * k, ny = gy[i] * k, nz = 1.0f;
        if (p.base) {
            nx = p.baseWeight * bx[i] + p.ownWeight * nx;
            ny = p.baseWeight * by[i] + p.ownWeight * ny;
        }
        if (wt[i] == 0.0f) { nx = 0.0f; ny = 0.0f; }
        nx *= keep[i]; ny *= keep[i];
        const float l = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
        const auto enc = [](float v) {
            return static_cast<std::uint8_t>(std::clamp(std::lround((v * 0.5f + 0.5f) * 255.0f), 0L, 255L));
        };
        out[4 * i + 0] = enc(nx * l);
        out[4 * i + 1] = enc(ny * l);
        out[4 * i + 2] = enc(nz * l);
        out[4 * i + 3] = static_cast<std::uint8_t>(std::clamp(std::lround(128.0f + height[i] * 255.0f), 0L, 255L));
    }
}

}  // namespace omk
