// SPDX-License-Identifier: GPL-3.0-or-later
// The Carbon frontend - see carbonfront.h.
#include "carbonfront.h"
#include "platform/profile.h"     // OMK_PROFILE, before it is tested
#if OMK_PROFILE
#  include "heapcount.h"
#endif

#if defined(OMK_GL1_AGL)
#  include "../gl1/gl1host.h"
#  include <agl.h>
#  include <gl.h>
#endif
#include "platform/datafs.h"      // hostPath, the films' HFS path

#include <AppleEvents.h>
#include <Files.h>
#include <Gestalt.h>
#include <Movies.h>
#include <Events.h>
#include <MacWindows.h>
#include <QDOffscreen.h>
#include <Quickdraw.h>
#include <Sound.h>
#include <TextUtils.h>
#include <Timer.h>
#include <ToolUtils.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

namespace omk {
namespace {

// Mac virtual key code -> the engine's DIK code: the SAME keys
// `backends/sdl/sdlfront.cpp`'s `keymap()` maps, and for the same reasons
// (the interface's vocabulary, the viewer's keys, the adventure bindings);
// see the comments there. Option is the Mac's Alt (DIK_LMENU, which skips
// all three films). A Mac keyboard's right shift and right control report
// their own codes only where the keyboard distinguishes them.
struct KeyRow { unsigned char mac, dik; };
constexpr KeyRow kKeys[] = {
    {0x7E, 0xC8}, {0x7D, 0xD0}, {0x7B, 0xCB}, {0x7C, 0xCD},   // arrows
    {0x24, 0x1C}, {0x31, 0x39}, {0x30, 0x0F}, {0x35, 0x01},   // return space tab escape
    {0x3A, 0x38},                                             // option = alt
    {0x0D, 0x11}, {0x00, 0x1E}, {0x01, 0x1F}, {0x02, 0x20},   // W A S D
    {0x0C, 0x10}, {0x0E, 0x12}, {0x25, 0x26}, {0x23, 0x19},   // Q E L P
    {0x21, 0x1A}, {0x1E, 0x1B},                               // [ ]
    {0x38, 0x2A}, {0x09, 0x2F}, {0x2E, 0x32},                 // left shift, V, M
    {0x3C, 0x36}, {0x3E, 0x9D}, {0x3B, 0x1D},                 // right shift, right/left control
};

bool keyHeld(const KeyMap& km, unsigned code) {
    return (reinterpret_cast<const unsigned char*>(km)[code >> 3] >> (code & 7)) & 1;
}

std::uint64_t microseconds() {
    UnsignedWide us;
    Microseconds(&us);
    return (static_cast<std::uint64_t>(us.hi) << 32) | us.lo;
}

// Quit from the application menu (Mac OS X) or the Finder: the 'quit' Apple
// event, which is how a Carbon application is asked to stop.
bool g_quitRequested = false;
pascal OSErr onQuit(const AppleEvent*, AppleEvent*, long) {
    g_quitRequested = true;
    return noErr;
}

// A buffer has PLAYED: the `callBackCmd` queued after it names its index. On
// Mac OS 9 this runs at interrupt time, so it sets one flag and nothing more;
// the main thread refills (`refillAudio`).
pascal void soundPlayed(SndChannelPtr chan, SndCommand* cmd) {
    reinterpret_cast<volatile bool*>(chan->userInfo)[cmd->param2] = true;
}

}  // namespace

namespace {
CarbonFrontend* g_carbon = nullptr;     // the one frontend, for gl1AglContext

#if defined(OMK_GL1_AGL)
// THE UPLOAD FORMATS a composed frame can go to the card in. Which one a
// driver takes fastest is the driver's business: Mac OS X's take 32-bit
// ARGB (BGRA, 8-8-8-8-REV) and 16-bit ARGB 1555 without converting, and an
// RGB565 or an RGBA-bytes source may go through a software conversion texel
// by texel. On the reader's iBook (Rage 128 Pro, Mac OS X 10.2) a 565 frame
// cost 220-310 ms a menu frame to present (2026-10-09). So `probeUploads`
// TIMES each when the context is made and the presents use the fastest.
struct UpFmt { GLint internal; GLenum format, type; int bytes; const char* name; };
const UpFmt kUiFmts[3] = {
    {GL_RGB5,    GL_RGB,  GL_UNSIGNED_SHORT_5_6_5,       2, "RGB565"},
    {GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2, "ARGB1555"},
    {GL_RGBA8,   GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,   4, "ARGB8888"},
};
bool g_overlayArgb = false;              // the overlay's pick (`gl1OverlayArgb`)

// `n` RGB565 pixels into format `f` at `out` (format 0 is the 565 itself).
// 1555 drops green's lowest bit; 8888 replicates the high bits down, as the
// rest of the port widens a 565 colour.
void convert565(int f, const std::uint16_t* in, std::size_t n, std::uint8_t* out) {
    if (f == 1) {
        auto* o = reinterpret_cast<std::uint16_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned p = in[i];
            o[i] = static_cast<std::uint16_t>(0x8000u | ((p >> 1) & 0x7FE0u) | (p & 0x1Fu));
        }
    } else if (f == 2) {
        auto* o = reinterpret_cast<std::uint32_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned p = in[i];
            const unsigned r5 = (p >> 11) & 31, g6 = (p >> 5) & 63, b5 = p & 31;
            o[i] = 0xFF000000u | (r5 << 3 | r5 >> 2) << 16 | (g6 << 2 | g6 >> 4) << 8 | (b5 << 3 | b5 >> 2);
        }
    }
}

