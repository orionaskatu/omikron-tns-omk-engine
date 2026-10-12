// SPDX-License-Identifier: GPL-3.0-or-later
// `.CTL` control files - a character's animation state machine.
//
// Magic "CE70", loaded by InitCEFFile (the engine's own name for it, from its
// error strings, which also call the file a "bank list"). The whole file is a
// **byte walk**: a fixed header, then entries, then nine variable-length
// sections in a fixed order, each present only when a flag on the entry says
// so. Nothing carries a pointer to the next section - the reader has to add up
// the sizes, which is why the walk landing exactly on the file size is the
// invariant that matters.
//
//     +12          uint32 groupCount
//     +88          group[groupCount], 32 bytes: +4 is its entry count
//     then         entry[...], 88 bytes each, groups in order
//     then, per entry and in THIS order, when its flag is set:
//         name       12 B   unless flags & 0x8002   (an unnamed junction)
//         children    4 B x entry[+87]
//         parents     4 B x entry[+86]
//         turn       24 B   when flags & 0x140
//         rootShift  20 B   when flags & 0x280
//         moveName   12 B   when entry[+8] & 0x10
//         combat     40 B   when flags & 0x2000000
//     then         the fight-AI table: 156 B a profile, 12 situation slots
//                  each of 16 B an item, then the items' input words
//     then         per entry with entry[+76] & 8: a 32-byte-strided block
//     then         the clips: uint32 length + payload, one per distinct name
//
// The trailing pointer fields the format seems to carry are DEAD: InitCEFFile
// never follows them, it recomputes them. Chasing them is what made this look
// unsolvable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace omk {

struct CtlClip {
    std::string name;
    std::size_t offset = 0;
    std::size_t length = 0;
    // How long the clip runs, which the live machine needs on every
    // transition: `GoToMove` writes it into the channel's `+8` and
    // `Cef_TickChannel` compares the frame against it to decide the state is
    // over. A `.CTL` clip is an `.ani` DESCRIPTOR with no "3.0V" wrapper -
    // `sub_45D1F0` hands each block straight to `Anim_RegisterClip` - so this
    // is `animDescriptor(...).frames`, read through the same code path the
    // libraries go through rather than a second guess at the layout.
    std::int32_t frames = 0;
};

// The COMBAT block - 40 bytes, present when the entry's flags carry
// 0x2000000. `Fight_ResolveHit` (0x0049A960) is what names the fields: it
// takes the damage, checks the hit window against the attacker's clock, and
// puts the victim into a reaction state named by the LOW SIXTEEN BITS of a
// state id.
//
// Read as ten floats by the walk, because that is how it is stored - but
// three of them are integers wearing a float's clothing, which the corpus
// settles: damage runs 1..25 over 116 blocks and the two reaction refs
// resolve to a real state in 232 of 232.
struct CtlCombat {
    float raw[10] = {};
    std::int32_t damage() const { return asInt(1); }
    std::int32_t reactionA() const { return asInt(5); }   // -1 = none
    std::int32_t reactionB() const { return asInt(6); }
    std::int32_t asInt(int i) const;
};

// One `+28` sub-record, 32 bytes. Decoded from `Cef_UpdateStateEffects`,
// `Cef_SpawnEffect` and `Cef_TickEffects` (0x0045B260/0x0045B3B0/0x0045ADF0);
// all 590 shipped records carry a sound (525) or a sprite (220) or both.
struct CtlEffect {
    float duration = 0;      // +0  how long a spawned sprite lives
    float from = 0, to = 0;  // +4/+8 the active window; +8 == 0 means open
    float soundAt = 0;       // +12 the frame the sound fires on
    std::uint32_t spriteParam = 0;   // +16
    std::uint16_t sprite = 0;        // +20 the scene's chunk-4 registry
    // +22 the SOUND, and it is an **id**, not an index: `Cef_TickEffects`
    // resolves it with `Scene_FindSoundIndex` (0x0048CC80), which SEARCHES the
    // resident scene's 26-byte chunk-3 records for a matching `+24` and
    // returns that record's `+22` handle. So the same id names different
    // sounds in different scenes - 34 is `AASC.WAV` almost everywhere and
    // `STPR.WAV` in the Impasse - and a state's footstep only sounds where
    // the resident scene carries its id. All 525 shipped references resolve
    // in some scene; none is orphaned.
    std::uint16_t sound = 0;
    std::uint8_t  attach = 0;        // +24 an attach code -> Actor_AttachPoint
    std::uint8_t  flags = 0;         // +25 1 follow the bone, 4 random roll,
                                     //     8 double advance, 0x10 kill on exit
    float scale = 0;                 // +28
};

