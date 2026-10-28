// SPDX-License-Identifier: GPL-3.0-or-later
// THE LIVE FRONTEND - a window, a keyboard, and the ported framebuffer.
//
//     omk-play <gamedata> <tables/>                        the interface, live
//     omk-play <gamedata> <tables/> --scene Aapkayl        the SCENE VIEWER
//
// **`backends/sdl/` is the only place in the tree that includes an SDL header**
// - and, behind the gateway (`platform/frontend.h`), not this file:
// only `sdlfront.{h,cpp}` (the window, keyboard, pad and audio device), the
// GPU glue `playgpu_*.cpp` and the `--scene` instrument. This file and the
// rest of the viewer reach the host through `omk::Frontend` - which is
// `docs/PORTING.md` A8 rule 2. (`backends/vulkan/vkrender.cpp` is the only one
// that includes Vulkan's, on the same terms and for the same reason; the rule
// is one dependency per backend file, not one backend file in total.) Rule 3 is the one that is not hygiene, and
// it decides what this file may do: SDL creates a window, reports keys and
// uploads a texture. It does **not** blit, scale, blend, lay out or draw text.
// Every pixel it shows was put there by `blt`, `fillQuad` and `drawRun` - the
// ported drawers, each already checked against the engine's own framebuffer -
// and letting SDL do any of that would make the checks test SDL while staying
// exactly as green.
//
// A8 names SDL3. Only SDL2 is installed on the machine this was written on, and
// the surface used here is a dozen calls that both versions have, so it builds
// against either and the Makefile prefers 3. That divergence is recorded in A8
// rather than left for someone to find.
#include "playshared.h"
#include "playstate.h"
#include "playframe.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif


bool omkInstrumentsBuilt();   // playharness.cpp / playharness_off.cpp

int PlayState::run(int argc, char** argv) {
    // WHICH BUILD THIS IS, first: a console log is the only report a run
    // leaves, and a `--profile` that printed nothing at all could not say
    // whether the release build was installed
    // RELEASE is the profiler's absence (`make release`, `vita-release`,
    // `classic-release`); the classic Mac's development build has no
    // instruments by design and is still a development build
    std::printf("build: %s - the profiler %s, the instruments %s\n",
                OMK_PROFILE ? "development" : "RELEASE",
                OMK_PROFILE ? "compiled in (--profile)" : "compiled OUT (OMK_PROFILE=0)",
                omkInstrumentsBuilt() ? "in" : "out");
    OMK_HEAPCHECK("main");
    // THE SETUP, in order (`playsetup_<section>.cpp`, todo/play-split.md)
    int r = phase::Continue;
    if ((r = setupOptions(argc, argv)) != phase::Continue) return r;
    vrSetup(argc, argv);   // the --vr-* flags (backends/vr; empty without OMK_VR)
    // the profiler (`--profile`, todo/debug-tools.md): the setup is FRAME -1,
    // each section a zone; then one capture frame per `step`
    omk::prof::beginFrame(-1);
    {
        { OMK_ZONE("setup: boot"); if ((r = setupBoot()) != phase::Continue) return r; }
        { OMK_ZONE("setup: session"); if ((r = setupSession()) != phase::Continue) return r; }
        { OMK_ZONE("setup: adventure"); if ((r = setupAdventure()) != phase::Continue) return r; }
        { OMK_ZONE("setup: devices"); if ((r = setupDevices()) != phase::Continue) return r; }
        // THE FILMS BEFORE THE GLOBAL LIBRARY, in `Game_Main`'s order: the
        // window, `Game_Init`, the three movies, THEN `Game_Start
        // ("aventure.scx")` - which `setupBodies` reads - then the splash
        // (`docs/BOOT.md` 1). The films need only the window.
        { OMK_ZONE("setup: play"); if ((r = setupPlay()) != phase::Continue) return r; }
        { OMK_ZONE("setup: bodies"); if ((r = setupBodies()) != phase::Continue) return r; }
        { OMK_ZONE("setup: splash"); if ((r = setupSplash()) != phase::Continue) return r; }
    }
    omk::prof::endFrame();
    // THE FRAME LOOP - `PlayState::step` in `playframe.cpp` (todo/play-split.md).
    for (;;) {
#if OMK_PROFILE
        // the profiler's PAUSE (todo/debug-tools.md step 3): held, the game
        // does not step - it keeps its window alive on the last frame
        if (omk::prof::control(frameNo) == omk::prof::Run::Hold) {
            // the frame on screen is the last one stepped
            if (omk::prof::snapshotOwed()) omk::prof::snapshot(profLastFrame, fb.w, fb.h, fb.px.data());
            if (!profPauseTick()) break;
            continue;
        }
#endif
        const long drawn = frameNo;             // the frame this step draws
        omk::prof::beginFrame(drawn);
        const int stepped = step();
        omk::prof::endFrame();
#if OMK_PROFILE
        profLastFrame = drawn;
        if (omk::prof::snapshotOwed()) omk::prof::snapshot(drawn, fb.w, fb.h, fb.px.data());
#endif
        if (stepped == phase::EndLoop) break;
        if (stepped >= 0) { omk::prof::close(); return stepped; }
    }
    const int rc = finish();
    omk::prof::close();
    return rc;
}

