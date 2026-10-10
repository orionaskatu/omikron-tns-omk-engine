// SPDX-License-Identifier: GPL-3.0-or-later
// THE MORPH STREAMER - a spoken line read and decoded AS IT PLAYS, which is
// how the original plays one.
//
// `Morph_Start` (0x0042C870) does not read a line: `Morph_Open` (0x0042C300)
// opens the file (`mmioOpenA`), reads its header, primes the sound buffer with
// a first block, and returns; then a 15 ms multimedia timer
// (`timeSetEvent(15, 0, fptc, ...)`) reads and decodes the next blocks into
// the free part of the buffer for as long as the line plays (`fptc` ->
// `sub_42D960`). So no frame ever pays for a whole line.
//
// The port read a line's whole `.3DM` - 3 to 4 MB, 82% of it face vertices -
// and decoded its whole voice on the frame the line started; on an iBook G3
// that frame took 0.2 to 1.1 s (`todo/handoff-classic-mac.md` 3f). Here the
// file stays open and `fillTo` reads the next records with one read and
// decodes their audio blocks onto the voice, a frame's worth at a time
// (`DialogPlayer::tick` keeps it a couple of seconds ahead of the line's
// clock). What a line IS does not change: the same bytes land at the same
// offsets of `bytes()` as a whole read puts them, and `AdpcmStream` decodes
// the blocks in pieces to the samples `adpcmDecode` gives the whole
// (`verify.py: engine: line stream`).
//
// `bytes()` is the WHOLE FILE's size from the start - the records not read
// yet are zeros - so a reader of a frame (`faceFrame`, `nodeTracks`) works on
// the same offsets it always did, and must ask only for frames below
// `filled()`. No thread: the classic build has none, and the read is small
// enough not to need one (a second of a line is ~115 KB).
#pragma once

#include "formats/adpcm.h"
#include "formats/morph.h"
#include "platform/datafs.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omk {

class MorphStream {
public:
    // Opens `path` (a real path, `DataFs::findPath`'s), reads the header and
    // the preamble and then the first `frames` records. -> false when the file
    // will not open or is not a `.3DM` (the stream is then closed).
    bool open(const std::string& path, std::size_t frames);
    void close();
    // a line is open (its file may already be closed: read to its end)
    bool valid() const { return L_.valid; }

    // Read the records up to frame `frames` (exclusive) and decode their
    // audio. -> the frames read so far. Past the last frame, the file closes.
    std::size_t fillTo(std::size_t frames);
    std::size_t filled() const { return filled_; }
    bool complete() const { return L_.valid && filled_ >= L_.frames; }
    const MorphLayout& layout() const { return L_; }

    // The whole file's size, read up to the end of frame `filled() - 1`.
    std::shared_ptr<const std::vector<std::byte>> bytes() const { return bytes_; }
    // The voice so far: 16-bit at 22050, interleaved when stereo.
    const std::vector<std::int16_t>& pcm() const { return pcm_; }
    int channels() const { return L_.channels > 1 ? 2 : 1; }
    // The whole voice's length in sample frames - known from the layout
    // before any of it is decoded: two samples a byte, interleaved in stereo.
    std::size_t totalSampleFrames() const;

    // what the stream has cost, in ms: the reads and the decode
    double readMs() const { return readMs_; }
    double decodeMs() const { return decodeMs_; }

private:
    HostFile file_;
    MorphLayout L_;
    AdpcmTables tables_ = AdpcmTables::builtin();
    std::unique_ptr<AdpcmStream> dec_;
    std::shared_ptr<std::vector<std::byte>> bytes_;
    std::vector<std::int16_t> pcm_;
    std::size_t filled_ = 0;
    std::size_t audioFrames_ = 0;   // the records that carry an audio block
    double readMs_ = 0.0, decodeMs_ = 0.0;
};

}  // namespace omk
