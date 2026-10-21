// SPDX-License-Identifier: GPL-3.0-or-later
// The walker - one frame of Actor_Move + Walk_GroundResponse.
//
// The order is the engine's: try the move, probe under the result, and let the
// ground decide.
//
// ---------------------------------------------------------------------------
// A DROP IS A FALL, NOT A REFUSAL
//
// Answering a drop past the step limit with `Refused` and leaving the actor
// where he was ("ledges are obeyed") is not what the engine does, and with it
// a player who gets onto a bench can never get off one: measured over three
// shipped sets, such a walker cannot take a single downward step anywhere
// (`engine/tools/stuck_probe.cpp`: 0 descents from 671 / 36 / 342 spots
// standing beside a drop in Aapkayl, AImpasse and Anekbah).
//
// `Walk_GroundResponse` (0x00465460) branches on whether the ground is above
// the feet after the frame's gravity or below them:
//
//   * ABOVE or level - the grounded branch. It snaps the actor up onto it, and
//     this is the only branch carrying a ledge or slope refusal. Both are
//     gated (`(fallByte == 0 || 2) && actor+256 + gap < 0`) on quantities the
//     port does not compute, and a refusal there does not stop the actor: it
//     restores the last safe position and re-issues the SAME delta through
//     `Actor_Move` in mode 4, the collide-and-slide.
//   * BELOW - the airborne branch, and this is the one that matters here. A
//     drop under 7.874 units is absorbed in the frame ONLY in shoot mode
//     (`sub_47CF00`, `setNoAbsorb`); otherwise it sets the fall byte at
//     actor+1304 - 2 under 59.055 (`dword_910350`), a short fall that keeps
//     walking (`shortFall`), and past it ACTOR_STATE 18, tiered at 59.055,
//     118.11 and 196.85. There is no refusal in this branch at all.
//
// So the port falls. The reason the engine rarely needs to is
// `Actor_Move`'s swept sphere (`Sweep_ActorMove` 0x004AD360 ->
// `Sweep_PolygonKernel` 0x004A9D30, 930 lines), which stops the player at a
// wall long before he reaches a balcony edge; the capsule sweep
// (`setBlockers`, `slide`) stands for it here, and `kMaxUnsweptDrop` below,
// the LABELLED refusal that stood in for the missing wall, is retired.
// ---------------------------------------------------------------------------
#pragma once

#include "o3de/collision.h"

#include <span>
#include <string>
#include <vector>

