// SPDX-License-Identifier: GPL-3.0-or-later
#include "o3de/weather.h"

#include <cmath>
#include <climits>

namespace omk {

void Weather::tick(unsigned bits, long nowMs, bool covered, bool noPlayer,
                   const std::function<int()>& rnd, const std::function<void(int)>& post) {
    const auto r = [&]() { return rnd ? rnd() : 0; };
    // `sub_481200`: the roof ray's three arms
    if (covered) {
        if (state_ == 2 || state_ == 1) { start_ = nowMs; length_ = 200; state_ = 4; }
        else if (state_ == 0) start_ = nowMs;
    }
    switch (state_) {
    case 0:
        count_ = 0;
        if (nowMs > length_ + start_) {
            if (post) post(23);
            start_ = nowMs;
            length_ = 20000;
            state_ = 1;
        }
        break;
    case 1:
        count_ = static_cast<int>(static_cast<long long>(
            static_cast<double>(200 * (nowMs - start_)) / static_cast<double>(length_)));
        if (nowMs > length_ + start_) {
            start_ = nowMs;
            state_ = 2;
            length_ = r() % 60000 + 150000;
        }
        break;
    case 2:
        count_ = 200;
        if ((bits & 0xAu) && nowMs > length_ + start_) {
            if (post) post(24);
            start_ = nowMs;
            length_ = 10000;
            state_ = 4;
        }
        break;
    case 4: {
        int box = 1000;                              // dword_4D0780
        if (bits & 0xAu) {
            // `(1 - t/len) * 200` through `_ftol`; at the boot's length 0 the
            // division is infinite and `_ftol` gives 0x80000000 - a count
            // the draw's `> 0` test refuses, which is what this keeps
            count_ = length_ == 0 ? INT_MIN : static_cast<int>(static_cast<long long>(
                (1.0 - static_cast<double>(nowMs - start_) / static_cast<double>(length_)) * 200.0));
            if (nowMs > length_ + start_) {
                start_ = nowMs;
                state_ = 0;
                length_ = r() % 360000 + 480000;
            }
            box = (bits & 8u) ? 300 : 1000;
        } else if (bits & 4u) {
            box = 400;
            state_ = 2;
        }
        // every frame of state 4: the 200 drops scattered afresh, x then y
        // then z of each from `rand() % box - box / 2`
        const int half = box / 2;
        for (int i = 0; i < kWeatherDrops; ++i) {
            x_[i] = static_cast<float>(r() % box - half);
            y_[i] = static_cast<float>(r() % box - half);
            z_[i] = static_cast<float>(r() % box - half);
        }
        break;
    }
    default:
        break;
    }
    if (noPlayer) { count_ = 0; state_ = 0; }
}

std::vector<WeatherDrop> Weather::step(unsigned bits, const float eye[3], const float at[3]) {
    std::vector<WeatherDrop> out;
    if (!count_) return out;                         // `else if (dword_4CFE1C)`
    const float d[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len <= 0.0f) return out;
    if (bits & 2u) {
        // the RAIN: the first `count` drops, box 1000, the wind 0.5 / 0.866 /
        // 0 times 64 a frame, the streak's tail 12 frames of wind on
        const float box = 1000.0f, half = 500.0f;
        const float wind[3] = {0.5f * 64.0f, 0.866f * 64.0f, 0.0f};
        const float tail[3] = {0.5f * 12.0f, 0.866f * 12.0f, 0.0f};
        for (int i = 0; i < count_ && i < kWeatherDrops; ++i) {
            WeatherDrop w;
            const float px = x_[i] - d[0] * box * -0.33333334f / len + eye[0];
            const float py = y_[i] - d[1] * box * -0.33333334f / len + eye[1];
            const float pz = z_[i] - d[2] * box * -0.33333334f / len + eye[2];
            x_[i] += wind[0]; if (x_[i] > half) x_[i] = -box * 0.5f;
            y_[i] += wind[1]; if (y_[i] > half) y_[i] = -box * 0.5f;
            z_[i] += wind[2]; if (z_[i] > half) z_[i] = -box * 0.5f;
            w.p[0] = px; w.p[1] = py; w.p[2] = pz;
            w.q[0] = px + tail[0]; w.q[1] = py + tail[1]; w.q[2] = pz + tail[2];
            out.push_back(w);
        }
    } else if (bits & 4u) {
        // the SNOW (`sub_481D90`'s bit-4 arm): every one of the 200, box 400,
        // speed 20 - x drifts back by 0.866 of it, y falls 0.3 of it, z only
        // wraps - and the flake's size, 8 along x, is the second point
        const float box = 400.0f, half = 200.0f, speed = 20.0f;
        for (int i = 0; i < kWeatherDrops; ++i) {
            WeatherDrop w;
            const float px = x_[i] - d[0] * box * -0.33333334f / len + eye[0];
            const float py = y_[i] - d[1] * box * -0.33333334f / len + eye[1];
            const float pz = z_[i] - d[2] * box * -0.33333334f / len + eye[2];
            x_[i] -= speed * 0.866f;          if (x_[i] < -half) x_[i] = box * 0.5f;
            y_[i] -= speed * -0.30000001f;    if (y_[i] > half)  y_[i] = -box * 0.5f;
            if (z_[i] > half) z_[i] = -box * 0.5f;
            w.p[0] = px; w.p[1] = py; w.p[2] = pz;
            w.q[0] = px + 8.0f; w.q[1] = py; w.q[2] = pz;
            out.push_back(w);
        }
    }
    return out;
}

}  // namespace omk
