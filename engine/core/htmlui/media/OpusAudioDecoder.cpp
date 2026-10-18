// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "OpusAudioDecoder.h"

#include <opus.h>
#include <opus_multistream.h>

#include <algorithm>
#include <cstring>

using namespace jasperengine;

namespace {

    uint16_t le16(const uint8_t* p){
        return (uint16_t)(p[0] | (p[1] << 8));
    }

    // Vorbis channel order (RFC 7845 §5.1.1.2) to stereo: front left/right at 1, centre and the
    // surrounds at -3 dB, LFE left out; normalized so full scale everywhere does not clip
    void downmix(const float* in, int channels, int frames, float* out){
        static const float c = 0.70710678f;
        // per channel count 3..8: the left and right weights of each input channel
        struct Weights { float l[8]; float r[8]; };
        static const Weights table[9] = {
            {}, {}, {},
            {{1, c, 0}, {0, c, 1}},                                    // 3: L C R
            {{1, 0, c, 0}, {0, 1, 0, c}},                              // 4: L R RL RR
            {{1, c, 0, c, 0}, {0, c, 1, 0, c}},                        // 5: L C R RL RR
            {{1, c, 0, c, 0, 0}, {0, c, 1, 0, c, 0}},                  // 6: L C R RL RR LFE
            {{1, c, 0, c, 0, c, 0}, {0, c, 1, 0, c, c, 0}},            // 7: L C R SL SR RC LFE
            {{1, c, 0, c, 0, c, 0, 0}, {0, c, 1, 0, c, 0, c, 0}},      // 8: L C R SL SR RL RR LFE
        };
        const Weights& w = table[std::clamp(channels, 3, 8)];
        float sumL = 0.0f, sumR = 0.0f;
        for (int ch = 0; ch < channels && ch < 8; ch++){
            sumL += w.l[ch];
            sumR += w.r[ch];
        }
        const float scaleL = sumL > 0.0f ? 1.0f / sumL : 1.0f;
        const float scaleR = sumR > 0.0f ? 1.0f / sumR : 1.0f;
        for (int i = 0; i < frames; i++){
            const float* frame = in + (size_t)i * (size_t)channels;
            float l = 0.0f, r = 0.0f;
            for (int ch = 0; ch < channels && ch < 8; ch++){
                l += frame[ch] * w.l[ch];
                r += frame[ch] * w.r[ch];
            }
            out[2 * i] = l * scaleL;
            out[2 * i + 1] = r * scaleR;
        }
    }

}

OpusAudioDecoder::~OpusAudioDecoder(){
    close();
}

void OpusAudioDecoder::close(){
    if (decoder)
        opus_multistream_decoder_destroy(decoder);
    decoder = nullptr;
}

bool OpusAudioDecoder::open(const AudioTrackInfo& track, std::string& error){
    close();
    const std::vector<uint8_t>& head = track.codecPrivate;
    if (head.size() < 19 || std::memcmp(head.data(), "OpusHead", 8) != 0){
        error = "the Opus track has no OpusHead header";
        return false;
    }
    if ((head[8] >> 4) != 0){
        error = "unsupported OpusHead version " + std::to_string(head[8]);
        return false;
    }
    streamChannels = head[9];
    preSkip = le16(&head[10]);
    const int16_t gain = (int16_t)le16(&head[16]);
    mappingFamily = head[18];
    if (streamChannels < 1){
        error = "the Opus track has no channels";
        return false;
    }

    int streams = 1, coupled = streamChannels == 2 ? 1 : 0;
    unsigned char mapping[255] = {0, 1};
    if (mappingFamily == 0){
        if (streamChannels > 2){
            error = "an Opus track of mapping family 0 has " + std::to_string(streamChannels) + " channels";
            return false;
        }
    }else{
        if (head.size() < 21 + (size_t)streamChannels){
            error = "the OpusHead channel mapping table is short";
            return false;
        }
        streams = head[19];
        coupled = head[20];
        std::memcpy(mapping, &head[21], (size_t)streamChannels);
    }

    int result = OPUS_OK;
    decoder = opus_multistream_decoder_create(SAMPLE_RATE, streamChannels, streams, coupled, mapping, &result);
    if (!decoder || result != OPUS_OK){
        decoder = nullptr;
        error = std::string("libopus could not start: ") + opus_strerror(result);
        return false;
    }
    if (gain != 0)
        opus_multistream_decoder_ctl(decoder, OPUS_SET_GAIN(gain));
    outputChannels = streamChannels == 1 ? 1 : 2;
    return true;
}

int OpusAudioDecoder::decode(const uint8_t* data, size_t size, std::vector<float>& out, std::string& error){
    if (!decoder){
        error = "the Opus decoder is not open";
        return -1;
    }
    scratch.resize((size_t)MAX_FRAME_SAMPLES * (size_t)streamChannels);
    const int frames = opus_multistream_decode_float(decoder, data, (opus_int32)size, scratch.data(), MAX_FRAME_SAMPLES, 0);
    if (frames < 0){
        error = std::string("the Opus data is damaged: ") + opus_strerror(frames);
        return -1;
    }
    out.resize((size_t)frames * (size_t)outputChannels);
    if (streamChannels <= 2){
        std::memcpy(out.data(), scratch.data(), out.size() * sizeof(float));
    }else{
        downmix(scratch.data(), streamChannels, frames, out.data());
    }
    return frames;
}

void OpusAudioDecoder::reset(){
    if (decoder)
        opus_multistream_decoder_ctl(decoder, OPUS_RESET_STATE);
}

int OpusAudioDecoder::getStreamChannels() const{
    return streamChannels;
}

int OpusAudioDecoder::getOutputChannels() const{
    return outputChannels;
}

int OpusAudioDecoder::getPreSkip() const{
    return preSkip;
}