namespace omk {

// The engine's own tunables, in world units (1 unit ~ 2.54 cm). Every one is
// a constant the binary states about itself, at the address named.
inline constexpr double kStepUp   = 11.811023622;  // 30 cm: dword_910340
inline constexpr double kStepDown = 11.811023622;  // the same limit, down
inline constexpr double kTerminal = 787.40155;     // 20 m/s: Actor_ApplyMotion
inline constexpr double kFallHurt = 118.11024;     // 3 m
inline constexpr double kFallKill = 196.85039;     // 5 m

// The drop the ground response absorbs in the frame it happens - a kerb, a
// stair nosing. Anything past it leaves the ground (`Walk_GroundResponse`'s
// `v68 < 7.8740158` snap).
inline constexpr double kSnapDrop = 7.8740158;     // 20 cm

// The lowest tier that changes the actor's state at all: below it the fall
// byte at +1304 is set to 2 and no bank group is asked for; at or above it the
// actor enters ACTOR_STATE 18. `dword_910350`.
inline constexpr double kFallShort = 59.055118;    // 150 cm

// Gravity: added to the actor's vertical speed (+220) every frame and clamped
// at kTerminal, with the frame's descent being `speed * (1/30) * dt`. Written
// into actor+228 by `Actor_LoadModel` (0x0041A730,
// `mov dword ptr [ebx+0E4h], 414DC637h`), so every actor gets it. It is 9.8
// m/s^2 in the engine's own unit - an inch - at 30 Hz.
inline constexpr double kGravity = 12.860892;

// The vertical speed the engine WRITES (not adds) while the actor stands on a
// face steeper than the slope limit, alongside adding the face normal to his
// horizontal velocity. Numerically the same float as the step limit, and from
// the same global - `f32(a1, 220) = dword_910340`.
inline constexpr double kSlideSpeed = 11.811023622;

// **A RECONSTRUCTION, RETIRED: this port's bound, never the game's.**
//
// The engine stops the player at a wall with a swept sphere (`Actor_Move` ->
// `Sweep_ActorMove` 0x004AD360). Without a sweep, a ledge refusal on every
// drop is the walker's only reason to stay out of a stairwell or off a
// balcony; this bound limited that refusal to drops past the engine's own
// no-damage tier (below 3 m the engine falls and the actor is unhurt). The
// capsule (`setBlockers` with the model's sphere list, `slide` with the
// engine's stand-off and push-out) stops a railing or a bench edge the way
// `Actor_Move` stops it, so the guard is RETIRED: `Walk_GroundResponse` has
// no refusal in its below-the-feet branch, a drop is a fall, and this says
// so. Kept as a name so the callers that quote it still compile; it no
// longer refuses.
inline constexpr double kMaxUnsweptDrop = 1.0e9;

enum class StepResult {
    Moved,      // the step took, and the actor snapped to the new floor
    Reverted,   // no floor under the destination - nobody walks into the void
    Blocked,    // a rise past the step limit: a wall, not a stair
    Fell,       // the step took and left the ground: the actor is airborne
    Slid,       // the step landed on a face past the slope limit: he slides
    Refused,    // a drop past kMaxUnsweptDrop - see the note there
};

class Walker {
public:
    Walker(const TriangleSoup& soup, double x, double y, double z)
        : soup_(soup), pos_{x, y, z} {}

    // Walk_ProbeGround: the surface under a point.
    //
    // The ray starts a step-height ABOVE the feet, not at them. Casting from
    // the feet exactly finds nothing - the probe wants a surface strictly
    // below its origin - and every step then reads as a hole; casting from a
    // step above also picks up a rise the actor is allowed to climb, which is
    // the same window the step limit describes.
    std::optional<double> ground(double x, double y, double z) const;

    // THE GROUND GRID (todo/optimization.md step 9): a grid over `soup()` that
    // `ground` walks instead of scanning every triangle - the same answer
    // (`engine: probe grid`, `engine: ground grid`). The grid overload scans
    // anyway while the grid does not match the soup, so a stale pointer costs
    // time, not correctness. Null (the default) is the linear scan.
    void setGrid(const SplitSoupGrid* grid) { grid_ = grid; }
    // THE FLOOR'S MESH FLAGS, one per triangle of `soup()` (`todo/swimming.md`).
    // With them the step refuses a mesh flagged 0x20000000 - `Walk_GroundResponse`'s
    // third step arm, "you may not step onto this" (docs/ASSETS.md 4: a water
    // SURFACE is one) - and `floorFlags()` names what he stands on, which is how
    // a 0x8000000 mesh (the water's banks and bed) is noticed. Null (the
    // default) or a table that does not fit the soup: neither happens.
    void setFloorFlags(const std::vector<std::uint32_t>* flags) { floorFlags_ = flags; }
    std::uint32_t floorFlags() const { return standFlags_; }
    // THE WATER'S PROBES (`todo/swimming.md` 3), both on the floor soup and its
    // flags table; nothing without them.
    // The nearest surface below `(x, from, z)` and its mesh flags.
    std::optional<double> probeFlags(double x, double from, double z,
                                     std::uint32_t& flags) const;
    // The nearest 0x20000000 surface ABOVE `y` (a smaller y) at `(x, z)` - the
    // ray up `sub_4A8F30` casts to find the water over a swimmer.
    std::optional<double> surfaceAbove(double x, double y, double z) const;
    // The floor under the water: the nearest surface below `from` that is not
    // flagged 0x20000000 (the landing's pass-through, factored).
    std::optional<double> floorThroughWater(double x, double from, double z) const;

