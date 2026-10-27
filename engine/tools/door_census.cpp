// SPDX-License-Identifier: GPL-3.0-or-later
// THE DOOR GRAPH (`Session::doorTargets`, the preload radius - `todo/
// remaster.md`): which areas each area's door zones lead to, read
// statically (`zoneDestinations`), so the preload's DEPTH is chosen from the
// data - a street's door can open on an airlock or a corridor (a "Sas")
// before the room.
//
//     door_census <gamedata> <tables> [area...]
//
// Without areas: one line an area - "@area <id> <set> <set KB> doors <n>
// -> <ids>". With areas: each one's graph breadth first to depth 4 - per
// layer the areas first reached there, with their sets and sizes.
#include "formats/iam.h"
#include "platform/datafs.h"
#include "script/area.h"
#include "script/script.h"
#include "script/world.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct Node { std::string set; long kb = -1; std::vector<int> to; bool ok = false; };

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: door_census <gamedata> <tables> [area...]\n"); return 2; }
    const std::string data = argv[1];
    const omk::DataFs fs(data);
    const auto iam = fs.resolve("IAM");
    if (!iam) { std::fprintf(stderr, "no IAM under %s\n", data.c_str()); return 1; }
    const auto table = omk::OpcodeTable::loadJson(std::string(argv[2]) + "/vm_opcodes.json");
    std::map<int, Node> g;
    for (int a = 0; a < 1024; ++a) {
        const auto chunk = omk::archiveChunk(*iam + "/AREA", a);
        if (chunk.size() < 97) continue;
        Node n;
        n.ok = true;
        for (int k = 0; k < 9; ++k) {
            const char c = static_cast<char>(chunk[88 + static_cast<std::size_t>(k)]);
            if (!c) break;
            n.set += c;
        }
        if (!n.set.empty())
            if (const auto p = fs.resolve("MESHES/DECORS/" + n.set + ".3DO"))
                n.kb = static_cast<long>(omk::DataFs::readPath(*p).size() / 1024);
        for (const omk::Zone& z : omk::zonesOf(chunk, omk::ChunkKind::Area))
            omk::zoneDestinations(z, chunk, table, n.to);
        g[a] = std::move(n);
    }
    if (argc == 3) {
        for (const auto& [a, n] : g) {
            std::printf("@area %d %s %ld doors %zu ->", a, n.set.c_str(), n.kb, n.to.size());
            for (const int t : n.to) std::printf(" %d", t);
            std::printf("\n");
        }
        return 0;
    }
    for (int i = 3; i < argc; ++i) {
        const int root = std::atoi(argv[i]);
        std::set<int> seen = {root};
        std::vector<int> layer = {root};
        std::printf("== area %d (%s, %ld KB)\n", root, g[root].set.c_str(), g[root].kb);
        for (int d = 1; d <= 4 && !layer.empty(); ++d) {
            std::vector<int> next;
            for (const int a : layer)
                for (const int t : g[a].to)
                    if (seen.insert(t).second) next.push_back(t);
            std::printf("  hop %d:", d);
            for (const int t : next) std::printf(" %d %s(%ldK,%zu doors)", t, g[t].set.c_str(), g[t].kb, g[t].to.size());
            std::printf("\n");
            layer = next;
        }
    }
    return 0;
}