// One quad over the window's `ww x wh`, texture rows top-down to `s1 x t1`.
void drawFrameQuad(int ww, int wh, float s1, float t1) {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);   glVertex2f(0, static_cast<float>(wh));
    glTexCoord2f(s1, 0);  glVertex2f(static_cast<float>(ww), static_cast<float>(wh));
    glTexCoord2f(s1, t1); glVertex2f(static_cast<float>(ww), 0);
    glTexCoord2f(0, t1);  glVertex2f(0, 0);
    glEnd();
}
#endif
}

CarbonFrontend::~CarbonFrontend() {
    close();
    if (g_carbon == this) g_carbon = nullptr;
}

bool CarbonFrontend::openAudio(int rate, int channels) {
    if (chan_) return true;
    static SndCallBackUPP played = NewSndCallBackUPP(soundPlayed);
    SndChannelPtr ch = nullptr;
    const OSErr e = SndNewChannel(&ch, sampledSynth, channels == 2 ? initStereo : initMono, played);
    if (e != noErr || !ch) {
        lastError_ = "SndNewChannel " + std::to_string(e);
        return false;
    }
    ch->userInfo = reinterpret_cast<long>(played_);
    chan_ = ch;
    refills_ = 0;                                  // counted per device opened
    arate_ = rate;
    achan_ = channels;
    const std::size_t frames = static_cast<std::size_t>(rate / 10);
    for (int i = 0; i < kBuffers; ++i) {
        buf_[i].assign(sizeof(ExtSoundHeader) + frames * static_cast<std::size_t>(channels) * 2, 0);
        played_[i] = true;
    }
    refillAudio();
    return true;
}

// The films open the device at their 44100 and the world reopens it at 22050
// (`Frontend::reopenAudio`): closed first, everything queued dropped.
bool CarbonFrontend::reopenAudio(int rate, int channels) {
    if (chan_ && arate_ == rate && achan_ == channels) return true;
    closeAudio();
    mix_.clear();
    return openAudio(rate, channels);
}

void CarbonFrontend::closeAudio() {
    if (!chan_) return;
    // what proves the device played: a buffer is refilled only after the
    // Sound Manager has called back to say it played the last one
    const long after = refills_ > kBuffers ? refills_ - kBuffers : 0;
    std::printf("audio: Sound Manager at %d Hz, %d channel(s) - %ld buffers queued, %ld of "
                "them after the device said one had played (%.1f s)\n",
                arate_, achan_, refills_, after, after * 0.1);
    SndDisposeChannel(static_cast<SndChannelPtr>(chan_), true);   // quietly, now
    chan_ = nullptr;
}

void CarbonFrontend::refillAudio() {
    if (!chan_) return;
    auto* ch = static_cast<SndChannelPtr>(chan_);
    const std::size_t frames = static_cast<std::size_t>(arate_ / 10);
    mixed_.resize(frames * static_cast<std::size_t>(achan_));
    for (int i = 0; i < kBuffers; ++i) {
        if (!played_[i]) continue;
        mix_.mix(mixed_.data(), mixed_.size());
        auto* h = reinterpret_cast<ExtSoundHeader*>(buf_[i].data());
        std::memset(h, 0, sizeof *h);
        h->samplePtr = nullptr;                  // the samples follow the header
        h->numChannels = static_cast<unsigned long>(achan_);
        h->sampleRate = static_cast<UnsignedFixed>(static_cast<unsigned long>(arate_) << 16);
        h->encode = extSH;
        h->baseFrequency = kMiddleC;
        h->numFrames = static_cast<unsigned long>(frames);
        h->sampleSize = 16;                      // signed, big-endian: the host's own
        auto* dst = reinterpret_cast<std::int16_t*>(h->sampleArea);
        for (std::size_t k = 0; k < mixed_.size(); ++k)
            dst[k] = static_cast<std::int16_t>(mixed_[k] * 32767.0f);   // already in [-1, 1]
        played_[i] = false;
        ++refills_;
        SndCommand c;
        c.cmd = bufferCmd; c.param1 = 0; c.param2 = reinterpret_cast<long>(h);
        SndDoCommand(ch, &c, true);
        c.cmd = callBackCmd; c.param1 = 0; c.param2 = i;
        SndDoCommand(ch, &c, true);
    }
}

void CarbonFrontend::queueAudio(std::span<const float> s) {
    if (s.empty()) return;
    if (!chan_) {
        if (!droppedToldOnce_) {
            droppedToldOnce_ = true;
            std::printf("audio: no device - the stream is dropped, not queued\n");
        }
        return;
    }
    mix_.queue(s);
    refillAudio();
}

int CarbonFrontend::playSound(std::span<const float> s, bool loop, float gain) {
    if (s.empty()) return -1;
    return playSound(std::make_shared<const std::vector<float>>(s.begin(), s.end()), loop, gain);
}
int CarbonFrontend::playSound(std::vector<float>&& s, bool loop, float gain) {
    if (s.empty()) return -1;
    return playSound(std::make_shared<const std::vector<float>>(std::move(s)), loop, gain);
}
int CarbonFrontend::playSound(std::shared_ptr<const std::vector<float>> s, bool loop, float gain) {
    return mix_.play(std::move(s), loop, gain);
}
int CarbonFrontend::playSound(std::shared_ptr<const omk::DeviceSound> s, bool loop, float gain) {
    return mix_.play(std::move(s), loop, gain);
}