    // The horizontal half. `dt` is the engine's own frame delta (30/fps, so
    // 1.0 at 30 Hz) and is only used to carry an ALREADY-FALLING actor down
    // this frame, so a caller that has not yet been taught about `tick` still
    // lands him rather than walking him through the air.
    StepResult step(double dx, double dz, double dt = 1.0);

    // One frame of the vertical half - `Actor_ApplyMotion`'s gravity plus
    // `Walk_GroundResponse`'s answer to it.
    //
    // The controller should call this EVERY frame, not only when the actor is
    // moving horizontally: an actor who steps off a ledge and then lets go of
    // the stick is still falling, and `step` is not called when the walk clip
    // stops producing a root delta. It is cheap. Without it he hangs in the air until he asks to move again, which is
    // what `step`'s own `dt` is a floor under rather than a substitute for.
    //
    // ...and NOT a no-op on the ground (todo/drift-audit.md M9):
    // `Walk_GroundResponse` answers the floor every
    // frame, so a grounded actor who does not move is still put on it -
    // snapped UP onto a floor that rose, absorbed DOWN onto one that sank.
    // That is what keeps the player on a lift; see `settle`.
    StepResult tick(double dt);

    // A TELEPORT: `actor.goto_address` writes the actor's position outright
    // (`sub_41BF50`), and the fall accounting starts over from the new floor.
    // `tick`'s GROUNDED half: `Walk_GroundResponse` on a frame with no move.
    StepResult settle(double dt = 1.0);

    void moveTo(double x, double y, double z) {
        pos_[0] = x; pos_[1] = y; pos_[2] = z;
        fall_ = 0.0; vy_ = 0.0; vx_ = 0.0; vz_ = 0.0;
        airborne_ = false; sliding_ = false; jumping_ = false; shortFall_ = false;
        throughSteep_ = false;
    }

    // THE JUMP, `MDJUMP01` (0x0046BD50) - an IMPULSE into the same three
    // velocity fields a fall and a slide already use.
    //
    // `MDJUMP0A` (0x0046BB50) prepares it and `MDJUMP01` writes it:
    //
    //     N   = u32(entry, 12)                 -- the .CTL entry's own field,
    //                                             14 on all five jump entries
    //                                             of H1AVNT and 0 elsewhere
    //     (X, 0, Z) = M_facing . (0, 0, -98.4252)   -- 2.5 m FORWARD
    //     +216 = X / N     +224 = Z / N        -- per FRAME
    //     +220 = -g * N/2                      -- per second, y down so
    //                                             negative is upward
    //
    // The vertical is the ballistic launch for N frames of hang: it reaches
    // zero at the apex on frame N/2 and lands on frame N. It is NOT taken from
    // the rotated vector - only X and Z are - and `MDJUMP0A` corroborates N as
    // a frame count by handing N/2 to `SetITPNbFrames`, which is the binary's
    // own name for it.
    //
    // `vy` is per second and `dx`/`dz` per frame, which is the split
    // `Actor_ApplyMotion` uses (`dx = +216 * dt` against
    // `dy = +220 * 0.0333 * dt`). Refused while already off the ground: the
    // engine's own gate is `dword_6A52CC`, which `MDJUMP0A` sets and only a
    // landing clears.
    bool jump(double vy, double dx, double dz) {
        if (airborne_ || sliding_) return false;
        vy_ = vy; vx_ = dx; vz_ = dz;
        airborne_ = true;
        jumping_ = true;             // dword_6A52CC = 1
        shortFall_ = false;          // the jump's arm bands nothing
        fall_ = 0.0;
        tier_ = 0;
        apex_ = pos_[1];
        return true;
    }

