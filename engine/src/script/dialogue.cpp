// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats/bytes.h"
#include "script/dialogue.h"

#include "formats/adpcm.h"
#include "o3de/worldcam.h"
#include "formats/morph.h"
#include "formats/morphstream.h"
#include "platform/datafs.h"

#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <set>

namespace omk {

const std::vector<std::byte>& DialogPlayer::morph() const {
    static const std::vector<std::byte> none;
    return stream_.bytes() ? *stream_.bytes() : none;
}

const std::vector<std::int16_t>& DialogPlayer::pcm() const { return stream_.pcm(); }

void DialogPlayer::finishLine() {
    if (stream_.valid()) stream_.fillTo(stream_.layout().frames);
}

namespace {

using omk::le::i16;
using omk::le::u32;

}  // namespace

Conversation parseConversation(int id, std::span<const std::byte> b) {
    Conversation c;
    c.id = id;
    if (b.size() < 8) return c;
    c.speaker = i16(b, 0);
    const int nn = i16(b, 2);
    const int nc = i16(b, 4);
    // the loader's own guard: a chunk that does not fit is not a conversation.
    // 321 of the 420 shipped chunks pass it; the rest are something else.
    if (nn <= 0 || nc <= 0 ||
        8u + 64u * static_cast<std::size_t>(nn) + 44u * static_cast<std::size_t>(nc)
            > b.size())
        return c;
    c.cameras = nc;
    // The 44-byte camera records after the nodes. `Dialog_Load` converts them
    // once at load time, so do it here: the six int32s through the same
    // `* 100/256/2.54 - 1` every other authored coordinate takes, and the two
    // angles through 360/4096.
    {
        const auto base = 8u + 64u * static_cast<std::size_t>(nn);
        for (int j = 0; j < nc; ++j) {
            const auto o = base + 44u * static_cast<std::size_t>(j);
            DialogCamera dc;
            for (int k = 0; k < 3; ++k) {
                dc.eye[k] = rawToWorld(static_cast<std::int32_t>(
                    u32(b, o + 4u * static_cast<std::size_t>(k))));
                dc.at[k]  = rawToWorld(static_cast<std::int32_t>(
                    u32(b, o + 12u + 4u * static_cast<std::size_t>(k))));
            }
            dc.id   = i16(b, o + 24);
            // INTEGER DEGREES, in the engine's order. `Dialog_Load` (0x00401800)
            // converts each angle in place - `fild; fmul 0.087890625; _ftol`,
            // TRUNCATED toward zero - and stores it back into the int16, so
            // the fraction is gone (a fov of 74.97 is 74). `Camera_LoadParams`
            // then wraps only the ROLL, once, into (-180, 180]. The order
            // matters for a roll: a raw 4086 is 359, then -1, where wrapping
            // the raw first gives -0.88 and truncating that gives 0. 95 of the
            // 1923 shipped cameras roll differently by half a degree or more.
            const auto deg = [](std::int16_t raw) {
                return static_cast<int>(static_cast<double>(raw) * 0.087890625);
            };
            int roll = deg(i16(b, o + 28));
            if (roll > 180) roll -= 360;
            if (roll <= -180) roll += 360;
            dc.roll = static_cast<float>(roll);
            dc.fov  = static_cast<float>(deg(i16(b, o + 30)));
            // A guard, not the engine's: no shipped camera reaches it (0 of
            // 1923), and a zero fov would divide by tan(0).
            if (dc.fov <= 1.0f) dc.fov = 74.0f;
            dc.subject[0] = static_cast<std::uint16_t>(i16(b, o + 32));
            dc.subject[1] = static_cast<std::uint16_t>(i16(b, o + 34));
            c.cams.push_back(dc);
        }
    }
    c.nodes.reserve(static_cast<std::size_t>(nn));
    for (int j = 0; j < nn; ++j) {
        const auto o = 8u + 64u * static_cast<std::size_t>(j);
        DialogNode n;
        for (int k = 0; k < 9; ++k) n.ptr[k] = u32(b, o + 4u * static_cast<std::size_t>(k));
        for (int k = 0; k < 4; ++k) n.param[k] = i16(b, o + 36u + 2u * static_cast<std::size_t>(k));
        n.id = i16(b, o + 44);
        // +46, ten bytes: the stem of `MORPH/<name>.3DM`, the line's voice.
        for (int k = 0; k < 10; ++k) {
            const auto ch = static_cast<char>(b[o + 46u + static_cast<std::size_t>(k)]);
            if (!ch) break;
            n.name[k] = ch;
        }
        // The REPLY pair comes first in the record, then the LINE pair.
        n.replyCam  = i16(b, o + 56);
        n.replyCam2 = i16(b, o + 58);
        n.lineCam   = i16(b, o + 60);
        n.lineCam2  = i16(b, o + 62);
        c.nodes.push_back(n);
    }
    c.valid = true;
    return c;
}

DialogRun runConversation(const Conversation& c, std::span<const std::byte> b,
                          GameState& state, const OpcodeTable& table,
                          int limit) {
    DialogRun r;
    const int n = static_cast<int>(c.nodes.size());
    std::set<int> seen;
    int at = 0;
    while (at >= 0 && at < n && r.steps < limit) {
        ++r.steps;
        if (seen.count(at)) { r.end = DialogEnd::Cycle; return r; }
        seen.insert(at);
        const auto& node = c.nodes[static_cast<std::size_t>(at)];

        struct Avail { int k; std::uint32_t action; int target; };
        std::vector<Avail> avail;
        for (int k = 0; k < 4; ++k) {
            const int tgt = node.param[k];
            if (tgt < 0 || tgt >= n) continue;
            bool ok = true;
            const auto cond = node.ptr[k];
            if (cond) {
                Interpreter vm(state, table);
                const auto res = vm.run(b, cond);
                ++r.conditionsRun;
                // Dialog_EvalBranchCondition takes the TOP OF STACK
                ok = vm.stackTop().value_or(0) != 0;
                if (res.status != RunStatus::End && res.status != RunStatus::Dialog)
                    ok = false;
            }
            if (ok) avail.push_back({k, node.ptr[4 + k], tgt});
        }
        DialogStep s;
        s.node = at;
        s.nodeId = node.id;
        for (const auto& a : avail) s.available.push_back(a.k);
        r.path.push_back(std::move(s));

        if (avail.empty()) { r.end = DialogEnd::Leaf; return r; }
        const auto& take = avail.front();
        if (take.action) {                      // the chosen branch's action
            Interpreter vm(state, table);
            vm.run(b, take.action);
            ++r.actionsRun;
        }
        at = take.target;
    }
    r.end = (r.steps >= limit) ? DialogEnd::Limit : DialogEnd::OutOfRange;
    return r;
}


// ------------------------------------------------------------ DialogPlayer

std::vector<std::string> DialogPlayer::pool(std::uint32_t at) const {
    // The walk `0x004011D0` performs: NUL-terminated cp1252 strings from the
    // node's `ptr[8]`. Index 0 is the spoken line, 1+k is reply k.
    std::vector<std::string> out;
    if (!at || at >= chunk_.size()) return out;
    std::size_t p = at;
    for (int i = 0; i < 6 && p < chunk_.size(); ++i) {
        std::size_t e = p;
        while (e < chunk_.size() && static_cast<char>(chunk_[e]) != '\0') ++e;
        if (e >= chunk_.size()) break;
        std::string sv;
        for (std::size_t q = p; q < e; ++q) sv.push_back(static_cast<char>(chunk_[q]));
        out.push_back(std::move(sv));
        p = e + 1;
    }
    return out;
}

bool DialogPlayer::open(const Conversation& c, std::span<const std::byte> chunk,
                        const std::string& morphDir) {
    conv_ = c;
    chunk_.assign(chunk.begin(), chunk.end());
    morphDir_ = morphDir;
    visited_.clear();
    stream_.close();
    lines_ = voiced_ = 0;
    phase_ = DialogPhase::Finished;
    if (!c.valid || c.nodes.empty()) return false;
    if (!enter(0)) return false;
    return true;
}

bool DialogPlayer::enter(int node) {
    if (node < 0 || node >= static_cast<int>(conv_.nodes.size())) return false;
    // A conversation is a graph and nothing in the format forbids a cycle, so
    // the guard `runConversation` keeps is kept here too.
    for (int v : visited_) if (v == node) return false;
    visited_.push_back(node);

    const DialogNode& n = conv_.nodes[static_cast<std::size_t>(node)];
    node_ = node;
    lineChanged_ = true;
    lineAt_ = 0.0;
    lineLen_ = 0.0;
    stream_.close();
    loadMs_[0] = loadMs_[1] = 0.0;
    replies_.clear();
    channels_ = 1;
    voice_ = n.name;
    const auto strs = pool(n.ptr[8]);
    line_ = strs.empty() ? std::string() : strs[0];
    ++lines_;
    // `Dialog_TickUI` case 4, at the point it has the asset name: the line's
    // END RULE is a property of that name and of nothing else.
    lineState_ = lineEndFor(voice_);

    if (!voice_.empty()) {
        // THE LINE OPENED, NOT READ (`formats/morphstream.h`): the header and
        // the first second of records, the rest read and decoded by `tick` as
        // the line plays - `Morph_Open` and its 15 ms timer. The line's length
        // is the layout's, known before its voice is decoded.
        const std::string path = morphDir_ + "/" + voice_ + ".3DM";
        if (const auto real = DataFs::findPath(path); real && stream_.open(*real, kOpenFrames)) {
            loadMs_[0] = stream_.readMs();
            loadMs_[1] = stream_.decodeMs();
            channels_ = stream_.channels();
            const std::size_t frames = stream_.totalSampleFrames();
            if (frames) {
                lineLen_ = static_cast<double>(frames) / 22050.0;
                ++voiced_;
            }
        }
    }
    phase_ = DialogPhase::Speaking;
    camFrames_ = 0.0;              // the move belongs to the line
    return true;
}

// Which PAIR is in force. The menu has its own, but only when the node names
// one: `replyCamera !== -1` is the viewer's guard too, and without it a node
// whose reply pair is unset loses its camera the moment the menu opens rather
// than holding the framing the line ended on.
bool DialogPlayer::menuPair() const {
    if (phase_ != DialogPhase::Menu || conv_.nodes.empty()) return false;
    return conv_.nodes[static_cast<std::size_t>(node_)].replyCam != -1;
}

const DialogCamera* DialogPlayer::cameraA() const {
    if (conv_.nodes.empty()) return nullptr;
    const DialogNode& n = conv_.nodes[static_cast<std::size_t>(node_)];
    const int want = menuPair() ? n.replyCam : n.lineCam;
    for (const auto& c : conv_.cams) if (c.id == want) return &c;
    return nullptr;
}

const DialogCamera* DialogPlayer::cameraB() const {
    if (conv_.nodes.empty()) return nullptr;
    const DialogNode& n = conv_.nodes[static_cast<std::size_t>(node_)];
    const int want = menuPair() ? n.replyCam2 : n.lineCam2;
    for (const auto& c : conv_.cams) if (c.id == want) return &c;
    return nullptr;
}

// `Dialog_TickUI` case 4's two string tests. `strcmp` for the first and a
// 7-byte `memcmp` for the second, which over a NUL-padded 10-byte name field
// is the same exact comparison - the seventh byte compared is the terminator,
// so "02E19AB" does NOT match. The engine writes 7 after the first and 8 after
// the second without an `else`; the two literals differ in their first byte,
// so the order cannot matter and this one does not reproduce it.
LineEnd DialogPlayer::lineEndFor(const std::string& asset) {
    if (asset == "02E19A") return LineEnd::SelfEnding;
    if (asset == "125338") return LineEnd::AnyKey;
    return LineEnd::Confirm;
}

// Case 2/7/8's condition, one clause at a time:
//
//     (a2 & 0x10) != 0                                  -> confirm, always
//     dword_9103DC == 7 && (a2 & 0xFFFFFFF3) != 0       -> any bit but 4 / 8
//     dword_9103DC == 8 && Morph_IsDone()               -> no input at all
//
// `Morph_IsDone` (0x0042CBD0) is "the morph this line is playing has finished"
// - the flag `dword_4EA84C`, or a 5000 ms cap in the one fallback mode - and
// the port's clock for that is the decoded voice, so `lineOver()` stands in
// for it. A `SelfEnding` line whose `.3DM` will not load has `lineLen_ == 0`
// and is therefore over on the first tick, which is also what the engine does
// with it: case 4 sets state 3 straight away when `Morph_Play` returns 0.
bool DialogPlayer::cutBy(std::uint32_t input) const {
    if (phase_ != DialogPhase::Speaking) return false;
    if ((input & kConfirmBit) != 0) return true;
    if (lineState_ == LineEnd::AnyKey && (input & kAnyKeyMask) != 0) return true;
    if (lineState_ == LineEnd::SelfEnding && lineOver()) return true;
    return false;
}

void DialogPlayer::tick(double dt) {
    // The camera move runs on the LINE's clock, in frames, and holds at the
    // end of its 160 - which is why a long line stops moving well before it
    // finishes. It keeps running while the menu is up, from the cut.
    camFrames_ += dt * 30.0;
    // The VOICE. When it runs out the line is over and the node stays exactly
    // where it is, because what comes next is the player's...
    if (phase_ == DialogPhase::Speaking && lineAt_ < lineLen_)
        lineAt_ = lineClock_ >= 0.0 ? std::min(lineClock_, lineLen_) : lineAt_ + dt;
    lineClock_ = -1.0;
    // ...and the STREAM kept ahead of it: what `fptc` does every 15 ms, done
    // once a frame - `kAheadSeconds` of records past the clock, read and
    // decoded, so the device never reaches the end of what was decoded.
    // A line cut short is not read further; the next line closes it.
    if (phase_ == DialogPhase::Speaking && stream_.valid() && !stream_.complete())
        stream_.fillTo(static_cast<std::size_t>((lineAt_ + kAheadSeconds) * 30.0) + 1);
    // ...unless the asset says otherwise. `cutBy(0)` is case 2/7/8 with no
    // input word, which only a `SelfEnding` line can satisfy, and the engine's
    // answer to it is state 3 - the line over, walk on - so this does what a
    // press does. One node per tick at most, the same as one press per frame.
    if (phase_ == DialogPhase::Speaking && cutBy(0)) next();
}

void DialogPlayer::next() {
    if (phase_ != DialogPhase::Speaking) return;
    lineAt_ = lineLen_;                       // NEXT cuts a line short
    const DialogNode& n = conv_.nodes[static_cast<std::size_t>(node_)];
    const auto strs = pool(n.ptr[8]);

    replies_.clear();
    int named = 0, firstCont = -1;
    for (int k = 0; k < 4; ++k) {
        DialogReply r;
        r.branch = k;
        r.target = n.param[k];
        r.text = strs.size() > static_cast<std::size_t>(1 + k)
                     ? strs[static_cast<std::size_t>(1 + k)] : std::string();
        if (r.text.empty() && r.target == -1) continue;
        // `Game_HandleEvent` event 55: EVALUATE the condition for a value.
        if (n.ptr[k]) {
            Interpreter vm(*state_, *table_);
            vm.run(chunk_, n.ptr[k]);
            const auto v = vm.stackTop();
            r.available = !(v && *v == 0);
        }
        if (r.available) {
            if (!r.text.empty()) ++named;
            else if (firstCont < 0) firstCont = static_cast<int>(replies_.size());
        }
        replies_.push_back(std::move(r));
    }

    const bool any = std::any_of(replies_.begin(), replies_.end(),
                                 [](const DialogReply& r) { return r.available; });
    if (!any) { phase_ = DialogPhase::Finished; return; }
    // "NEXT with nothing but an unnamed continue branch advances straight to
    // the next line, exactly as in the game" - the viewer's rule. A named
    // reply, even a single one, is a MENU and waits.
    if (!named && firstCont >= 0) {
        choose(replies_[static_cast<std::size_t>(firstCont)].branch);
        return;
    }
    // The cut to the reply pair happens on the press that reveals the menu,
    // so the move's clock restarts with it - WHEN THERE IS ONE.
    // `Dialog_ApplyLineCameras` returns at `request[0] == -1`: a node whose
    // reply pair is unset issues no camera command, and the camera holds
    // where the line's travel left it. Restarting the clock regardless made
    // the line's own pair play its 160-frame travel AGAIN from its first
    // framing - a reader: *the camera suddenly returns to a previous position
    // while continuing the interpolation*.
    phase_ = DialogPhase::Menu;
    if (menuPair()) camFrames_ = 0.0;
}

void DialogPlayer::choose(int branch) {
    if (phase_ == DialogPhase::Finished) return;
    if (branch < 0 || branch > 3) return;
    const DialogNode& n = conv_.nodes[static_cast<std::size_t>(node_)];
    // `Game_HandleEvent` event 59: EXECUTE the chosen branch's action in a
    // throwaway context.
    if (n.ptr[4 + branch]) {
        if (runner_) {
            runner_(runnerSelf_, chunk_, static_cast<std::size_t>(n.ptr[4 + branch]));
        } else {
            Interpreter act(*state_, *table_);
            act.run(chunk_, n.ptr[4 + branch]);
        }
    }
    if (n.param[branch] < 0 || !enter(n.param[branch]))
        phase_ = DialogPhase::Finished;
}

}  // namespace omk
