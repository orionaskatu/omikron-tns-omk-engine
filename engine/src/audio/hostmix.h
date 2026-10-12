// SPDX-License-Identifier: GPL-3.0-or-later
// THE HOST'S MIX - what a frontend sums into its audio device: the STREAM
// (the films' soundtrack, the game's music) at the music gain, and up to
// eight ONE-SHOTS (interface blips, scene sounds, a line's voice) each at its
// own gain, looping or not. Shared by the SDL and the Carbon frontends so
// both sum the same way.
//
// THE STREAM IS A RING, as the original's is: `Morph_Open` plays the music
// through a DirectSound buffer of a FIXED eight seconds (bytes/s x 8.0,
// `dbl_4BC298`), refilled every 15 ms. Keeping every played sample until
// 2^20 floats have gone by and only then compacting holds 5.4 MB for a
// one-second queue, which the profiler shows doubling
// (`todo/debug-tools.md`, the first full capture). So nothing played is
// kept: the ring holds what is queued, grows only to the most ever queued
// at once, and `clear()` gives it back (the films queue more than the world).
//
// Not the engine's mixer: there is none to port - `Sound_Init` sets a
// DirectSound primary buffer and DirectSound sums into it (`audio/mixer.h`).
// This stands where DirectSound stood, and its attenuation and pan law are
// this port's (PORTING, the audio row). It takes NO lock: a frontend whose
// device pulls from another thread (SDL) holds its own around every call; one
// that refills from the main thread (Carbon) needs none.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace omk {

// A SOUND KEPT AS ITS FILE HOLDS IT - 16-bit at its own rate and channel
// count - and read at the device's rate as it plays (todo/ram-vs-original.md
// tier B). `sub_46C740` creates each DirectSound buffer in the file's own
// format and DirectSound converts while mixing; this held a float stereo copy
// at 22050 instead, four times a mono file's bytes. `at(i)` is EXACTLY the
// float `wavToDevice` puts at index i (the same nearest frame, the same
// /32768), so the mix is bit-identical; `wavToDeviceSound` builds one.
struct DeviceSound {
    std::vector<std::int16_t> pcm;   // interleaved, host byte order
    std::size_t channels = 1;
    unsigned shift = 0;              // device rate = source rate << shift, when exact
    bool exact = false;              // ...that, else `step`
    double step = 0.0;               // source frames per device frame
    std::size_t size = 0;            // device samples, interleaved stereo
    // the device samples (stereo, interleaved) that `frames` source frames
    // make: `wavToDevice`'s count, cut where the nearest source frame runs off
    // the end - so a sound counted in pieces ends where it counts whole
    std::size_t samplesFor(std::size_t frames) const {
        if (exact) return (frames << shift) * 2;
        std::size_t out = static_cast<std::size_t>(frames / step);
        while (out > 0 && static_cast<std::size_t>((out - 1) * step) >= frames) --out;
        return out * 2;
    }
    float at(std::size_t i) const {
        const std::size_t frame = i >> 1;
        const std::size_t src = exact ? frame >> shift
                                      : static_cast<std::size_t>(frame * step);
        return pcm[src * channels + (channels > 1 ? (i & 1) : 0)] / 32768.0f;
    }
};

// An empty `DeviceSound` for samples at `rate` with `channels`, read at
// `deviceRate`: exact for a power-of-two ratio up to 8, `step` otherwise (the
// rule `wavToDeviceSound` applies to a file).
std::shared_ptr<DeviceSound> makeDeviceSound(int channels, int rate, int deviceRate);