// The films pace their video by this (the audio clock), so it refills first.
double CarbonFrontend::queuedSeconds() {
    refillAudio();
    return arate_ > 0 ? static_cast<double>(mix_.queued()) / (arate_ * achan_) : 0.0;
}

bool CarbonFrontend::open(int w, int h, const std::string& title) {
    InitCursor();
    static AEEventHandlerUPP quitUPP = NewAEEventHandlerUPP(onQuit);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, quitUPP, 0, false);
    ::Rect r;
    SetRect(&r, 40, 60, 40 + w, 60 + h);
    Str255 t;
    c2pstrcpy(t, title.c_str());
    WindowRef win = NewCWindow(nullptr, &r, t, true, noGrowDocProc,
                               reinterpret_cast<WindowRef>(-1), true, 0);
    if (!win) return false;
    win_ = win;
    SetPortWindowPort(win);
    // AN INSTRUMENT: `omk.quit` beside the application, holding a number of
    // seconds, ends the run by itself after that long, as Cmd-Q would - so a
    // run on Mac OS 9 (no shell, nothing to send it a quit) still closes its
    // audio device and writes its last lines. Absent, it does nothing.
    if (std::FILE* q = std::fopen("omk.quit", "r")) {
        double secs = 0.0;
        if (std::fscanf(q, "%lf", &secs) == 1 && secs > 0.0) {
            quitAt_ = microseconds() + static_cast<std::uint64_t>(secs * 1e6);
            std::printf("quit: omk.quit - the run ends after %.0f s\n", secs);
        }
        std::fclose(q);
    }
    return true;
}

bool CarbonFrontend::pump(HostInput& out) {
    refillAudio();
    out.text.clear();
    EventRecord ev;
    // everything waiting, without sleeping: the frame loop is the clock
    while (WaitNextEvent(everyEvent, &ev, 0, nullptr)) {
        switch (ev.what) {
        case keyDown: case autoKey: {
            const char c = static_cast<char>(ev.message & charCodeMask);
            if ((ev.modifiers & cmdKey) && (c == 'q' || c == 'Q')) {
                if (!quit_) std::printf("quit: Cmd-Q\n");
                quit_ = true;
                break;
            }
            // the characters `WM_CHAR` carries: printable text, and the four
            // control characters the name field reads (sdlfront.cpp, charmap)
            if (c >= 32 && c < 127) out.text += c;
            else if (c == 8 || c == 9 || c == 13 || c == 27) out.text += c;
            else if (c == 3) out.text += '\r';     // the keypad's enter
            break;
        }
        case mouseDown: {
            WindowRef w = nullptr;
            const short part = FindWindow(ev.where, &w);
            if (w == win_ && part == inDrag) {
                ::Rect bounds;
                GetRegionBounds(GetGrayRgn(), &bounds);
                DragWindow(w, ev.where, &bounds);
#if defined(OMK_GL1_AGL)
                if (agl_) aglUpdateContext(static_cast<AGLContext>(agl_));
#endif
            } else if (w == win_ && part == inGoAway) {
                if (TrackGoAway(w, ev.where)) {
                    if (!quit_) std::printf("quit: the window's close box\n");
                    quit_ = true;
                }
            }
            break;
        }
        case updateEvt:
            if (reinterpret_cast<WindowRef>(ev.message) == win_) {
                BeginUpdate(static_cast<WindowRef>(win_));
                blit();
                EndUpdate(static_cast<WindowRef>(win_));
            }
            break;
        case kHighLevelEvent:
            AEProcessAppleEvent(&ev);
            break;
        default:
            break;
        }
    }
    if (g_quitRequested && !quit_) {
        std::printf("quit: a Quit Apple event (the Finder, the Dock, or another program)\n");
        quit_ = true;
    }
    if (quitAt_ && !quit_ && microseconds() >= quitAt_) {
        quit_ = true;
        std::printf("quit: omk.quit's time is up\n");
    }
    out.quit = quit_;

    // the keys HELD, from the keyboard's own state - not from key-up events,
    // which classic Mac OS does not deliver by default
    out.held.clear();
    KeyMap km;
    GetKeys(km);
    for (const KeyRow& k : kKeys)
        if (keyHeld(km, k.mac)) out.held.insert(k.dik);
    out.mouse.clear();
    if (Button()) out.mouse.insert(12);
    out.mouseDX = out.mouseDY = 0.0f;
    return !out.quit;
}

