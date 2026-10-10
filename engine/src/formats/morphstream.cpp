// SPDX-License-Identifier: GPL-3.0-or-later
// The morph streamer - see morphstream.h.
#include "formats/morphstream.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace omk {
namespace {
double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

bool MorphStream::open(const std::string& path, std::size_t frames) {
    close();
    const auto t0 = std::chrono::steady_clock::now();
    if (!file_.open(path)) return false;
    const auto size = static_cast<std::size_t>(file_.size());
    // the header, then the preamble its node count sizes
    std::vector<std::byte> head(16);
    if (file_.readAt(0, head.data(), head.size()) != head.size()) { close(); return false; }
    const std::uint32_t nodes = static_cast<std::uint32_t>(head[12]) |
                                static_cast<std::uint32_t>(head[13]) << 8 |
                                static_cast<std::uint32_t>(head[14]) << 16 |
                                static_cast<std::uint32_t>(head[15]) << 24;
    if (16 + 4 * std::uint64_t{nodes} > size) { close(); return false; }
    head.resize(16 + 4 * static_cast<std::size_t>(nodes));
    if (file_.readAt(16, head.data() + 16, head.size() - 16) != head.size() - 16) {
        close(); return false;
    }
    L_ = morphLayout(head, size);
    if (!L_.valid) { close(); return false; }
    bytes_ = std::make_shared<std::vector<std::byte>>(size);
    std::copy(head.begin(), head.end(), bytes_->begin());
    audioFrames_ = L_.audio == 0 ? 0 : (L_.lastFrameHasAudio ? L_.frames : L_.frames - 1);
    pcm_.reserve(totalSampleFrames() * static_cast<std::size_t>(channels()));
    dec_ = std::make_unique<AdpcmStream>(tables_, L_.channels > 1);
    readMs_ = msSince(t0);
    fillTo(frames);
    return true;
}

void MorphStream::close() {
    file_.close();
    L_ = MorphLayout{};
    dec_.reset();
    bytes_.reset();
    pcm_.clear();
    pcm_.shrink_to_fit();
    filled_ = audioFrames_ = 0;
    readMs_ = decodeMs_ = 0.0;
}

std::size_t MorphStream::totalSampleFrames() const {
    if (!L_.valid) return 0;
    // `adpcmDecode` gives two samples a byte: two FRAMES of a mono voice, one
    // of a stereo one
    const std::size_t bytes = audioFrames_ * L_.audio;
    return channels() > 1 ? bytes : 2 * bytes;
}

std::size_t MorphStream::fillTo(std::size_t frames) {
    if (!L_.valid || !bytes_ || !file_.isOpen()) return filled_;
    frames = std::min(frames, L_.frames);
    if (frames <= filled_) return filled_;
    // ONE read for every record asked for, at the offset a whole read puts
    // them - the last record may be short (no audio block)
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t from = L_.preamble + filled_ * L_.record;
    const std::size_t to = std::min(L_.preamble + frames * L_.record, L_.size);
    const std::size_t got = file_.readAt(from, bytes_->data() + from, to - from);
    readMs_ += msSince(t0);
    if (got < to - from) {
        // a short read (the medium pulled?): what arrived stands, the rest
        // stays zeros, and the stream stops where the file did
        std::printf("morph stream: SHORT READ - %lu of %lu bytes at %lu\n",
                    static_cast<unsigned long>(got), static_cast<unsigned long>(to - from),
                    static_cast<unsigned long>(from));
        frames = filled_ + got / L_.record;
    }
    // the audio blocks, in order: `morphAudio`'s concatenation, a piece at a
    // time - the decoder's state carries from one block to the next
    const auto t1 = std::chrono::steady_clock::now();
    const std::size_t lastAudio = std::min(frames, audioFrames_);
    for (std::size_t f = filled_; f < lastAudio; ++f) {
        const std::size_t end = L_.preamble + f * L_.record + L_.record;
        dec_->decode(std::span<const std::byte>(bytes_->data() + (end - L_.audio), L_.audio), pcm_);
    }
    decodeMs_ += msSince(t1);
    filled_ = frames;
    if (complete() || got < to - from) file_.close();
    return filled_;
}

}  // namespace omk