// THE VIEWER, as one object (`todo/play-split.md` S3): what `main` held as
// locals is `PlayState`'s members, and `main`'s body is `PlayState::run`. On
// the heap, as its members are hundreds of containers and caches - the Vita's
// main thread has a small stack.
//
// WINDOWS + SDL2: `sdl2-config --cflags` carries `-Dmain=SDL_main`, and SDL2's
// `libSDL2main` calls `SDL_main` with C linkage. This file includes no SDL
// header, so without SDL_main.h's declaration the renamed `main` would be a C++
// function and the link fails ("undefined reference to `SDL_main'"). Declaring
// it here gives the definition below C linkage; on every other host `main` is
// not a macro and this is compiled out.
#if defined(main) && !defined(OMK_SDL3)
extern "C" int main(int argc, char** argv);
#endif
namespace {
namespace fs = std::filesystem;

bool isGameFolder(const fs::path& d) {
    std::error_code ec;
    if (!fs::is_directory(d, ec)) return false;
    for (const auto& e : fs::directory_iterator(d, ec)) {
        std::string n = e.path().filename().string();
        for (auto& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (n == "IAM" && e.is_directory(ec)) return true;
    }
    return false;
}

// RUN WITHOUT NAMING THE GAME: a release zip is unpacked INTO the game's folder
// and started by double-click, so `omk-play.exe` alone (or with flags only)
// finds the game where it is - beside the exe, else in the current folder, else
// in Steam's default place - and the tables, `omk.ini` and `omk-saves/` beside
// the exe. Returns 0 to go on with `args`, 1 when no game folder was found
// (told to the user in a box: the exe has no console), -1 when the game was
// named and nothing needs doing.
int autoArguments(int argc, char** argv, std::vector<std::string>& args) {
    if (argc >= 2 && argv[1][0] != '-') return -1;
    std::error_code ec;
    fs::path exeDir = fs::path(argc > 0 ? argv[0] : "").parent_path();
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0) exeDir = fs::path(exe).parent_path();
#elif defined(__linux__)
    // started through PATH, argv[0] has no folder
    {
        std::error_code self;
        const fs::path p = fs::read_symlink("/proc/self/exe", self);
        if (!self) exeDir = p.parent_path();
    }
#endif
    if (exeDir.empty()) exeDir = fs::current_path(ec);
    std::vector<fs::path> where = {exeDir, fs::current_path(ec)};
    if (const char* pf = std::getenv("ProgramFiles(x86)"))
        where.push_back(fs::path(pf) / "Steam" / "steamapps" / "common" / "Omikron");
    where.push_back("C:/Program Files (x86)/Steam/steamapps/common/Omikron");
    // Steam on Linux: the usual library, the old symlink, and the Flatpak one
    if (const char* home = std::getenv("HOME")) {
        const fs::path h(home);
        where.push_back(h / ".local/share/Steam/steamapps/common/Omikron");
        where.push_back(h / ".steam/steam/steamapps/common/Omikron");
        where.push_back(h / ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Omikron");
    }
    fs::path game;
    for (const auto& d : where)
        if (isGameFolder(d)) { game = d; break; }
    if (game.empty()) {
#ifdef _WIN32
        MessageBoxA(nullptr,
                    "The game's files were not found.\n\nUnzip this build INTO the game's folder "
                    "(the one that contains Runtime.exe and the IAM folder), then start "
                    "omk-play.exe again.",
                    "Omikron (OMK engine)", MB_OK | MB_ICONERROR);
        return 1;
#else
        if (argc >= 2) return -1;   // flags only: the usage text says what to give
        std::fprintf(stderr,
                     "The game's files were not found.\n\nPut this build INTO the game's folder (the one "
                     "that contains the IAM folder) and run ./omk-play again, or name the folder: "
                     "./omk-play /path/to/Omikron\n");
        return 1;
#endif
    }
    fs::current_path(exeDir, ec);   // the saves and the config are read from here
#ifdef _WIN32
    // The exe has no console: started by double-click (or from a terminal without a
    // redirect) its output would go nowhere, and a report of "it froze" would have
    // nothing behind it. Write the log beside the exe, line by line so a stall keeps
    // everything up to it. A redirect (`omk-play.exe > log.txt`) is left alone.
    for (const DWORD h : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        const HANDLE s = GetStdHandle(h);
        if (s != nullptr && s != INVALID_HANDLE_VALUE) continue;
        const bool out = h == STD_OUTPUT_HANDLE;
        const std::string log = (exeDir / (out ? "omk-play.log" : "omk-play-errors.log")).string();
        if (std::freopen(log.c_str(), "w", out ? stdout : stderr) != nullptr)
            std::setvbuf(out ? stdout : stderr, nullptr, _IOLBF, 4096);
    }
#endif
    const bool tables = fs::exists(exeDir / "tables" / "vm_opcodes.json", ec);
    args.push_back(argc > 0 ? argv[0] : "omk-play");
    args.push_back(game.string());
    args.push_back(tables ? (exeDir / "tables").string() : std::string("tables"));
    bool named = false;
    for (int i = 1; i < argc; ++i) named = named || std::string(argv[i]) == "--config";
    if (!named && fs::exists(exeDir / "omk.ini", ec)) {
        args.push_back("--config");
        args.push_back((exeDir / "omk.ini").string());
    }
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    switch (autoArguments(argc, argv, args)) {
        case 1: return 1;
        case 0: {
            std::vector<char*> v;
            for (auto& a : args) v.push_back(a.data());
            v.push_back(nullptr);
            const auto ps = std::make_unique<PlayState>();
            return ps->run(static_cast<int>(args.size()), v.data());
        }
        default: break;
    }
    const auto ps = std::make_unique<PlayState>();
    return ps->run(argc, argv);
}