    // THE SHOOT MOVER'S VERTICAL (`actor/shootmove.h`, MDJP): `+248 -=
    // flt_6579C0 x delta` before `Actor_ApplyMotion` tries the frame - the
    // body goes UP by `dy` (y down, so `dy` is negative) outright, and a body
    // that was standing is now above the floor, so the ground response finds
    // him in the air: airborne, falling from rest, apex here.
    void lift(double dy) {
        if (dy == 0.0) return;
        pos_[1] += dy;
        if (dy < 0.0 && !airborne_ && !sliding_) {
            airborne_ = true;
            shortFall_ = true;           // byte 0: the airborne arm bands it
            vy_ = 0.0;
            fall_ = 0.0;
            tier_ = 0;
            apex_ = pos_[1];
        } else {
            fall_ += dy;                 // the fall is the NET descent: the rise counts against it
            if (pos_[1] < apex_) apex_ = pos_[1];   // y grows down: smaller is higher
        }
    }
    // ...and the mover's HORIZONTAL in the air. The shoot mover writes
    // `+244/+252` every frame, airborne or not (flag 0x10 freezes the
    // velocities it comes from, so the run he jumped with carries him), and
    // `Actor_ApplyMotion` hands it to `Actor_Move` - swept, as a walk is.
    // `step` drops a walk's delta in the air ("a FALL IS VERTICAL", the
    // clip's root motion); this is not that delta. -> false on the ground.
    bool airMove(double dx, double dz) {
        if (!airborne_) return false;
        double push[2] = {0.0, 0.0};
        slide(dx, dz, push);
        pos_[0] += dx;
        pos_[2] += dz;
        return true;
    }
    // `sub_47CF00` (0x0047CF00): `dword_6579CC ? flags & 0x810 : 1`. While
    // it is nonzero the ground response's 7.874 ABSORB is off (`21_d3d.c`
    // 2734, `v68 < 7.874 && !sub_47CF00()`), as `dword_6A52CC` turns it off
    // for the adventure leap. `dword_6579CC` is the SHOOTER, written only by
    // `Shoot_Enter` and `Shoot_TickPlayer` (`sub_47CC70`) and cleared by
    // `sub_47CE70`, so OUTSIDE SHOOT MODE IT RETURNS 1 AND NOTHING IS EVER
    // ABSORBED: a drop is banded a short fall (`+1304 = 2`) and gravity takes
    // him down (`todo/scene-gameplay-audit.md` 22). In shoot mode
    // the absorb is on except while the mover's jump flags hold, so a shoot
    // jump is not snapped back onto the floor it has just left
    // (`shootMoveNoAbsorb`). The default is the adventure answer.
    void setNoAbsorb(bool on) { noAbsorb_ = on; }
    bool noAbsorb() const { return noAbsorb_; }

    // The steep faces of the same set - what the actor slides down. Optional:
    // with none installed a steep face is a hole, which is what the walker did
    // before it could see them.
    void setSteep(const TriangleSoup* steep) { steep_ = steep; }
    // THE NARROW PHASE (collision.h): the faces a horizontal move is swept
    // against - every collision face steeper than the walker climbs, the
    // engine's split between "the probe answers with a floor" and "the sweep
    // answers with a wall" - and the sphere's radius. Radius 0 leaves the
    // sweep off, which is how a check shows what it is worth.
    // `centres` are the swept body's sphere centres RELATIVE TO THE FEET
    // (`Actor_Move` builds a capsule from the model's own list at descriptor
    // +244/+248 - HO1_FN: four of radius 10.9 from the feet to the head);
    // empty means one sphere sitting on the feet, the simulator's shape.
    // A grid over the blockers soup for the body sweep (step 11); null keeps
    // the linear scan, and a grid that no longer matches the soup scans too.
    void setBlockerGrid(const SplitSoupGrid* grid) { blockerGrid_ = grid; }
    void setBlockers(const TriangleSoup* blockers, double radius,
                     std::vector<std::array<double, 3>> centres = {}) {
        blockers_ = blockers; radius_ = radius; centres_ = std::move(centres);
    }
    // sweep, clamp, slide - `Actor_Move`'s three passes over `Sweep_ActorMove`
    // and `Walk_ClampNormal`, with the engine's STAND-OFF (a hit at distance
    // d >= 1 moves d - 1) and PUSH-OUT (d < 1: the body is pushed along the
    // clamped normal by 1 - d, re-swept, growing 1.1x until clear).
    // -> (dx, dz) becomes the whole displacement to apply; (0, 0) with a hit
    // is a block. `push` receives the push-out part, already inside dx/dz.
    void slide(double& dx, double& dz, double push[2]);
    int  lastSlides() const { return slides_; }   // passes that hit, last step

