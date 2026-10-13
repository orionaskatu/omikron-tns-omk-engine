// SPDX-License-Identifier: GPL-3.0-or-later
// THE GPU BACKENDS' SHARED CORE AGREES WITH THE REFERENCE PATH.
//
//     drawcore_probe <set.3DO> <character.3DO>...
//
// `o3de/drawcore.h` is what Vulkan, GLES, GL 1.x and citro3d pose and light a
// body with on the CPU; the software reference poses it with `applyPose` and
// lights it with `applyLights`. This drives both over random poses of real
// characters, under the real lights of a real set (the body stood where each
// light reaches it in turn), starting alternately from the baked colour and
// from the scene's grey (`Draw::lightsFromBlack`), and prints per model:
//
//   * pose_err - the largest difference between a corner the core posed from
//     `meshAffines` and the same corner `applyPose` posed, positions and
//     normals: the same rotation reached in two orders, so a few ulps;
//   * light_mismatch - corners whose colour BITS differ between the core
//     lighting the body itself and `applyLights` lighting the core's own
//     unlit pose: the same normals, so the law alone, and exact;
//   * lit - corners the lights changed, so the comparison is not vacuous;
//   * rest_mismatch - corners the GLES variant (only what is posed written,
//     the rest copied once) leaves different from the whole-corner one.
//
// Then `mismatches <total>`. Prints only; writes nothing.
#include "actor/pose.h"
#include "formats/light3do.h"
#include "formats/mesh3do.h"
#include "o3de/drawcore.h"
#include "o3de/geom3do.h"
#include "o3de/vertexlight.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Lcg {
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    std::uint32_t next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(s >> 33);
    }
    float unit() { return static_cast<float>(next()) / 2147483648.0f; }   // [0, 1)
};

std::vector<char> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::span<const std::byte> bytesOf(const std::vector<char>& raw) {
    return {reinterpret_cast<const std::byte*>(raw.data()), raw.size()};
}

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: drawcore_probe <set.3DO> <character.3DO>...\n");
        return 2;
    }
    const std::vector<char> setRaw = readFile(argv[1]);
    const auto sh = omk::readHeader(bytesOf(setRaw));
    if (!sh) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    const std::vector<omk::Light3do> lights = omk::readLights(bytesOf(setRaw), *sh);
    std::printf("set lights %zu\n", lights.size());
    if (lights.empty()) return 1;

    long total = 0;
    for (int a = 2; a < argc; ++a) {
        const std::vector<char> raw = readFile(argv[a]);
        if (raw.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[a]); return 1; }
        const auto md = bytesOf(raw);
        std::string stem = argv[a];
        if (const auto s = stem.find_last_of("/\\"); s != std::string::npos) stem = stem.substr(s + 1);
        const auto mh = omk::readHeader(md);
        if (!mh) { std::printf("%s no header\n", stem.c_str()); continue; }
        const auto meshes = omk::readMeshes(md, *mh);
        const omk::Geometry rest = omk::buildGeometry(md, omk::DrawFilter::Engine);
        if (rest.corners.empty() || rest.cornerMesh.size() != rest.corners.size()) {
            std::printf("%s not a posable model\n", stem.c_str());
            continue;
        }
        Lcg r;
        float poseErr = 0.0f;
        long lightBad = 0, lit = 0, restBad = 0, calls = 0;
        omk::Geometry ref, core, unlit, partial;
        std::vector<float> aff, reach;
        for (int step = 0; step < 48; ++step) {
            std::vector<omk::MeshPose> pose(meshes.size());
            for (auto& mp : pose) {
                float q[4] = {r.unit() - 0.5f, r.unit() - 0.5f, r.unit() - 0.5f, r.unit() - 0.5f};
                const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                if (n > 1e-6f) for (auto& v : q) v /= n; else q[0] = 1.0f;
                mp.q = omk::Quatf{q[0], q[1], q[2], q[3]};
                for (auto& v : mp.pos) v = r.unit() * 200.0f - 100.0f;
            }
            // stand the body inside one light's inner radius, a light a step
            const omk::Light3do& L = lights[static_cast<std::size_t>(step) % lights.size()];
            const float body[3] = {L.pos[0] + 10.0f, L.pos[1], L.pos[2] - 10.0f};
            reach.clear();
            const int nl = omk::lightReach(body, lights, reach);
            const bool fromBlack = step % 2 == 1;
            const float base = 0.25f;

            // the reference: applyPose, then applyLights on its corners
            omk::applyPose(ref, rest, meshes, pose);
            // the core: one affine a mesh, posed and lit as a backend does it
            omk::meshAffines(meshes, pose, nullptr, aff);
            omk::Draw d;
            d.geo = &rest;
            d.start = 0;
            d.count = rest.corners.size();
            d.meshPose = aff.data();
            d.meshPoses = aff.size() / 12;
            d.vertexLights = reach.data();
            d.vertexLightCount = nl;
            d.lightsFromBlack = fromBlack;
            d.lightBase = base;
            omk::poseGeometry(d, core, omk::PoseParts{true, true, 64});
            omk::poseGeometry(d, unlit, omk::PoseParts{true, true, 0});
            if (fromBlack)
                for (auto& c : unlit.corners) c.r = c.g = c.b = base;
            omk::applyLights(unlit, 0, unlit.corners.size(), body, lights);
            // the GLES variant: posed into a geometry copied once, earlier
            if (partial.corners.size() != rest.corners.size()) partial = rest;
            omk::poseGeometry(d, partial, omk::PoseParts{false, true, 0});
            ++calls;
            for (std::size_t i = 0; i < rest.corners.size(); ++i) {
                const omk::Corner& a0 = ref.corners[i];
                const omk::Corner& b0 = core.corners[i];
                const float e[6] = {a0.x - b0.x, a0.y - b0.y, a0.z - b0.z,
                                    a0.nx - b0.nx, a0.ny - b0.ny, a0.nz - b0.nz};
                for (const float v : e) poseErr = std::fmax(poseErr, std::fabs(v));
                const omk::Corner& u = unlit.corners[i];
                if (!sameBits(u.r, b0.r) || !sameBits(u.g, b0.g) || !sameBits(u.b, b0.b)) ++lightBad;
                const omk::Corner& rc = rest.corners[i];
                const float r0 = fromBlack ? base : rc.r;
                if (!sameBits(b0.r, r0) || (!fromBlack && (!sameBits(b0.g, rc.g) || !sameBits(b0.b, rc.b))))
                    ++lit;
                const omk::Corner& p = partial.corners[i];
                if (!sameBits(p.x, b0.x) || !sameBits(p.y, b0.y) || !sameBits(p.z, b0.z) ||
                    !sameBits(p.nx, b0.nx) || !sameBits(p.ny, b0.ny) || !sameBits(p.nz, b0.nz))
                    ++restBad;
            }
        }
        std::printf("%s corners %zu calls %ld pose_err %.6f light_mismatch %ld lit %ld rest_mismatch %ld\n",
                    stem.c_str(), rest.corners.size(), calls, poseErr, lightBad, lit, restBad);
        total += lightBad + restBad + (poseErr > 1e-3f ? 1 : 0);
    }
    std::printf("mismatches %ld\n", total);
    return total == 0 ? 0 : 3;
}