void CarbonFrontend::present(const Surface& fb) {
    if (!win_ || fb.w <= 0 || fb.h <= 0) return;
    if (agl_) {
        presentGL(fb);
#if OMK_PROFILE
        classic_heap_sample();
#endif
        return;
    }
    if (!gw_ || gwW_ != fb.w || gwH_ != fb.h) {
        if (gw_) DisposeGWorld(static_cast<GWorldPtr>(gw_));
        gw_ = nullptr;
        ::Rect b;
        SetRect(&b, 0, 0, static_cast<short>(fb.w), static_cast<short>(fb.h));
        // AT THE WINDOW'S DEPTH: a GWorld of another depth makes CopyBits
        // convert every pixel of every frame (Mac OS X windows are 32 bits;
        // a 1999 Mac in thousands of colours is 16), so the one conversion
        // is the 565 one below. The films dropped a third of their frames
        // on Tiger with a 16-bit GWorld.
        const int depth = GetPixDepth(GetPortPixMap(GetWindowPort(static_cast<WindowRef>(win_)))) >= 24 ? 32 : 16;
        GWorldPtr gw = nullptr;
        if (NewGWorld(&gw, static_cast<short>(depth), &b, nullptr, nullptr, 0) != noErr || !gw) return;
        gw_ = gw;
        gwW_ = fb.w;
        gwH_ = fb.h;
        gwDepth_ = depth;
    }
    PixMapHandle pm = GetGWorldPixMap(static_cast<GWorldPtr>(gw_));
    if (!LockPixels(pm)) return;
    char* base = GetPixBaseAddr(pm);
    const long rowBytes = GetPixRowBytes(pm);
    for (int y = 0; y < fb.h; ++y) {
        const std::uint16_t* src = fb.px.data() + static_cast<std::size_t>(y) * fb.w;
        if (gwDepth_ == 16) {
            // RGB565 -> xRGB1555, native (big-endian) words: red and green
            // shift down one, green's low bit is dropped, blue stays
            auto* row = reinterpret_cast<std::uint16_t*>(base + y * rowBytes);
            for (int x = 0; x < fb.w; ++x)
                row[x] = static_cast<std::uint16_t>(((src[x] & 0xFFC0) >> 1) | (src[x] & 0x1F));
        } else {
            // RGB565 -> xRGB8888, each channel widened by bit replication -
            // what the 16-bit target looks like expanded, the colours unchanged
            auto* row = reinterpret_cast<std::uint32_t*>(base + y * rowBytes);
            for (int x = 0; x < fb.w; ++x) {
                const std::uint32_t v = src[x];
                const std::uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                row[x] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            }
        }
    }
    UnlockPixels(pm);
    blit();
#if OMK_PROFILE
    classic_heap_sample();   // the memory budget's low-water mark, once a frame
#endif
}

void CarbonFrontend::blit() {
    if (!win_ || !gw_ || agl_) return;       // under GL the next frame redraws
    CGrafPtr port = GetWindowPort(static_cast<WindowRef>(win_));
    SetPort(port);
    ForeColor(blackColor);   // CopyBits colourises with the port's colours
    BackColor(whiteColor);
    PixMapHandle pm = GetGWorldPixMap(static_cast<GWorldPtr>(gw_));
    if (!LockPixels(pm)) return;
    ::Rect src, dst;
    SetRect(&src, 0, 0, static_cast<short>(gwW_), static_cast<short>(gwH_));
    GetPortBounds(port, &dst);
    CopyBits(GetPortBitMapForCopyBits(static_cast<GWorldPtr>(gw_)),
             GetPortBitMapForCopyBits(port), &src, &dst, srcCopy, nullptr);
    UnlockPixels(pm);
    QDFlushPortBuffer(port, nullptr);   // Mac OS X buffers the window; OS 9 draws through
}

void CarbonFrontend::close() {
    filmClose();
    if (qtState_ == 1) { ExitMovies(); qtState_ = 0; }
    closeAudio();
#if defined(OMK_GL1_AGL)
    if (agl_) {
        AGLContext ctx = static_cast<AGLContext>(agl_);
        agl_ = nullptr;
        if (aglGetCurrentContext() == ctx) aglSetCurrentContext(nullptr);
        aglSetDrawable(ctx, nullptr);
        aglDestroyContext(ctx);   // the frame's texture goes with it
        uiTex_ = 0;
        uiTexW_ = uiTexH_ = 0;
        filmTex_ = 0;
        filmTexW_ = filmTexH_ = 0;
    }
#endif
    if (gw_) { DisposeGWorld(static_cast<GWorldPtr>(gw_)); gw_ = nullptr; }
    if (win_) { DisposeWindow(static_cast<WindowRef>(win_)); win_ = nullptr; }
}

std::uint32_t CarbonFrontend::ticksMs() {
    return static_cast<std::uint32_t>(microseconds() / 1000);
}
std::uint64_t CarbonFrontend::perfCounter() { return microseconds(); }

// No sleep call that is both in CarbonLib and fine-grained: whole ticks
// (1/60 s) with `Delay`, the rest by watching the microsecond clock.
void CarbonFrontend::delayMs(std::uint32_t ms) {
    const std::uint64_t end = microseconds() + static_cast<std::uint64_t>(ms) * 1000;
    for (;;) {
        const std::uint64_t now = microseconds();
        if (now >= end) break;
        if (end - now > 20000) {
            unsigned long fin = 0;
            Delay(1, &fin);
        }
    }
}

std::string CarbonFrontend::windowTitle() const {
    if (!win_) return {};
    Str255 t;
    GetWTitle(static_cast<WindowRef>(win_), t);
    return std::string(reinterpret_cast<const char*>(t + 1), t[0]);
}

void CarbonFrontend::setWindowTitle(const std::string& title) {
    if (!win_) return;
    Str255 t;
    c2pstrcpy(t, title.c_str());
    SetWTitle(static_cast<WindowRef>(win_), t);
}

// ---- OPENGL through AGL ------------------------------------------------------