    const double* pos() const { return pos_; }
    const TriangleSoup& soup() const { return soup_; }
    double fall() const { return fall_; }
    bool   airborne() const { return airborne_; }
    bool   sliding() const { return sliding_; }
    bool   jumping() const { return jumping_; }   // dword_6A52CC
    // In the air in the SHORT band - `+1304 == 2`, a drop under 59.055
    // (`kFallShort`): no bank group is asked for, so the walk's clip keeps
    // playing and its root motion keeps carrying him (`step`). The airborne
    // arm re-bands every frame while the byte is 0 or 2, so a short fall
    // that finds 59 units under him becomes a banded one, never back.
    bool   shortFall() const { return airborne_ && shortFall_; }

    // The tier the LAST landing arrived in - 0 none, 1 a step, 2 a fall,
    // 3 hurt, 4 killed. The engine reads the same four bands off the drop and
    // asks for .CTL bank group 2, 4 or 5 and ACTOR_STATE 18 or 19.
    int lastLandingTier() const { return tier_; }
    // ...and the FALL DISTANCE it landed with - `+280`, which LABEL_83 zeroes
    // after `Walk_GroundResponse` has read it. The tiers above merge two of the
    // engine's bands (3 m and over); the landing MESSAGES are told apart at 5 m
    // by this, so it is kept rather than recomputed.
    double lastLandingFall() const { return landFall_; }

    // HOW FAR THE LAST LANDING FELL, kept because `land()` zeroes `fall_` and
    // `MDJUMP03` bands the drop on its OWN thresholds, not the walker's four.
    //
    // This is the DESCENT FROM THE APEX, not the net `fall_`. They differ only
    // for a jump, and there the difference matters: `fall_` counts the rise
    // negative, so a level leap nets to 0 - which gives the right band by
    // accident - while a leap off a ledge would come out one apex-height short
    // of the real drop. The descent is what the 1.50 / 3.00 / 5.00 m
    // thresholds are about, and it is never negative.
    double lastLandingDrop() const { return drop_; }

    // `g_IgnoreLedges` - the engine's own global, whose only writer is
    // `sub_41C260` (0x0041C260), a five-line setter with no caller in the
    // decompilation. With it on, both the ledge refusal and the fall are
    // skipped and every drop is snapped.
    bool ignoreLedges = false;

private:
    // Land on `y`, ending an airborne or sliding stretch and banding the drop
    // the way `Walk_GroundResponse` does.
    void land(double y);

