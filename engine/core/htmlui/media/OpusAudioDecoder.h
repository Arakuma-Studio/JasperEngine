// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef OPUSAUDIODECODER_H
#define OPUSAUDIODECODER_H

#include "Export.h"
#include "VideoDemuxer.h"

#include <cstdint>
#include <string>
#include <vector>

struct OpusMSDecoder;

namespace jasperengine {

    // The Opus track of a <video> (docs/html-ui/phase-6-video.md §3.7): libopus' multistream
    // decoder from the OpusHead of the codec private data (RFC 7845: channel mapping families 0,
    // 1 and 255), 48 kHz float, down-mixed to stereo when the stream has more than two channels.
    // The output gain of the header is applied; the pre-skip is the caller's (VideoStream drops
    // the samples before time 0).
    class JASPERENGINE_API OpusAudioDecoder {
    public:
        static constexpr int SAMPLE_RATE = 48000;
        static constexpr int MAX_FRAME_SAMPLES = 5760;   // 120 ms at 48 kHz

        OpusAudioDecoder() = default;
        ~OpusAudioDecoder();
        OpusAudioDecoder(const OpusAudioDecoder&) = delete;
        OpusAudioDecoder& operator=(const OpusAudioDecoder&) = delete;

        bool open(const AudioTrackInfo& track, std::string& error);
        // Decodes one packet into `out` (interleaved, outputChannels() per sample frame); returns
        // the sample frames, or -1 with error on damaged data
        int decode(const uint8_t* data, size_t size, std::vector<float>& out, std::string& error);
        void reset();   // before a seek

        int getStreamChannels() const;   // channels of the stream
        int getOutputChannels() const;   // 1 or 2
        int getPreSkip() const;          // samples at 48 kHz (OpusHead)

    private:
        void close();

        OpusMSDecoder* decoder = nullptr;
        int streamChannels = 0;
        int outputChannels = 0;
        int preSkip = 0;
        int mappingFamily = 0;
        std::vector<float> scratch;
    };

}

#endif //OPUSAUDIODECODER_H