void* CarbonFrontend::glContext() {
#if defined(OMK_GL1_AGL)
    if (agl_ || !win_) return agl_;
    // a hardware renderer first; else whatever renders (Mac OS 9 under
    // emulation has only Apple's software one)
    const GLint accel[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 16,
                           AGL_ACCELERATED, AGL_NO_RECOVERY, AGL_NONE};
    const GLint any[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 16, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(nullptr, 0, accel);
    const bool accelerated = pf != nullptr;
    if (!pf) pf = aglChoosePixelFormat(nullptr, 0, any);
    if (!pf) {
        std::printf("display: no AGL pixel format (%s)\n",
                    reinterpret_cast<const char*>(aglErrorString(aglGetError())));
        return nullptr;
    }
    AGLContext ctx = aglCreateContext(pf, nullptr);
    aglDestroyPixelFormat(pf);
    if (!ctx) return nullptr;
    if (!aglSetDrawable(ctx, GetWindowPort(static_cast<WindowRef>(win_)))) {
        std::printf("display: aglSetDrawable failed (%s)\n",
                    reinterpret_cast<const char*>(aglErrorString(aglGetError())));
        aglDestroyContext(ctx);
        return nullptr;
    }
    aglSetCurrentContext(ctx);
    // the surface brought up to the window now, and one black frame shown, so
    // the window holds nothing stale before the first world is drawn
    aglUpdateContext(ctx);
    probeUploads();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    aglSwapBuffers(ctx);
    agl_ = ctx;
    std::printf("display: an AGL context on the window (%s); the frame is presented "
                "as a textured quad and aglSwapBuffers from here on\n",
                accelerated ? "accelerated" : "NOT accelerated");
#endif
    return agl_;
}

void CarbonFrontend::presentGL(const Surface& fb) {
#if defined(OMK_GL1_AGL)
    // A frame the world is not swapped straight for - the interface alone,
    // the films, a composite - as ONE TEXTURED QUAD. It was `glDrawPixels`
    // with `glPixelZoom`, which the drivers of the period run in software:
    // on a real iBook G3 under Mac OS X 10.2 (Rage 128) the menus were far
    // slower than the city, whose world never takes this path (the reader,
    // 2026-10-09). The frame stays in RGB565, the packed type in the host's
    // own word order - the one `glDrawPixels` already read correctly on
    // OS 9 and Tiger - into a 16-bit texture, and only the RUNS OF ROWS
    // whose content changed since they were last sent are uploaded (a hash
    // a row, the GL1 overlay's): a menu's text and frame stand still while
    // only its animated parts move.
    AGLContext ctx = static_cast<AGLContext>(agl_);
    const AGLContext prev = aglGetCurrentContext();
    if (prev != ctx) aglSetCurrentContext(ctx);
    ::Rect b;
    GetPortBounds(GetWindowPort(static_cast<WindowRef>(win_)), &b);
    const int ww = b.right - b.left, wh = b.bottom - b.top;
    // the 2D state for one quad; the renderer sets all of its own in begin(),
    // its texture cache included
    glViewport(0, 0, ww, wh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, ww, 0, wh, -1, 1);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_LIGHTING);
    glDrawBuffer(GL_BACK);
    int tw = 1, th = 1;
    while (tw < fb.w) tw <<= 1;
    while (th < fb.h) th <<= 1;
    GLuint tex = static_cast<GLuint>(uiTex_);
    if (!tex || uiTexW_ != tw || uiTexH_ != th) {
        if (!tex) glGenTextures(1, &tex);
        uiTex_ = tex;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        const UpFmt& f = kUiFmts[uiFmt_];
        glTexImage2D(GL_TEXTURE_2D, 0, f.internal, tw, th, 0, f.format, f.type, nullptr);
        uiTexW_ = tw;
        uiTexH_ = th;
        uiRowKey_.assign(static_cast<std::size_t>(th), 0);   // nothing sent yet
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    const UpFmt& fmt = kUiFmts[uiFmt_];
    glPixelStorei(GL_UNPACK_ALIGNMENT, fmt.bytes == 4 ? 4 : 2);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    OMK_ZONE("present: rows");               // the profiler (todo/debug-tools.md)
    int runStart = -1;
    for (int y = 0; y <= fb.h; ++y) {
        bool changed = false;
        if (y < fb.h) {
            const std::uint16_t* src = fb.px.data() + static_cast<std::size_t>(y) * fb.w;
            std::uint32_t h0 = 2166136261u, h1 = 0x9E3779B9u;
            int x = 0;
            for (; x + 2 <= fb.w; x += 2) {
                h0 = (h0 ^ src[x]) * 16777619u;
                h1 = (h1 ^ src[x + 1]) * 16777619u;
            }
            for (; x < fb.w; ++x) h0 = (h0 ^ src[x]) * 16777619u;
            const std::uint32_t key = (h0 ^ (h1 * 3u)) | 1u;     // 0 = never sent
            std::uint32_t& slot = uiRowKey_[static_cast<std::size_t>(y)];
            changed = slot != key;
            slot = key;
        }
        if (changed && runStart < 0) runStart = y;
        if (!changed && runStart >= 0) {                         // a run ends: send it
            const std::uint16_t* src = fb.px.data() + static_cast<std::size_t>(runStart) * fb.w;
            const std::size_t n = static_cast<std::size_t>(fb.w) * static_cast<std::size_t>(y - runStart);
            const void* send = src;
            if (uiFmt_ != 0) {
                if (uiConv_.size() < n * 4) uiConv_.resize(n * 4);
                convert565(uiFmt_, src, n, uiConv_.data());
                send = uiConv_.data();
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, runStart, fb.w, y - runStart, fmt.format,
                            fmt.type, send);
            runStart = -1;
        }
    }
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    // texture row 0 is the frame's TOP, drawn at the window's top (GL's y up)
    const float s1 = static_cast<float>(fb.w) / tw, t1 = static_cast<float>(fb.h) / th;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);   glVertex2f(0, static_cast<float>(wh));
    glTexCoord2f(s1, 0);  glVertex2f(static_cast<float>(ww), static_cast<float>(wh));
    glTexCoord2f(s1, t1); glVertex2f(static_cast<float>(ww), 0);
    glTexCoord2f(0, t1);  glVertex2f(0, 0);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    {
        OMK_ZONE("present: swap");
        aglSwapBuffers(ctx);
    }
    if (prev != ctx) aglSetCurrentContext(prev);
#else
    (void)fb;
#endif
}