// AN ENTRY'S FLAGS - the word at entry `+8` (`CtlState::flags`), bit by bit.
// Each name says what its consumer DOES with the bit (`actor/channel.cpp`'s
// `gotoMove` and `findTransition`, `GoToMove` 0x004A7B80 and
// `Cef_TickChannel` 0x004A8160 in the engine; `docs/ASSETS.md` "What the
// flag-gated blocks hold" and the input queue). Where the file's layout
// depends on a bit, the block it gates is named too.
namespace ctl_entry {
inline constexpr std::uint32_t OneShot        = 0x1;         // its clip plays once and then follows its GoTo; without it the clip LOOPS
inline constexpr std::uint32_t Alias          = 0x2;         // adopt its start frame and follow its GoTo
inline constexpr std::uint32_t BlendBreak     = 0x4;         // as a child: may be taken while a blend is running
inline constexpr std::uint32_t StepQueue      = 0x8;         // walks the whole input queue for an edge, not only its front
inline constexpr std::uint32_t MoveName       = 0x10;        // a 12-byte `tab_special_move[]` name at +64, queued on entry
inline constexpr std::uint32_t GroupDefault   = 0x20;        // the group's default entry (`Cef_DefaultEntry`)
inline constexpr std::uint32_t TurnInWindow   = 0x40;        // the turn block applied over the cancel window, per tick
inline constexpr std::uint32_t ShiftInWindow  = 0x80;        // the root-shift block, likewise
inline constexpr std::uint32_t TurnOnLeave    = 0x100;       // the turn block applied WHOLE when the state is left
inline constexpr std::uint32_t ShiftOnLeave   = 0x200;       // the root-shift block, likewise
inline constexpr std::uint32_t LoopEdges      = 0x400;       // a LOOPING state that takes an edge in mid-clip
inline constexpr std::uint32_t DynamicReturn  = 0x800;       // its GoTo rewritten to the entry it was entered from
inline constexpr std::uint32_t QueueHold      = 0x1000;      // a word is committed only while an edge is still open for it
inline constexpr std::uint32_t InheritGlobal  = 0x2000;      // this state also takes its group's GroupGlobal edges
inline constexpr std::uint32_t GroupGlobal    = 0x4000;      // an edge every InheritGlobal state of the group may take
inline constexpr std::uint32_t PassThrough    = 0x8000;      // no clip of its own: crossed through its GoTo chain, blended
inline constexpr std::uint32_t PhaseMatch     = 0x10000;     // entered at the current phase (+80 added): walk -> run keeps the gait
inline constexpr std::uint32_t FullBlend      = 0x20000;     // blend the leaving state's whole +78, not what is left of it
inline constexpr std::uint32_t DropOwnRepeat  = 0x40000;     // a queued word equal to its own input code is dropped
inline constexpr std::uint32_t ExactCode      = 0x80000;     // its input code must match exactly, not as a bitfield
inline constexpr std::uint32_t PopOnEnter     = 0x100000;    // pop the input queue when entered
inline constexpr std::uint32_t MoveEveryTick  = 0x200000;    // with MoveName: the move is queued on every tick, not once
inline constexpr std::uint32_t PopOnLeave     = 0x400000;    // pop the input queue when left
inline constexpr std::uint32_t PopOnLand      = 0x800000;    // pop the input queue when a transition lands on it
inline constexpr std::uint32_t ResetOnEnter   = 0x1000000;   // reset the queue to the idle word when entered
inline constexpr std::uint32_t Combat         = 0x2000000;   // a 40-byte combat block at +52 (`Fight_ResolveHit`)
inline constexpr std::uint32_t ResetOnLeave   = 0x4000000;   // reset the queue when left
inline constexpr std::uint32_t CutNotBlend    = 0x8000000;   // leaving a PassThrough state, cut instead of blending
inline constexpr std::uint32_t ClearLastInput = 0x10000000;  // forget the last committed input word when entered
inline constexpr std::uint32_t QueueOne       = 0x20000000;  // cut the queue to one word; no push behind it
inline constexpr std::uint32_t LatchCode      = 0x40000000;  // record its input code in the channel's 20-slot latch
inline constexpr std::uint32_t MidClip        = 0x80000000;  // as a child: may be taken before the leaving clip has ended

inline constexpr std::uint32_t NoClipName = Alias | PassThrough;        // no 12-byte clip name at +68
inline constexpr std::uint32_t HasTurn    = TurnInWindow | TurnOnLeave;   // a 24-byte turn block at +44
inline constexpr std::uint32_t HasShift   = ShiftInWindow | ShiftOnLeave; // a 20-byte root-shift block at +48
}  // namespace ctl_entry