class HostMixer {
public:
    // Device-rate interleaved float samples onto the end of the stream.
    void queue(std::span<const float> s);
    // A one-shot over the stream; the oldest is dropped past eight (a cap: a
    // held key would otherwise stack voices without end). -> its handle for
    // `stop`. The samples are SHARED (`Sound_Play3D` plays a duplicate of the
    // bank's buffer: same memory, a second voice). What the cap pushes out is
    // moved to `*dropped` when given, so a caller under a lock frees it after.
    int play(std::shared_ptr<const std::vector<float>> s, bool loop, float gain,
             std::shared_ptr<const void>* dropped = nullptr);
    // ...and one kept in its file's own form, read at the device rate
    int play(std::shared_ptr<const DeviceSound> s, bool loop, float gain,
             std::shared_ptr<const void>* dropped = nullptr);
    // A SOUND THAT GROWS WHILE IT PLAYS - a line's voice, decoded as the line
    // is read (`formats/morphstream.h`, the original's `fptc` filling its
    // DirectSound buffer). `s` starts with what is decoded so far and
    // `totalSamples` (device samples) is its length once whole: it plays what
    // it has, waits silent where it runs short (an underrun, not an end), and
    // ends only once all of it has played. `feed` appends samples in its own
    // form and is the only thing that touches `s` after this.
    int open(std::shared_ptr<DeviceSound> s, std::size_t totalSamples, float gain,
             std::shared_ptr<const void>* dropped = nullptr);
    void feed(int handle, std::span<const std::int16_t> pcm);
    // ...made here, for `Frontend::openSound`: `frames` sample frames at
    // `rate` with `channels` once whole, read at `deviceRate`; nothing in it yet
    int open(int channels, int rate, std::size_t frames, int deviceRate, float gain,
             std::shared_ptr<const void>* dropped = nullptr);
    void stop(int handle);
    // A playing shot's gain, changed in place - what `Sound_SetVoice3D`
    // (`sub_46CFC0`) does to a moving source each frame through DirectSound's
    // 3D buffer; this mixer has no position, so the caller hands it the gain.
    void setGain(int handle, float gain);
    // How far a one-shot has PLAYED: the device samples (all channels) mixed
    // from it so far, or -1 once it is gone (ended, stopped or never). Mixed
    // is a device buffer ahead of heard - the nearest thing to the play
    // cursor `Game_Frame`'s line clock reads (T2).
    long long played(int handle) const;
    void flush() { head_ = 0; count_ = 0; }                   // the stream only
    // everything, and the ring's memory back: the next device's rate and
    // queue need not be the last one's (the films at 44100, the world at 22050)
    void clear() { flush(); std::vector<float>().swap(ring_); shots_.clear(); }
    void setMusicGain(float g) { musicGain_ = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g); }
    // Stream samples still to play (all channels counted).
    std::size_t queued() const { return count_; }
    // The ring's size in floats - what the stream holds in memory (a check).
    std::size_t ringCapacity() const { return ring_.size(); }
    // `n` output samples, interleaved: the stream at the music gain plus every
    // shot at its gain, clamped to [-1, 1]; a finished shot is dropped, a
    // looping one wraps until `stop`.
    void mix(float* dst, std::size_t n);

private:
    struct Shot { std::shared_ptr<const std::vector<float>> pcm; std::size_t pos; int id;
                  bool loop = false; float gain = 1.0f;
                  std::shared_ptr<const DeviceSound> snd = nullptr;   // when `pcm` is null
                  std::shared_ptr<DeviceSound> grow = nullptr;        // `snd`, while it grows
                  std::size_t total = 0;                              // ...its device samples when whole
                  std::size_t size() const { return pcm ? pcm->size() : snd->size; }
                  bool ended() const { return !loop && pos >= size() && (!grow || size() >= total); } };
    int add(Shot s, std::shared_ptr<const void>* dropped);
    int nextShot_ = 1;
    float musicGain_ = 1.0f;     // Music_SetVolume, applied to the stream
    std::vector<float> ring_;    // a power of two in size, or empty
    std::size_t head_ = 0;       // the next sample to play
    std::size_t count_ = 0;      // samples queued, from head_ on, wrapping
    std::vector<Shot> shots_;
};

}  // namespace omk