#if defined(OMK_GL1_AGL)
// What the window's drawable and the card are, and how long each upload
// format takes to send a 640x480 frame AND draw it - a driver may defer a
// texture's upload to its first use, so the draw and a `glFinish` are inside
// the time. Best of four, the frame changed between them. A format the
// driver refuses (a GL error) is never picked.
void CarbonFrontend::probeUploads() {
    GLint rb = 0, gb = 0, bb = 0, ab = 0, db = 0;
    glGetIntegerv(GL_RED_BITS, &rb); glGetIntegerv(GL_GREEN_BITS, &gb);
    glGetIntegerv(GL_BLUE_BITS, &bb); glGetIntegerv(GL_ALPHA_BITS, &ab);
    glGetIntegerv(GL_DEPTH_BITS, &db);
    std::printf("display: the drawable is %d-%d-%d bits (alpha %d), depth buffer %d bits%s\n",
                static_cast<int>(rb), static_cast<int>(gb), static_cast<int>(bb),
                static_cast<int>(ab), static_cast<int>(db),
                rb <= 5 ? " - the display is in thousands of colours" : " - millions of colours");
    ::Rect b;
    GetPortBounds(GetWindowPort(static_cast<WindowRef>(win_)), &b);
    const int ww = b.right - b.left, wh = b.bottom - b.top;
    const int W = 640, H = 480, TW = 1024, TH = 512;
    std::vector<std::uint16_t> frame(static_cast<std::size_t>(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            frame[static_cast<std::size_t>(y) * W + x] = static_cast<std::uint16_t>(x * 37 ^ y * 101);
    std::vector<std::uint8_t> conv(static_cast<std::size_t>(W) * H * 4);
    std::vector<std::uint32_t> rgba(static_cast<std::size_t>(W) * H);   // the overlay's source
    glViewport(0, 0, ww, wh);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, ww, 0, wh, -1, 1);
    glMatrixMode(GL_TEXTURE); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_ALPHA_TEST); glDisable(GL_FOG);
    glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    // -> ms for one send-and-draw, best of four; -1 when the driver refused it
    const auto timeOne = [&](GLint internal, GLenum format, GLenum type, int bytes,
                             const std::function<const void*()>& source, bool blend) {
        while (glGetError() != GL_NO_ERROR) {}
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, TW, TH, 0, format, type, nullptr);
        bool ok = glGetError() == GL_NO_ERROR;
        if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_SRC_ALPHA); }
        else glDisable(GL_BLEND);
        double best = 1e30;
        for (int rep = 0; rep < 4 && ok; ++rep) {
            frame[static_cast<std::size_t>(rep)] ^= 0x0841u;
            glFinish();
            const std::uint64_t t0 = microseconds();
            const void* src = source();
            glPixelStorei(GL_UNPACK_ALIGNMENT, bytes == 4 ? 4 : 2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, format, type, src);
            drawFrameQuad(ww, wh, static_cast<float>(W) / TW, static_cast<float>(H) / TH);
            glFinish();
            const double ms = static_cast<double>(microseconds() - t0) / 1000.0;
            if (ms < best) best = ms;
            ok = glGetError() == GL_NO_ERROR;
        }
        glDisable(GL_BLEND);
        glDeleteTextures(1, &t);
        return ok ? best : -1.0;
    };
    double ui[3];
    int pick = -1;
    for (int f = 0; f < 3; ++f) {
        const UpFmt& u = kUiFmts[f];
        ui[f] = timeOne(u.internal, u.format, u.type, u.bytes, [&]() -> const void* {
            if (f == 0) return frame.data();
            convert565(f, frame.data(), frame.size(), conv.data());
            return conv.data();
        }, false);
        if (ui[f] >= 0 && (pick < 0 || ui[f] < ui[pick])) pick = f;
    }
    uiFmt_ = pick < 0 ? 0 : pick;
    argbOk_ = ui[2] >= 0;                    // the films' layout (`filmShow`)
    film1555_ = ui[1] >= 0 && (ui[2] < 0 || ui[1] < ui[2]);
    // the overlay: RGBA bytes (what it sent) against ARGB 8888, both 8-bit
    // alpha and blended as the overlay draws; the conversion is in the time
    const auto fillRgba = [&](bool argb) -> const void* {
        for (std::size_t i = 0; i < rgba.size(); ++i) {
            const unsigned p = frame[i];
            const unsigned r = (p >> 8) & 0xF8, g = (p >> 3) & 0xFC, bl = (p << 3) & 0xF8, a = p & 0xFF;
            if (argb) rgba[i] = a << 24 | r << 16 | g << 8 | bl;
            else {
                auto* o = reinterpret_cast<std::uint8_t*>(&rgba[i]);
                o[0] = static_cast<std::uint8_t>(r); o[1] = static_cast<std::uint8_t>(g);
                o[2] = static_cast<std::uint8_t>(bl); o[3] = static_cast<std::uint8_t>(a);
            }
        }
        return rgba.data();
    };
    const double ovBytes = timeOne(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4,
                                   [&]() { return fillRgba(false); }, true);
    const double ovArgb = timeOne(GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 4,
                                  [&]() { return fillRgba(true); }, true);
    g_overlayArgb = ovArgb >= 0 && (ovBytes < 0 || ovArgb < ovBytes);
    glDisable(GL_TEXTURE_2D);
    std::printf("gl1: upload probe, a 640x480 frame sent and drawn (ms, best of 4; -1 refused): "
                "RGB565 %.1f, ARGB1555 %.1f, ARGB8888 %.1f -> the menus as %s; overlay RGBA "
                "bytes %.1f, ARGB8888 %.1f -> %s\n", ui[0], ui[1], ui[2], kUiFmts[uiFmt_].name,
                ovBytes, ovArgb, g_overlayArgb ? "ARGB8888" : "RGBA bytes");
}
#endif