struct CtlState {
    std::size_t  offset = 0;
    int          group  = 0;
    std::uint32_t id    = 0;
    std::string  name;
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> parents, children;
    std::uint32_t gotoId = 0;
    int          clip = -1;
    bool childOk = true, parentOk = true, gotoOk = true;

    // What `Cef_FindTransition` consumes, in the record itself.
    //
    // `+4` is an INPUT BITFIELD an entry waits for. `0x80000000` is the
    // "no input" sentinel and is NOT the queue's idle word 0x40000000 -
    // `Cef_InputMatches` ends `if (a1 == 0x80000000) return a2 <= 0x2000`, so
    // the idle word matches no idle edge (docs/ASSETS.md).
    std::uint32_t inputCode = 0;         // +4
    // `+12`'s low half is a ROLE code. Six of them - 3, 4, 5, 9, 18, 20 - are
    // what `Fight_Begin` caches, and every combat file carries exactly one
    // entry of each.
    std::uint16_t role = 0;              // +12
    // The CANCEL WINDOW, a pair of times in frames within which a transition
    // may interrupt this entry. 168 entries carry one and none is malformed.
    float cancelFrom = 0.0f, cancelTo = 0.0f;   // +16, +20
    std::uint16_t priority = 0;          // +84, only ever 0, 1 or 2
    bool hasCombat = false;
    CtlCombat combat;

    // ---- what the LIVE machine reads, beyond the data half above --------
    //
    // These are the fields `GoToMove` (0x004A7B80) and `Cef_TickChannel`
    // (0x004A8160) consume on every transition. They were skipped by the walk
    // before because nothing consumed them; `actor/channel.cpp` does.
    std::uint32_t flags12 = 0;           // +12; the low half is `role` above,
                                         //      0x10000000 = knockdown
    float startFrame = 0.0f;             // +24 where playback begins when the
                                         //     state is entered through an
                                         //     alias (flag 2) or a junction
    std::uint16_t playBits = 0;          // +76 bit 3 = has effect records,
                                         //     0x20 = the group default AND
                                         //     scan children in reverse,
                                         //     high nibble = the play mode
    std::uint16_t blendFrames = 0;       // +78 frames to blend on a 0x8000 exit
    std::uint16_t phaseOffset = 0;       // +80 added when 0x10000 phase-matches
    bool hasEffects = false;             // +76 bit 3
    // The state's EFFECT RECORDS - what makes an animation carry its own
    // sound and sprites. `H_WALK` is the case that shows what they are: a
    // pair, one per footfall.
    std::vector<CtlEffect> effects;
    bool hasTurn = false;                // flags & 0x140  -> +44, 24 B
    bool hasShift = false;               // flags & 0x280  -> +48, 20 B
    float turn[6]  = {};                 // start, end, dX, dY, dZ, ?
    float shift[5] = {};                 // start, end, dx, dy, dz
    std::string moveName;                // flags & 0x10 -> tab_special_move[]

