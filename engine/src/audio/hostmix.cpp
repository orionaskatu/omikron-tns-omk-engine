// SPDX-License-Identifier: GPL-3.0-or-later
// The host's mix - see hostmix.h.
#include "audio/hostmix.h"

#include <algorithm>

namespace omk {

int HostMixer::add(Shot s, std::shared_ptr<const void>* dropped) {
    if (shots_.size() >= 8) {
        if (dropped) {
            if (shots_.front().pcm) *dropped = std::move(shots_.front().pcm);
            else                    *dropped = std::move(shots_.front().snd);
        }
        shots_.erase(shots_.begin());
    }
    s.id = nextShot_++;
    shots_.push_back(std::move(s));
    return shots_.back().id;
}

int HostMixer::play(std::shared_ptr<const std::vector<float>> s, bool loop, float gain,
                    std::shared_ptr<const void>* dropped) {
    if (!s || s->empty()) return -1;
    return add({std::move(s), 0, 0, loop, gain}, dropped);
}

int HostMixer::play(std::shared_ptr<const DeviceSound> s, bool loop, float gain,
                    std::shared_ptr<const void>* dropped) {
    if (!s || !s->size) return -1;
    return add({nullptr, 0, 0, loop, gain, std::move(s)}, dropped);
}

std::shared_ptr<DeviceSound> makeDeviceSound(int channels, int rate, int deviceRate) {
    auto d = std::make_shared<DeviceSound>();
    d->channels = channels > 1 ? 2 : 1;
    if (rate > 0 && deviceRate % rate == 0) {
        const int k = deviceRate / rate;
        if (k == 1 || k == 2 || k == 4 || k == 8) {
            d->exact = true;
            d->shift = k == 1 ? 0u : k == 2 ? 1u : k == 4 ? 2u : 3u;
            return d;
        }
    }
    d->step = static_cast<double>(rate) / deviceRate;
    return d;
}

int HostMixer::open(std::shared_ptr<DeviceSound> s, std::size_t totalSamples, float gain,
                    std::shared_ptr<const void>* dropped) {
    if (!s || !totalSamples) return -1;
    Shot o{nullptr, 0, 0, false, gain, s};
    o.grow = std::move(s);
    o.total = totalSamples;
    return add(std::move(o), dropped);
}

int HostMixer::open(int channels, int rate, std::size_t frames, int deviceRate, float gain,
                    std::shared_ptr<const void>* dropped) {
    if (deviceRate <= 0 || rate <= 0 || !frames) return -1;
    auto d = makeDeviceSound(channels, rate, deviceRate);
    d->pcm.reserve(frames * d->channels);      // fed without a reallocation
    return open(d, d->samplesFor(frames), gain, dropped);
}

void HostMixer::feed(int handle, std::span<const std::int16_t> pcm) {
    if (handle < 0 || pcm.empty()) return;
    for (Shot& o : shots_)
        if (o.id == handle && o.grow) {
            DeviceSound& d = *o.grow;
            d.pcm.insert(d.pcm.end(), pcm.begin(), pcm.end());
            d.size = std::min(o.total, d.samplesFor(d.pcm.size() / d.channels));
            return;
        }
}

void HostMixer::queue(std::span<const float> s) {
    if (s.empty()) return;
    if (count_ + s.size() > ring_.size()) {
        // grow to the next power of two that holds everything queued, the
        // queued samples moved to the front in order
        std::size_t cap = ring_.empty() ? 4096 : ring_.size();
        while (cap < count_ + s.size()) cap *= 2;
        std::vector<float> next(cap);
        for (std::size_t i = 0; i < count_; ++i) next[i] = ring_[(head_ + i) & (ring_.size() - 1)];
        ring_.swap(next);
        head_ = 0;
    }
    const std::size_t mask = ring_.size() - 1;
    std::size_t tail = (head_ + count_) & mask;
    for (const float v : s) { ring_[tail] = v; tail = (tail + 1) & mask; }
    count_ += s.size();
}

void HostMixer::setGain(int handle, float gain) {
    if (handle < 0) return;
    for (Shot& o : shots_)
        if (o.id == handle) o.gain = gain;
}

void HostMixer::stop(int handle) {
    if (handle < 0) return;
    std::erase_if(shots_, [handle](const Shot& o) { return o.id == handle; });
}

long long HostMixer::played(int handle) const {
    if (handle < 0) return -1;
    for (const Shot& o : shots_)
        if (o.id == handle) return static_cast<long long>(o.pos);
    return -1;
}

void HostMixer::mix(float* dst, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        float v = 0.0f;
        if (count_) {
            v += ring_[head_] * musicGain_;
            head_ = (head_ + 1) & (ring_.size() - 1);
            --count_;
        }
        for (auto& one : shots_) {
            const std::size_t size = one.size();
            if (one.pos >= size) {
                if (!one.loop || !size) continue;
                one.pos = 0;                  // a looping shot wraps
            }
            v += (one.pcm ? (*one.pcm)[one.pos] : one.snd->at(one.pos)) * one.gain;
            ++one.pos;
        }
        dst[i] = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    }
    std::erase_if(shots_, [](const Shot& o) {
        return o.ended();                     // a loop ends only on stop
    });
}

}  // namespace omk