// ---- THE BOOT FILMS THROUGH QUICKTIME ------------------------------------
//
// What a 1999 Mac port would have done (`todo/classic-mac-port-1999.md`:
// QuickTime for the MPEG-1 films), and what the iBook needs: the software
// decoder showed ~0.5 fps there (2026-10-09, `GAME.mpg: 82 of 115 frames
// dropped`) - every picture decoded by pl_mpeg, doubled on the CPU and
// sent as a whole 640x480 frame. QuickTime decodes and paces the film and
// plays its sound itself; only the picture it drew is sent, at its own
// 320x240, and the card doubles it (GL_NEAREST: the same replication as the
// CPU's exact 2x).

namespace {
pascal OSErr filmDrawn(::Movie, long refCon) {
    reinterpret_cast<CarbonFrontend*>(refCon)->filmDrawn_ = true;
    return noErr;
}
}  // namespace

bool CarbonFrontend::filmOpen(const std::string& hostFile) {
    if (!win_ || qtState_ < 0) return false;
    if (qtState_ == 0) {
        long version = 0;
        if (Gestalt(gestaltQuickTime, &version) != noErr) {
            std::printf("film: no QuickTime - the software decoder plays the films\n");
            qtState_ = -1;
            return false;
        }
        const OSErr e = EnterMovies();
        if (e != noErr) {
            std::printf("film: EnterMovies failed (%d) - the software decoder plays the films\n", e);
            qtState_ = -1;
            return false;
        }
        std::printf("film: QuickTime %lx entered\n", static_cast<unsigned long>(version) >> 16);
        qtState_ = 1;
    }
    const std::string hp = hostPath(hostFile);
    Str255 pname;
    c2pstrcpy(pname, hp.c_str());
    FSSpec spec;
    OSErr e = FSMakeFSSpec(0, 0, pname, &spec);
    short ref = 0;
    if (e == noErr) e = OpenMovieFile(&spec, &ref, fsRdPerm);
    if (e != noErr) {
        std::printf("film: %s not opened by QuickTime (%d) - the software decoder\n", hp.c_str(), e);
        return false;
    }
    ::Movie m = nullptr;
    short resId = 0;
    e = NewMovieFromFile(&m, ref, &resId, nullptr, newMovieActive, nullptr);
    CloseMovieFile(ref);
    if (e != noErr || !m) {
        std::printf("film: %s not imported by QuickTime (%d) - the software decoder\n", hp.c_str(), e);
        return false;
    }
    ::Rect box;
    GetMovieBox(m, &box);
    OffsetRect(&box, static_cast<short>(-box.left), static_cast<short>(-box.top));
    filmW_ = box.right;
    filmH_ = box.bottom;
    ::Rect win;
    CGrafPtr port = GetWindowPort(static_cast<WindowRef>(win_));
    GetPortBounds(port, &win);
    const int ww = win.right - win.left, wh = win.bottom - win.top;
    if (agl_) {
        // under GL: QuickTime draws into a GWorld, `filmShow` sends it
        // AT 16 BITS when the probe found ARGB1555 the faster upload - the
        // reader's iBook in thousands of colours: 63 ms a 640x480 frame
        // against 113 for ARGB8888 (2026-10-09). A 16-bit GWorld is xRGB
        // 1-5-5-5, which BGRA 1-5-5-5-REV reads as it stands, and QuickTime
        // converts less to fill it.
        filmDepth_ = film1555_ ? 16 : 32;
        GWorldPtr gw = nullptr;
        if (NewGWorld(&gw, static_cast<short>(filmDepth_), &box, nullptr, nullptr, 0) != noErr || !gw) {
            std::printf("film: no %d-bit GWorld - the software decoder\n", filmDepth_);
            DisposeMovie(m);
            return false;
        }
        LockPixels(GetGWorldPixMap(gw));
        filmGw_ = gw;
        SetMovieGWorld(m, gw, nullptr);
    } else {
        // the plain window: QuickTime draws into it, fitted and centred
        SetPort(port);
        ForeColor(blackColor);
        PaintRect(&win);
        const double fit = std::min(static_cast<double>(ww) / filmW_, static_cast<double>(wh) / filmH_);
        const int ow = static_cast<int>(filmW_ * fit), oh = static_cast<int>(filmH_ * fit);
        ::Rect r;
        SetRect(&r, static_cast<short>((ww - ow) / 2), static_cast<short>((wh - oh) / 2),
                static_cast<short>((ww - ow) / 2 + ow), static_cast<short>((wh - oh) / 2 + oh));
        SetMovieGWorld(m, port, nullptr);
        SetMovieBox(m, &r);
    }
    static MovieDrawingCompleteUPP drawn = NewMovieDrawingCompleteUPP(filmDrawn);
    SetMovieDrawingCompleteProc(m, movieDrawingCallWhenChanged, drawn, reinterpret_cast<long>(this));
    // the world's sound channel stays as it is: QuickTime opens its own
    flushAudio();
    film_ = m;
    filmFrames_ = 0;
    filmDrawn_ = false;
    GoToBeginningOfMovie(m);
    SetMovieVolume(m, kFullVolume);
    StartMovie(m);
    std::printf("film: %s through QuickTime, %dx%d, %.1f s, %s\n", hp.c_str(), filmW_, filmH_,
                static_cast<double>(GetMovieDuration(m)) / GetMovieTimeScale(m),
                agl_ ? (filmDepth_ == 16 ? "sent to the card as ARGB1555"
                        : argbOk_ ? "sent to the card as ARGB8888" : "sent to the card as RGBA bytes")
                     : "drawn into the window");
    return true;
}