    // The edges, RESOLVED to indices into `CtlFile::states`. The file stores
    // ids, and `InitCEFFile`'s link pass turns them into pointers - within the
    // state's own group for parents and children, file-wide for the GoTo. A
    // runtime that re-resolved them per tick would be doing the load pass
    // every frame; more to the point, resolving once is what lets the sweep
    // assert that every landing is a real entry.
    std::vector<int> childIdx, parentIdx;
    int gotoIdx = -1;
};

// One move set. `Cef_FindGroupById` matches `id` - the engine asks for 2
// (falling), 45 (get up), 100 (locomotion, the default), 200 (shoot stance),
// 300 (ladder) and 400 (dialogue stance) by number - and `Cef_DefaultGroup`
// picks the one whose `flags` bit 0 is set, of which there is exactly one per
// file. The combat files keep stale authoring ids and are reached through the
// graph instead.
struct CtlGroup {
    std::int32_t  id = 0;
    std::uint32_t flags = 0;
    int first = 0;      // index into CtlFile::states
    int count = 0;
    int defaultEntry = -1;   // the flag-0x20 entry Cef_DefaultEntry returns
};

// The FIGHT AI, out of the `+76`/`+80` table.
//
// One 156-byte profile per difficulty level, `+0` being the level plus one -
// which is how `sub_45DCB0` picks one.  A "move" is not a single button: it is
// a SEQUENCE of input words that `Fight_TickAI` pushes into the actor's own
// input queue with `Perso_InjectInput`, so the AI drives the same `.CTL` state
// machine the player does, by pressing buttons.  There is no separate AI
// animation path, which is why this belongs in the format reader and not in a
// combat module.
//
//     +0        int32  the id: difficulty level + 1
//     +4,+6     uint16 the delay range entering a fight, in ms
//     +8,+10    uint16 the delay range between moves
//     +12+12k   twelve SITUATION slots, each {int32 weight, int32 count,
//                      ptr moves} - so the COUNT of slot k is at +16+12k,
//                      which is what this reader walks
//
// The slot's first dword is a PERCENTAGE in its low byte, and `Fight_TickAI`
// (0x00464830) and its two helpers `sub_465160` / `sub_465210` do nothing but
// roll `rand() % 100` against it: slot 0's weight gates attacking against
// moving (`u8(profile, 12)`), slots 1..3 and 4..6 are the two move families
// each helper picks from by cumulative weight, 8..10 choose the AI's next
// INTENT, and slot 11 is the special, whose weight is `u8(profile, 144)` with
// its count and pointer at +148/+152. Read here because `actor/fight.cpp`
// needs it; the walk had skipped it, which is why this comment used to
// describe the slot as `{int32 count, ptr, int32}`.
//
// Slot 7 is the one `Fight_TickAI` reads at `+100`/`+104` in its state-6/21
// branch, and that is what fixes both the 12-byte slot stride and the order of
// the two fields.
//
// `0x40000000` is the queue's IDLE word - `Perso_SetInputEnabled` resets the
// queue to exactly it - interleaved between presses so the machine sees a
// release.  It is not the `0x80000000` "no input" edge code: `Cef_InputMatches`
// ends `if (a1 == 0x80000000) return a2 <= 0x2000`, so the idle word matches no
// idle edge.
struct CtlAiSlot {
    // The slot's own PERCENTAGE, the low byte of its first dword: every choice
    // `Fight_TickAI` makes is `rand() % 100` against these, cumulatively
    // within a family. Read because `actor/fight.cpp` needs it.
    std::uint32_t weight = 0;
    std::vector<std::vector<std::uint32_t>> moves;  // each move a word sequence
};

struct CtlAiProfile {
    std::uint32_t id = 0;                 // difficulty level + 1
    std::uint16_t enterDelay[2] = {0, 0};
    std::uint16_t moveDelay[2]  = {0, 0};
    CtlAiSlot slots[12];
};

struct CtlFile {
    bool valid = false;
    std::size_t size = 0;
    std::size_t end  = 0;      // where the walk finished
    bool exact = false;        // end == size: the invariant
    int  groups = 0;
    std::vector<CtlGroup> groupList;
    std::vector<CtlClip>  clips;
    std::vector<CtlState> states;
    std::vector<CtlAiProfile> ai;
};

CtlFile readCtl(std::span<const std::byte> d);

}  // namespace omk
