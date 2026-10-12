// SPDX-License-Identifier: GPL-3.0-or-later
//
// formats/bytes.h - THE ONE READER OF A LITTLE-ENDIAN FIELD AT AN OFFSET.
//
// Every file format, script chunk and save record the game ships is
// little-endian, and every read of one goes through here (or through
// `loadLE` directly, which this is built on - PORTING A9, the byte order
// done once), and there is one rule for a read past the end
// (`todo/code-readability-audit.md`):
//
//     a read that would run past the end of the buffer returns 0.
//
// It never throws and never reads out of bounds, so a walker over a damaged
// file sees zeros and stops on its own guards instead of faulting. The bound
// is written `o <= size && n <= size - o`, which cannot wrap the way
// `o + n > size` can for an offset near the top of `size_t`.
//
//     using le::u32;  ...  const std::uint32_t count = u32(data, 44);
#pragma once

#include "formats/le.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace omk::le {

namespace detail {
template <typename T, typename B>
T at(std::span<const B> d, std::size_t o) {
    static_assert(sizeof(B) == 1, "a byte buffer");
    if (o > d.size() || sizeof(T) > d.size() - o) return T{};
    return loadLE<T>(reinterpret_cast<const std::byte*>(d.data() + o));
}
}  // namespace detail

// over `std::byte` - the readers' own buffers (DataFs, IamArchive)...
inline std::uint8_t  u8 (std::span<const std::byte> d, std::size_t o) { return detail::at<std::uint8_t>(d, o); }
inline std::uint16_t u16(std::span<const std::byte> d, std::size_t o) { return detail::at<std::uint16_t>(d, o); }
inline std::uint32_t u32(std::span<const std::byte> d, std::size_t o) { return detail::at<std::uint32_t>(d, o); }
inline std::int16_t  i16(std::span<const std::byte> d, std::size_t o) { return detail::at<std::int16_t>(d, o); }
inline std::int32_t  i32(std::span<const std::byte> d, std::size_t o) { return detail::at<std::int32_t>(d, o); }
inline float         f32(std::span<const std::byte> d, std::size_t o) { return detail::at<float>(d, o); }

// ...and over `std::uint8_t` - the VM's instruction bytes and the scene
// objects' parameter blocks, which are `std::vector<std::uint8_t>`
inline std::uint8_t  u8 (std::span<const std::uint8_t> d, std::size_t o) { return detail::at<std::uint8_t>(d, o); }
inline std::uint16_t u16(std::span<const std::uint8_t> d, std::size_t o) { return detail::at<std::uint16_t>(d, o); }
inline std::uint32_t u32(std::span<const std::uint8_t> d, std::size_t o) { return detail::at<std::uint32_t>(d, o); }
inline std::int16_t  i16(std::span<const std::uint8_t> d, std::size_t o) { return detail::at<std::int16_t>(d, o); }
inline std::int32_t  i32(std::span<const std::uint8_t> d, std::size_t o) { return detail::at<std::int32_t>(d, o); }
inline float         f32(std::span<const std::uint8_t> d, std::size_t o) { return detail::at<float>(d, o); }

}  // namespace omk::le