bool CarbonFrontend::filmStep() {
    if (!film_) return false;
    ::Movie m = static_cast<::Movie>(film_);
    MoviesTask(m, 0);
    if (filmDrawn_) {
        filmDrawn_ = false;
        ++filmFrames_;
        if (agl_) filmShow();
        else QDFlushPortBuffer(GetWindowPort(static_cast<WindowRef>(win_)), nullptr);
    }
    return !IsMovieDone(m) && GetMoviesError() == noErr;
}

void CarbonFrontend::filmClose() {
    if (!film_) return;
    ::Movie m = static_cast<::Movie>(film_);
    StopMovie(m);
    DisposeMovie(m);
    film_ = nullptr;
    if (filmGw_) {
        DisposeGWorld(static_cast<GWorldPtr>(filmGw_));
        filmGw_ = nullptr;
    }
    std::printf("film: %ld pictures drawn by QuickTime\n", filmFrames_);
}

void CarbonFrontend::filmShow() {
#if defined(OMK_GL1_AGL)
    AGLContext ctx = static_cast<AGLContext>(agl_);
    const AGLContext prev = aglGetCurrentContext();
    if (prev != ctx) aglSetCurrentContext(ctx);
    PixMapHandle pm = GetGWorldPixMap(static_cast<GWorldPtr>(filmGw_));
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetPixBaseAddr(pm));
    const int rowBytes = GetPixRowBytes(pm) & 0x3FFF;
    ::Rect b;
    GetPortBounds(GetWindowPort(static_cast<WindowRef>(win_)), &b);
    const int ww = b.right - b.left, wh = b.bottom - b.top;
    // fitted and centred, as the software path fits the film
    const double fit = std::min(static_cast<double>(ww) / filmW_, static_cast<double>(wh) / filmH_);
    const int ow = static_cast<int>(filmW_ * fit), oh = static_cast<int>(filmH_ * fit);
    glViewport(0, 0, ww, wh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport((ww - ow) / 2, (wh - oh) / 2, ow, oh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, ow, 0, oh, -1, 1);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDrawBuffer(GL_BACK);
    int tw = 1, th = 1;
    while (tw < filmW_) tw <<= 1;
    while (th < filmH_) th <<= 1;
    GLuint tex = static_cast<GLuint>(filmTex_);
    if (!tex || filmTexW_ != tw || filmTexH_ != th) {
        if (!tex) glGenTextures(1, &tex);
        filmTex_ = tex;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexImage2D(GL_TEXTURE_2D, 0, filmDepth_ == 16 ? GL_RGB5 : GL_RGB8, tw, th, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        filmTexW_ = tw;
        filmTexH_ = th;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    {
        OMK_ZONE("film: upload");
        if (filmDepth_ == 16) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rowBytes / 2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, filmW_, filmH_, GL_BGRA,
                            GL_UNSIGNED_SHORT_1_5_5_5_REV, base);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        } else if (argbOk_) {
            // a 32-bit GWorld is xRGB in memory, big-endian: one word
            // x<<24|r<<16|g<<8|b, which BGRA 8-8-8-8-REV reads as it stands
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rowBytes / 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, filmW_, filmH_, GL_BGRA,
                            GL_UNSIGNED_INT_8_8_8_8_REV, base);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        } else {
            const std::size_t n = static_cast<std::size_t>(filmW_) * filmH_;
            if (uiConv_.size() < n * 4) uiConv_.resize(n * 4);
            std::uint8_t* o = uiConv_.data();
            for (int y = 0; y < filmH_; ++y) {
                const std::uint8_t* s = base + static_cast<std::size_t>(y) * rowBytes;
                for (int x = 0; x < filmW_; ++x, s += 4, o += 4) {
                    o[0] = s[1]; o[1] = s[2]; o[2] = s[3]; o[3] = 255;
                }
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, filmW_, filmH_, GL_RGBA, GL_UNSIGNED_BYTE,
                            uiConv_.data());
        }
    }
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    drawFrameQuad(ow, oh, static_cast<float>(filmW_) / tw, static_cast<float>(filmH_) / th);
    glDisable(GL_TEXTURE_2D);
    {
        OMK_ZONE("film: swap");
        aglSwapBuffers(ctx);
    }
    if (prev != ctx) aglSetCurrentContext(prev);
#endif
}

void* gl1AglContext() { return g_carbon ? g_carbon->glContext() : nullptr; }
#if defined(OMK_GL1_AGL)
bool gl1OverlayArgb() { return g_overlayArgb; }
#endif

void gl1AglWindowSize(int& w, int& h) {
    w = h = 0;
    if (!g_carbon || !g_carbon->windowId()) return;
    ::Rect b;
    GetPortBounds(GetWindowPort(static_cast<WindowRef>(const_cast<void*>(g_carbon->windowId()))), &b);
    w = b.right - b.left;
    h = b.bottom - b.top;
}

std::unique_ptr<Frontend> makeHostFrontend() {
    auto f = std::make_unique<CarbonFrontend>();
    g_carbon = f.get();
    return f;
}

}  // namespace omk
