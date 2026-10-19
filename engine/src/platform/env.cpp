// SPDX-License-Identifier: GPL-3.0-or-later
// The `OMK_*` switches: the table and the one reader (`env.h`).
#include "platform/env.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace omk {

namespace {

// EVERY SWITCH, in `strcmp` order (the lookup is a binary search, and a
// `static_assert` below refuses an unsorted table). A check named in brackets
// is the `verify.py` check that sets it. "A/B" is a switch that puts back the
// way something was done before, so the two can be laid side by side; a
// "log" prints lines and changes nothing else.
constexpr const char* kSwitches[] = {
    "OMK_ACTIVE_POOL_ONLY",     // A/B: a staged body's program looked up in the active pool alone - the old, wrong lookup (engine: walk-in scene)
    "OMK_ANIMBLEND_LOG",        // log: the player's drawn pose every frame - state, clip, frame, the largest mesh rotation since the last frame, the blend (engine: anim blend)
    "OMK_BODYLOG",            // log: every staged body's line every frame, culled or not (engine: beat handover)
    "OMK_CAMEYE",               // log: the resolved camera eye every frame (camera travel)
    "OMK_CAMLOG",               // =1 log: transitions, camera requests and moves, scene ops, zone prunes, and the camera as drawn (engine: player move)
    "OMK_CAM_OBSTRUCT_PROBE",   // log: one line a frame from the camera obstruction test, `sub_417070` (engine: camera obstruction)
    "OMK_CLIPLOG",              // log: a frame's clip-distance test against the staged bodies, for hunting a flicker
    "OMK_CPU_MIRROR",           // A/B: the mirror through the CPU read-back fallback instead of the stencil
    "OMK_CPU_MOTION",           // A/B: the moving meshes posed on the CPU on a renderer that poses them (engine: dirty corners)
    "OMK_CPU_PLAYER",           // A/B: the player posed on the CPU on a renderer that poses bodies
    "OMK_CPU_SKY",              // A/B: the sky moved on the CPU on a renderer that moves it
    "OMK_D3D12_DEBUG",          // the D3D12 debug layer on, every message printed (engine: d3d12)
    "OMK_DIRTY_AUDIT",          // GLES: checks the dirty-corner list is complete on every partial upload
    "OMK_DOF_LOG",              // =1 D3D12: the depth of field's focus each frame (a character or the picture centre) (engine: post chain)
    "OMK_EAGER_SOUPS",          // A/B: every collision soup built at the load instead of when first asked (engine: lazy collision)
    "OMK_EVENTS",               // the `@evt` lines beside the prose (`platform/events.h`): machine-readable records for the checks
    "OMK_FADELOG",              // =1 log: the screen fade's terms when they change (engine: fight letterbox)
    "OMK_FEETLOG",              // =<actor> log: that body's lowest drawn corner against the floor, every 30 frames (engine: astaroth floor)
    "OMK_FIGHT_REFUSE",         // the frontend refuses every fight, so a script's wait for event 2 shows (engine: fight park)
    "OMK_FORCE_CINE",           // =1 D3D12: the post chain treats every frame as cinematic, so depth of field runs on it (engine: post chain)
    "OMK_FRAMEGEN_ALWAYS",      // D3D12 framegen: an in-between frame whatever the display's room (engine: framegen)
    "OMK_FRAMEGEN_PROBE",       // =<prefix> D3D12 framegen: frames N-2 and N interpolated and laid against N-1, a line a frame (engine: framegen)
    "OMK_FRAMEGEN_TIMING",      // log: the D3D12 frame generation's GPU time (timestamps), every 60 frames
    "OMK_FSR_DEBUG",            // the FSR context made with the SDK's own parameter checks (engine: upscaler)
    "OMK_GL1",                  // =0: the GL1 build draws with the software reference (engine: gl1 backend)
    "OMK_GL1_COMPOSITE",        // GL1: the interface composed on the CPU instead of drawn as an overlay
    "OMK_GL1_NO_SKIP",          // GL1: the whole-mesh frustum skip off, to show it changes no pixel (engine: gl1 backend)
    "OMK_GLES_NO_STATE_CACHE",  // GLES: the draw-state cache off (engine: gles state cache)
    "OMK_GLES_WINDUMP",         // =<path> GLES: the window's own picture written on the last frame (engine: gles overlay)
    "OMK_GPU_PRESENT_STATS",    // log: the GPU present's counts every 30 frames (engine: gpu present)
    "OMK_HDR_FORCE",            // D3D12, hdr = on: the scRGB swapchain even on a display that reports no HDR (a test of the path)
    "OMK_HIDDEN_WINDOW",        // the GPU window created hidden, the present kept - for profiling (engine: gles overlay)
    "OMK_KOTRACE",              // =1 log: a knocked-out fighter's channel, tick by tick
    "OMK_LBLOG",                // =1 log: which term holds the letterbox bars, when it changes
    "OMK_LOAD_DELAY_MS",        // =<ms> the set-loading job sleeps first - a slow card, desktop only
    "OMK_LOAD_GATE",            // the load gated on the set's arrival on a desktop, as on the Vita
    "OMK_LOOK_ALL",             // every staged actor turns his head to the player
    "OMK_MARKS_MS",             // =<ms> a frame longer than this names its slowest sections (default 150)
    "OMK_MESH_AT",              // =<mesh> log: that mesh's corners as the set's buffer holds them; also keeps moving meshes on the CPU (engine: node rest)
    "OMK_MOTION_STATS",         // log: the D3D12 upscaler's motion vectors summed up every frame (engine: upscaler)
    "OMK_MOVING_GRID",          // A/B: the ground grid rebuilt every moving frame instead of grouped per mesh
    "OMK_NOCLOUD",              // the start menu's cloud left out of a screen render (engine: screen scale)
    "OMK_NOUI",                 // the interface left out of the frame - the HUD, the bars, the fight gauges (engine: fight letterbox)
    "OMK_NO_AMBIENT_CLAMP",     // A/B: a set's ambient floor not clamped (engine: ambient clamp)
    "OMK_NO_CAM_COLLIDE",       // the follow camera's collision with the set off (engine: camera collision)
    "OMK_NO_CULL",              // both faces drawn - the back-face cull off (engine: back-face cull)
    "OMK_NO_DIRTY",             // A/B: whole uploads instead of the dirty corners (engine: dirty corners)
    "OMK_NO_FX_GATE",           // A/B: every particle drawn, none gated by distance (engine: particle gate)
    "OMK_NO_GEOMETRY_RELEASE",  // GLES: a destroyed geometry's GPU state kept (engine: geometry release)
    "OMK_NO_GPU_PRESENT",       // the GPU window hidden and every frame read back - a measurement run (engine: grey bank)
    "OMK_NO_GROUND_GRID",       // A/B: the floor probe as a linear scan instead of through the grid (engine: ground grid)
    "OMK_NO_LOAD_GATE",         // the Vita's load gate off
    "OMK_NO_MIRROR",            // a mirror drawn as a flat pane instead of reflecting
    "OMK_NO_NEON",              // the generic loop instead of the NEON one, for comparing
    "OMK_NO_OVERLAY",           // the interface composed on the CPU instead of drawn as an overlay (engine: gles overlay)
    "OMK_NO_PED_LOD",           // A/B: every walker on its largest skeleton
    "OMK_NO_PROGSPIN",          // diagnostic: a scene call's Euler not applied to the body
    "OMK_NO_ROOTSPIN",          // diagnostic: a scene clip's root motion not turned by the call's Euler (engine: scene facing)
    "OMK_NO_SIDECULL",          // A/B: the staged-body side cull off (engine: unlimited clip)
    "OMK_NO_STREAM",            // GLES: the streamed upload off where it is on by default
    "OMK_NO_SWEEP_GRID",        // A/B: the body and camera sweeps as a linear scan instead of through the grids
    "OMK_NO_TIE",               // the coincident-face depth tie off (engine: sign tie)
    "OMK_NO_TIE_BAKE",          // A/B: the depth tie per frame instead of baked (engine: gles tie bake)
    "OMK_NO_ZONE_BAND",         // a trigger zone's height band not tested
    "OMK_PANEL_CHECK",          // 3DS: the bottom-screen panel checks what it draws
    "OMK_PED_LOD_MAX",          // =<n> no walker skeleton past level n (default 3)
    "OMK_PLY",                  // log: the player's posed height span every frame, =<n> every n; keeps him posed on the CPU (engine: player vertical)
    "OMK_POOL_PAD",             // =<n> n spare copies of texture slot 0, to push the pool past 64 (engine: pool past 64)
    "OMK_POOL_TWICE",           // A/B: textures dropped and re-sent on an arrival, the old order
    "OMK_POST_MESSAGE",         // =<msg>@<frame> posts that message on that frame (engine: mirror park)
    "OMK_POST_TIMING",          // log: the D3D12 post chain's GPU time (timestamps), every 60 frames
    "OMK_PRESENT_CHECK",        // GLES: the row-by-row present compared with the whole one
    "OMK_PRESENT_ROWS",         // GLES: the present row by row always - the Vita's path on a desktop
    "OMK_PROGYAW_NEG",          // diagnostic: a scene call's Euler applied with its sign turned
    "OMK_ROOTSPIN_NEG",         // diagnostic: a scene clip's root delta turned the other way, the body left alone
    "OMK_RT_NO_INDIRECT",       // A/B: raytracing without the indirect light on characters (engine: raytracing)
    "OMK_RT_NO_SHADOW_RAYS",    // A/B: raytracing without the lamps' shadow rays
    "OMK_RT_REFL_TUNE",         // rt reflections: =<strength>,<F0>,<reach>,<roughness>,<wet share>,<emissive boost>, by eye
    "OMK_RT_TUNE",              // raytracing: six numbers replacing the indirect/occlusion/lamp tuning, by eye
    "OMK_SHADOWLOG",            // log: the shadows' light, the player's feet and a walker's shadow, periodically (engine: character shadow)
    "OMK_SKIN_FAR",             // every staged body posed, in view or not
    "OMK_SKIP_EFFECT",          // =<n> set-piece effect n left unspawned
    "OMK_SOFT_RANGE",           // =<units> soft particles: the fade's depth range (default 24), for judging
    "OMK_SSAO_VIEW",            // the SSAO occlusion itself written over the frame, grey on white
    "OMK_STAGE_PROBE",          // log: the staged bodies' placement every frame, every body posed (engine: scene facing)
    "OMK_STICKY_EULER",         // A/B: a scene call's Euler kept across steps - the old, wrong reading (engine: scene facing)
    "OMK_STREAM",               // GLES: the streamed upload on where it is off by default
    "OMK_SWIMTRACE",            // log: the swimmer's probes every tick
    "OMK_SYNC_SETS",            // A/B: a set loaded on the frame's own thread instead of a job
    "OMK_TEXTURE_MEMORY",       // =<bytes> GL1: the free texture memory to size the textures by (engine: gl1 backend)
    "OMK_TIE_LOG",              // log: the depth tie's decisions per upload (engine: gles tie bake)
    "OMK_TIE_STATS",            // log: how each large geometry's revisions were answered, every 30 frames (engine: dirty corners)
    "OMK_TRACE_ACTOR",          // =<actor> log: that body's placement and pose source every frame (program placement)
    "OMK_TRACE_MOTION",         // log: each moving mesh's motion as it is applied
    "OMK_TRANSIENT_MOTIONS",    // A/B: a scene's mesh motions per tick - the old, wrong reading (engine: node rest)
    "OMK_TUNNEL",               // =1 log: the transition refusals that lost the tunnel's far door
    "OMK_UPLOAD_LOG",           // log: every whole vertex upload, by geometry and size
    "OMK_VERIFY_GPU_PRESENT",   // the GPU present's picture checked against the CPU's (engine: gpu present)
    "OMK_VERIFY_GROUND",        // the floor probe through the grid and the scan both, counted (engine: ground grid)
    "OMK_VERIFY_PATCH",         // the full merge beside the in-place one, compared bit for bit (engine: patch index)
    "OMK_VERIFY_SPLIT",         // the two-layer grid against the linear scan every moving frame (engine: split grid)
    "OMK_VMTRACE",              // log: every instruction a script context executes, with its status
    "OMK_VOL_TIMING",           // log: the D3D12 volumetric lamp light's GPU time (timestamps), every 60 frames
    "OMK_VOL_TUNE",             // =<density>,<core>,<glow> volumetrics: the air's density, a lamp's core and glow (shares of its reach), by eye
    "OMK_VRLOG",                // =1 log: the VR camera's head and aim (engine: vr modes)
    "OMK_WEATHER_SKIP_MS",      // =<ms> the weather's clock jumps that far on frame 1 (engine: weather)
    "OMK_WINDUMP",              // =<path> D3D12/Vulkan: the window's picture (RGBA8; D3D12's back buffer) written on the last frame (engine: colour depth)
};
constexpr int kCount = static_cast<int>(sizeof kSwitches / sizeof kSwitches[0]);

constexpr int compare(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}
constexpr bool sorted() {
    for (int i = 1; i < kCount; ++i)
        if (compare(kSwitches[i - 1], kSwitches[i]) >= 0) return false;
    return true;
}
static_assert(sorted(), "env.cpp's switch table must be in strcmp order, without repeats");

// The environment as the first read found it.
struct Snapshot {
    std::string value[kCount];
    bool set[kCount] = {};
    Snapshot() {
        for (int i = 0; i < kCount; ++i)
            if (const char* e = std::getenv(kSwitches[i])) { set[i] = true; value[i] = e; }
    }
};

int indexOf(const char* name) {
    int lo = 0, hi = kCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int c = std::strcmp(name, kSwitches[mid]);
        if (c == 0) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

}  // namespace

const char* envStr(const char* name) {
    const int i = indexOf(name);
    if (i < 0) {
        std::fprintf(stderr, "env: %s is not in src/platform/env.cpp's table - read uncached\n", name);
        return std::getenv(name);
    }
    static const Snapshot snap;
    return snap.set[i] ? snap.value[i].c_str() : nullptr;
}

bool envFlag(const char* name) { return envStr(name) != nullptr; }

bool envOn(const char* name) {
    const char* e = envStr(name);
    return e && *e == '1';
}

long envInt(const char* name, long unset) {
    const char* e = envStr(name);
    return e ? std::atol(e) : unset;
}

double envNum(const char* name, double unset) {
    const char* e = envStr(name);
    return e ? std::atof(e) : unset;
}

}  // namespace omk
