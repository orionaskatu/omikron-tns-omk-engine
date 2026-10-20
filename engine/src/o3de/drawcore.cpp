// SPDX-License-Identifier: GPL-3.0-or-later
#include "o3de/drawcore.h"

#include "o3de/depthtie.h"
#include "platform/env.h"

#include <algorithm>

namespace omk {

void poseGeometry(const Draw& d, Geometry& out, const PoseParts& parts) {
    if (out.corners.size() != d.geo->corners.size()) out = *d.geo;
    CornerLight lights[64];
    const int nl = prepareLights(d, parts.lights < 64 ? parts.lights : 64, lights);
    const bool lit = relit(d, nl) && parts.lights > 0;
    const bool tint = tinted(d);
    for (std::size_t i = 0; i < out.corners.size(); ++i) {
        const Corner& rc = d.geo->corners[i];
        Corner& c = out.corners[i];
        if (parts.rest) c = rc;
        if (const float* a = cornerAffine(d, i)) {
            float p[3];
            posePoint(a, rc.x, rc.y, rc.z, p);
            c.x = p[0]; c.y = p[1]; c.z = p[2];
            if (parts.normals) {
                poseNormal(a, rc.nx, rc.ny, rc.nz, p);
                c.nx = p[0]; c.ny = p[1]; c.nz = p[2];
            }
        } else if (!parts.rest) {
            c.x = rc.x; c.y = rc.y; c.z = rc.z;
            if (parts.normals) { c.nx = rc.nx; c.ny = rc.ny; c.nz = rc.nz; }
        }
        if (lit) {
            float rgb[3];
            lightStart(d, rc, rgb);
            const float n[3] = {c.nx, c.ny, c.nz};
            lightCorner(n, lights, nl, rgb);
            c.r = rgb[0]; c.g = rgb[1]; c.b = rgb[2];
        } else if (tint && parts.lights > 0) {
            // an unlit body takes the tint on its baked colour, as `scene.vert`
            c.r = rc.r * d.lightTint[0]; c.g = rc.g * d.lightTint[1]; c.b = rc.b * d.lightTint[2];
        }
    }
}

bool tieOff() {
    static const bool off = envFlag("OMK_NO_TIE");
    return off;
}

bool tieBakeOff() {
    static const bool off = tieOff() || envFlag("OMK_NO_TIE_BAKE");
    return off;
}

std::vector<std::size_t> bakeTieLosers(const Geometry& g, std::span<const Draw> order) {
    DepthTie t;
    std::vector<std::size_t> losers, restore;
    for (const Draw& d : order)
        if (d.geo == &g) t.resolve(g, d.start, d.count, d.blend == Blend::Opaque, losers, restore);
    std::sort(losers.begin(), losers.end());
    losers.erase(std::unique(losers.begin(), losers.end()), losers.end());
    return losers;
}

}  // namespace omk
