// SPDX-License-Identifier: GPL-3.0-or-later
#include "o3de/lighttiles.h"

#include <algorithm>
#include <cmath>

#include "platform/env.h"

namespace omk {

int depthSlice(float z) {
    if (!(z > kSliceNear)) return 0;
    const float t = std::log(z / kSliceNear) / std::log(kSliceFar / kSliceNear);
    return std::clamp(static_cast<int>(t * kLightSlices), 0, kLightSlices - 1);
}

void binLights(const RCamera& cam, int pxW, int pxH, std::span<const View::GpuLight> points, LightTiles& out) {
    const int tile = std::max(kLightTile, (pxW + kMaxAcross - 1) / kMaxAcross);
    out.tile = tile;
    out.across = std::max(1, (pxW + tile - 1) / tile);
    out.down = std::max(1, (pxH + tile - 1) / tile);
    out.listed = 0;
    out.full = 0;
    const std::size_t plane = static_cast<std::size_t>(out.across) * static_cast<std::size_t>(out.down);
    const std::size_t tiles = plane * kLightSlices;   // the CLUSTERS
    float s[3], u[3], f[3], tanH = 1.0f, tanV = 1.0f;
    cameraBasis(cam, s, u, f, tanH, tanV);
    const float sx = static_cast<float>(pxW) * 0.5f, sy = static_cast<float>(pxH) * 0.5f;
    // each light's tile rectangle, and its depth (the lists nearest first)
    struct Rect { int x0, y0, x1, y1; float z; int s0 = 0, s1 = kLightSlices - 1; };
    std::vector<Rect> rects(points.size(), Rect{1, 1, 0, 0, 0.0f});
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& l = points[i];
        const float d[3] = {l.pos[0] - cam.eye[0], l.pos[1] - cam.eye[1], l.pos[2] - cam.eye[2]};
        const float vx = s[0] * d[0] + s[1] * d[1] + s[2] * d[2];
        const float vy = u[0] * d[0] + u[1] * d[1] + u[2] * d[2];
        const float vz = f[0] * d[0] + f[1] * d[1] + f[2] * d[2];
        const float r = l.radiusA;
        if (vz + r <= kNearCut) continue;                 // wholly behind the eye
        Rect& q = rects[i];
        q.z = vz;
        q.s0 = depthSlice(vz - r);
        q.s1 = depthSlice(vz + r);
        if (vz - r <= kNearCut * 4.0f) {                   // the eye in or near it: every tile
            q.x0 = 0; q.y0 = 0; q.x1 = out.across - 1; q.y1 = out.down - 1;
            continue;
        }
        // the sphere's screen rectangle, CONSERVATIVE: its projected radius
        // grown by 1/cos of the half-angle it subtends
        const float k = 1.0f / std::sqrt(std::max(1e-6f, 1.0f - (r * r) / (vz * vz)));
        float cx = sx * (1.0f + (vx / vz) / tanH);
        const float cy = sy * (1.0f - (vy / vz) / tanV);
        if (cam.flipX) cx = static_cast<float>(pxW) - cx;
        const float rx = sx * (r / vz) / tanH * k * 1.15f, ry = sy * (r / vz) / tanV * k * 1.15f;
        q.x0 = std::max(0, static_cast<int>(std::floor((cx - rx) / tile)));
        q.x1 = std::min(out.across - 1, static_cast<int>(std::floor((cx + rx) / tile)));
        q.y0 = std::max(0, static_cast<int>(std::floor((cy - ry) / tile)));
        q.y1 = std::min(out.down - 1, static_cast<int>(std::floor((cy + ry) / tile)));
    }
    // THE CLUSTER TEST, tight: a light joins a cluster only where its sphere
    // reaches the cluster's own box of view space (the tile's rays between the
    // slice's two depths) - the padded rectangle and whole slice ranges filled
    // half the clusters, which then dropped lights as the camera moved: blocks
    // of street darkening in turn, "flickering big shadows" (the reader,
    // 2026-10-20). And a full cluster keeps the lights NEAREST IT, not nearest
    // the camera: what it drops is what lights it least.
    const float F = kSliceFar / kSliceNear;
    float zs[kLightSlices + 1];
    zs[0] = 0.0f;
    for (int z = 1; z <= kLightSlices; ++z)
        zs[z] = kSliceNear * std::pow(F, static_cast<float>(z) / static_cast<float>(kLightSlices));
    zs[kLightSlices] = 1e9f;   // the last slice runs on to the clip
    std::vector<float> cx0(static_cast<std::size_t>(out.across)), cx1(cx0.size());
    for (int x = 0; x < out.across; ++x) {
        float p0 = static_cast<float>(x * tile), p1 = static_cast<float>(std::min(pxW, (x + 1) * tile));
        if (cam.flipX) { const float a = static_cast<float>(pxW) - p1, b = static_cast<float>(pxW) - p0; p0 = a; p1 = b; }
        cx0[static_cast<std::size_t>(x)] = (p0 / sx - 1.0f) * tanH;
        cx1[static_cast<std::size_t>(x)] = (p1 / sx - 1.0f) * tanH;
    }
    std::vector<float> cy0(static_cast<std::size_t>(out.down)), cy1(cy0.size());
    for (int y = 0; y < out.down; ++y) {
        const float p0 = static_cast<float>(y * tile), p1 = static_cast<float>(std::min(pxH, (y + 1) * tile));
        cy0[static_cast<std::size_t>(y)] = (1.0f - p1 / sy) * tanV;   // y grows down the screen
        cy1[static_cast<std::size_t>(y)] = (1.0f - p0 / sy) * tanV;
    }
    // `OMK_TILES_LOOSE=1`, an instrument: no box test and no cap - every
    // light in every cluster its padded rectangle and slices cover. The shader
    // gives a light that cannot reach a fragment nothing, so a CORRECT tight
    // test draws the same frame byte for byte (`verify.py: engine: emitter light`)
    static const bool loose = envFlag("OMK_TILES_LOOSE");
    std::vector<std::vector<std::pair<float, std::uint32_t>>> lists(tiles);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Rect& q = rects[i];
        if (q.x0 > q.x1 || q.y0 > q.y1) continue;
        const auto& l = points[i];
        const float d[3] = {l.pos[0] - cam.eye[0], l.pos[1] - cam.eye[1], l.pos[2] - cam.eye[2]};
        const float vx = s[0] * d[0] + s[1] * d[1] + s[2] * d[2];
        const float vy = u[0] * d[0] + u[1] * d[1] + u[2] * d[2];
        const float vz = f[0] * d[0] + f[1] * d[1] + f[2] * d[2];
        const float r2 = l.radiusA * l.radiusA;
        for (int z = q.s0; z <= q.s1; ++z) {
            const float zn = zs[z], zf = std::min(zs[z + 1], vz + l.radiusA + 1.0f);
            const float bz = std::clamp(vz, zn, zf) - vz;
            for (int y = q.y0; y <= q.y1; ++y) {
                const float ylo = std::min(cy0[static_cast<std::size_t>(y)] * zn, cy0[static_cast<std::size_t>(y)] * zf);
                const float yhi = std::max(cy1[static_cast<std::size_t>(y)] * zn, cy1[static_cast<std::size_t>(y)] * zf);
                const float by = std::clamp(vy, ylo, yhi) - vy;
                if (!loose && bz * bz + by * by > r2) continue;
                for (int x = q.x0; x <= q.x1; ++x) {
                    const float xlo = std::min(cx0[static_cast<std::size_t>(x)] * zn, cx0[static_cast<std::size_t>(x)] * zf);
                    const float xhi = std::max(cx1[static_cast<std::size_t>(x)] * zn, cx1[static_cast<std::size_t>(x)] * zf);
                    const float bx = std::clamp(vx, xlo, xhi) - vx;
                    const float dd = bx * bx + by * by + bz * bz;
                    if (!loose && dd > r2) continue;
                    lists[static_cast<std::size_t>(z) * plane + static_cast<std::size_t>(y) * out.across + x]
                        .emplace_back(dd, static_cast<std::uint32_t>(i));
                }
            }
        }
    }
    out.words.assign(4 + 2 * tiles, 0);
    out.words[0] = static_cast<std::uint32_t>(out.across);
    out.words[1] = static_cast<std::uint32_t>(out.down);
    out.words[2] = static_cast<std::uint32_t>(kLightSlices);
    out.words[3] = static_cast<std::uint32_t>(tile);
    for (std::size_t t = 0; t < tiles; ++t) {
        auto& v = lists[t];
        if (!loose && v.size() > static_cast<std::size_t>(kTileCap)) {
            std::nth_element(v.begin(), v.begin() + kTileCap, v.end());
            v.resize(static_cast<std::size_t>(kTileCap));
            ++out.full;
        }
        out.words[4 + 2 * t] = static_cast<std::uint32_t>(out.words.size());
        out.words[4 + 2 * t + 1] = static_cast<std::uint32_t>(v.size());
        for (const auto& [dd, idx] : v) out.words.push_back(idx);
        out.listed += static_cast<long>(v.size());
    }
}

}  // namespace omk
