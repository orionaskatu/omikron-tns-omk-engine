// SPDX-License-Identifier: GPL-3.0-or-later
// THE SMALL VECTOR AND ANGLE HELPERS, once (`todo/code-readability-audit.md`
// 5): the degree/radian factors were spelled ten ways in forty files and the
// `float[3]` helpers written out in four.
//
// The angle factors are the ENGINE'S: `dbl_4BCF00` and `0x4BC2E8` hold
// pi/180 as the double 0x1.1df46a2529d39p-6, which is `kDegToRad` exactly.
// Every float spelling of these numbers rounds to the same float, so the `F`
// forms replace any of them bit for bit; a double written short of them
// (`0.0174532925199433`, `3.14159265358979`) is NOT the same number and moves
// a result by an ulp.
#pragma once

#include <cmath>

namespace omk {

constexpr double kPi = 3.14159265358979323846;
constexpr float kPiF = static_cast<float>(kPi);
constexpr double kDegToRad = kPi / 180.0;      // dbl_4BCF00
constexpr float kDegToRadF = static_cast<float>(kDegToRad);
constexpr double kRadToDeg = 180.0 / kPi;
constexpr float kRadToDegF = static_cast<float>(kRadToDeg);

inline void sub3(const float a[3], const float b[3], float o[3]) {
    for (int k = 0; k < 3; ++k) o[k] = a[k] - b[k];
}
inline float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
inline float len3(const float a[3]) { return std::sqrt(dot3(a, a)); }
// to unit length; a zero vector is left as it is
inline void norm3(float v[3]) {
    const float l = len3(v);
    if (l > 0.0f) for (int k = 0; k < 3; ++k) v[k] /= l;
}

}  // namespace omk