    const TriangleSoup& soup_;
    const SplitSoupGrid* grid_ = nullptr;
    const std::vector<std::uint32_t>* floorFlags_ = nullptr;   // per soup triangle
    std::uint32_t standFlags_ = 0;          // the mesh flags of the floor he stands on
    const TriangleSoup* steep_ = nullptr;
    const TriangleSoup* blockers_ = nullptr;
    const SplitSoupGrid* blockerGrid_ = nullptr;
    double radius_ = 0.0;
    std::vector<std::array<double, 3>> centres_;
    int    slides_ = 0;
    // the earliest hit of every sphere of the body swept by `d` from feet `p`
    std::optional<SweepHit> bodyHit(const double p[3], const double d[3]) const;
    double pos_[3];
    double fall_ = 0.0;
    double vy_   = 0.0;
    double vx_   = 0.0;   // the slide's horizontal velocity (+216 / +224)
    double vz_   = 0.0;
    bool   airborne_ = false;
    bool   sliding_  = false;
    // `dword_6A52CC` - set by `MDJUMP0A` and cleared by the landing
    // (`MDJUMP03`). `Walk_GroundResponse` runs its above-the-surface arm - the
    // 7.874 absorb and the fall tiers - only `if (!dword_6A52CC)`, so while it
    // is set a leap is never snapped back onto the floor it is still above.
    bool   jumping_  = false;
    bool   noAbsorb_ = true;     // `sub_47CF00` nonzero: always outside shoot mode
    bool   shortFall_ = false;   // `+1304` 0 or 2 while airborne (see `shortFall`)
    bool   throughSteep_ = false;   // this fall passes the ramps (`airFrame`'s void edge)
    // Leave the ground `drop` above the floor, in the frame: the gravity
    // `Actor_ApplyMotion` adds before the ground response is spent at once
    // (`a5`), and the rest (`v68`) bands the fall.
    StepResult leaveGround(double drop, double dt);
    // `tick`'s airborne half; the frame began at (fromX, fromZ), where a
    // move that finds nothing under it is put back
    StepResult airFrame(double dt, double fromX, double fromZ);
    int    tier_ = 0;
    double landFall_ = 0.0;
    double drop_ = 0.0;          // the last landing's descent from its apex
    double apex_ = 0.0;          // the highest y reached in this airborne stretch
};

// `Walk_ProbeGround`'s OTHER answer: which decor is under the point. The
// probe runs over every decor in state 2 and `Game_HandleEvent` case 9 fires
// when the owner changes (0x00467030 raises it; `sub_459AA0` too), which is
// what moves the ACTIVE row between two resident areas - not the load
// (todo/pending/T11.md, finding 2). With two shown sets the nearest floor
// below the step window decides, the same window `Walker::ground` casts from.
struct DecorSoup {
    int area = -1;                        // the AREA id the decor belongs to
    const TriangleSoup* soup = nullptr;   // its walkable soup
};
// -> the area of the nearest floor under (x, y, z), or -1 when no shown decor
// has one there. Game Y grows downward, so the nearest is the SMALLEST y.
int decorUnder(std::span<const DecorSoup> decors, double x, double y, double z);
// The same answer from `merged` - the decors' soups concatenated in the same
// order, which is how the viewer builds the player's soup - through its grid:
// the floor's first triangle names the decor, and the earliest decor wins a
// tie exactly as the loop above keeps the first strictly nearer. Falls back to
// the loop unless every decor has an area and the sizes add up to `merged`.
int decorUnder(std::span<const DecorSoup> decors, const TriangleSoup& merged,
               const SplitSoupGrid& grid, double x, double y, double z);

// `OMK_VERIFY_GROUND`: every grid answer above also computed the linear way
// and compared, bit for bit. Off by default; the counts are cumulative.
struct GroundVerify { long walker = 0, walkerBad = 0, decor = 0, decorBad = 0,
                      sweep = 0, sweepBad = 0; };
void setGroundVerify(bool on);
const GroundVerify& groundVerify();

// `sweepSphere` through `grid` when there is one - the body and camera sweeps
// (step 11) - counted against the linear scan under `OMK_VERIFY_GROUND`.
std::optional<SweepHit> sweepThrough(const TriangleSoup& tris, const SplitSoupGrid* grid,
                                     const double p0[3], const double d[3], double radius);

}  // namespace omk
