// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// SoLoud's loaders, streams and mixer as the engine uses them (the local patches listed in
// engine/libs/soloud/CMakeLists.txt), each on a SoLoud of its own on the null driver: mono, 44.1
// kHz, no clipping, mixed here in calls of the sizes a game's frames give. A voice at pan 0 reaches
// a mono output at cos(pi / 4), so an output sample is the clip's sample x that gain.
//
// Fixtures (engine/tests/core/fixtures/audio): spaceEngineLow_003.ogg is Kenney's "Sci-Fi Sounds"
// 1.0 (CC0, License-kenney.txt), 5 s of 44.1 kHz mono whose last page counts 220,500 frames and
// which stb_vorbis decodes past; sweep.flac and sweep.mp3 are FFmpeg's encodes (flac s16, libmp3lame
// 128k) of 1 s of a 44.1 kHz mono sweep, aevalsrc=0.5*sin(2*PI*(200*t+1900*t*t)).

#include <gtest/gtest.h>

#include "audio/SoloudEngineFile.h"

#include "soloud.h"
#include "soloud_bus.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace {

    namespace fs = std::filesystem;

    constexpr unsigned int kRate = 44100;

    std::string fixture(const char* name) {
        return std::string(JASPER_TEST_REPO_DIR) + "/engine/tests/core/fixtures/audio/" + name;
    }

    std::vector<unsigned char> readBytes(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // a 16-bit PCM WAV of `channels` x `frames`, sample(frame, channel) in -1..1
    std::vector<unsigned char> wavBytes(unsigned int channels, unsigned int frames, const std::function<float(unsigned int, unsigned int)>& sample) {
        std::vector<unsigned char> out;
        auto u32 = [&out](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((unsigned char)(v >> (8 * i))); };
        auto u16 = [&out](uint16_t v) { out.push_back((unsigned char)v); out.push_back((unsigned char)(v >> 8)); };
        const uint32_t data = frames * channels * 2;
        out.insert(out.end(), {'R', 'I', 'F', 'F'});
        u32(36 + data);
        out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        u32(16);
        u16(1);
        u16((uint16_t)channels);
        u32(kRate);
        u32(kRate * channels * 2);
        u16((uint16_t)(channels * 2));
        u16(16);
        out.insert(out.end(), {'d', 'a', 't', 'a'});
        u32(data);
        for (unsigned int f = 0; f < frames; f++) {
            for (unsigned int c = 0; c < channels; c++) {
                u16((uint16_t)(int16_t)std::lround(std::clamp(sample(f, c), -1.0f, 1.0f) * 32767.0f));
            }
        }
        return out;
    }

    // the sweep of the FLAC and MP3 fixtures, as a WAV
    float sweep(unsigned int frame) {
        const double t = (double)frame / kRate;
        return (float)(0.5 * std::sin(2.0 * M_PI * (200.0 * t + 1900.0 * t * t)));
    }

    // one name per test: a death test's child runs the test again up to its statement
    fs::path writeTemp(const std::string& name, const std::vector<unsigned char>& bytes) {
        const fs::path path = fs::temp_directory_path() / ("jasper-soloud-test-" + name);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        return path;
    }

    // a SoLoud of its own on the null driver: mono, no clipping, mixed by the test. A source keeps
    // the SoLoud it last played on and stops itself there when it goes: the ones played here forget
    // it before it goes.
    struct NullSoloud {
        SoLoud::Soloud soloud;
        std::vector<SoLoud::AudioSource*> played;
        NullSoloud() {
            EXPECT_EQ(soloud.init(0, SoLoud::Soloud::NULLDRIVER, kRate, 512, 1), SoLoud::SO_NO_ERROR);
        }
        ~NullSoloud() {
            for (SoLoud::AudioSource* source : played) {
                source->stop();
                source->mSoloud = nullptr;
            }
            soloud.deinit();
        }
        SoLoud::handle play(SoLoud::AudioSource& source) {
            track(source);
            return soloud.play(source);
        }
        // a source played through a bus of this SoLoud
        void track(SoLoud::AudioSource& source) { played.push_back(&source); }

        // `frames` of the mix, in calls of `call` frames
        std::vector<float> mix(unsigned int frames, unsigned int call = 735) {
            std::vector<float> out(frames, 0.0f);
            for (unsigned int at = 0; at < frames; at += call) {
                soloud.mix(out.data() + at, std::min(call, frames - at));
            }
            return out;
        }
    };

    // the first frame louder than `threshold`, -1 when none
    int onset(const std::vector<float>& out, float threshold = 0.05f) {
        for (size_t i = 0; i < out.size(); i++) {
            if (std::fabs(out[i]) > threshold) return (int)i;
        }
        return -1;
    }

    // the largest difference of out[from + k] and ref[refFrom + k] over `count` frames
    float worstDifference(const std::vector<float>& out, size_t from, const std::vector<float>& ref, size_t refFrom, size_t count) {
        float worst = 0.0f;
        for (size_t k = 0; k < count; k++) {
            worst = std::max(worst, std::fabs(out[from + k] - ref[refFrom + k]));
        }
        return worst;
    }

    // A looping stream with its loop point wraps to it: after the clip's end the stream plays what
    // a whole decode of the clip plays from the loop point on (both through the same mixer, so any
    // latency of it is the same in both)
    void expectLoopWrapsToItsPoint(SoLoud::WavStream& stream, SoLoud::Wav& whole, double loopPoint, float tolerance) {
        NullSoloud reference;
        reference.play(whole);
        const unsigned int length = whole.mSampleCount;
        const std::vector<float> ref = reference.mix(length + 4096);

        NullSoloud looping;
        const SoLoud::handle voice = looping.play(stream);
        looping.soloud.setLooping(voice, true);
        looping.soloud.setLoopPoint(voice, loopPoint);
        const std::vector<float> out = looping.mix(length + 8192);

        ASSERT_EQ(stream.mSampleCount, length) << "the stream's length is the whole decode's";
        const size_t point = (size_t)std::floor(loopPoint * kRate);
        EXPECT_LT(worstDifference(out, 16, ref, 16, length - 32), tolerance) << "the first pass plays the clip";
        // where the stream went on from: the reference's frame that matches best
        int lag = 0;
        float best = 1e9f;
        for (int shift = -3000; shift <= 3000; shift++) {
            if ((long)point + 16 + shift < 0) continue;
            const float diff = worstDifference(out, length + 16, ref, point + 16 + shift, 1024);
            if (diff < best) {
                best = diff;
                lag = shift;
            }
        }
        EXPECT_LT(worstDifference(out, length + 16, ref, point + 16, 4096), tolerance)
            << "after the wrap the stream plays from its loop point (frame " << point << "); it plays from "
            << (long)point + lag << " (" << best << ")";
    }

}

// ---- S-1: an Ogg Vorbis stream reads through SoLoud's File ---------------------------------------

TEST(SoloudOggStream, StreamsFromAFileAndFromMemory) {
    // WavStream hands stb_vorbis a SoLoud File*; the stock stb_vorbis took it for a FILE* (glibc
    // aborts: invalid stdio handle). In a child first: on that build it dies.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::string path = fixture("spaceEngineLow_003.ogg");
    ASSERT_EXIT({
        jasperengine::EngineWavStream stream;
        const bool opened = stream.open(path);
        std::exit(opened ? 0 : 1);
    }, ::testing::ExitedWithCode(0), "");

    jasperengine::EngineWavStream fromFile;
    ASSERT_TRUE(fromFile.open(path));
    EXPECT_NEAR(fromFile.getLength(), 5.0, 1e-6) << "the last page's 220,500 frames";

    const std::vector<unsigned char> bytes = readBytes(path);
    SoLoud::WavStream fromMemory;
    ASSERT_EQ(fromMemory.loadMem(bytes.data(), (unsigned int)bytes.size(), true, false), SoLoud::SO_NO_ERROR);
    EXPECT_EQ(fromMemory.mSampleCount, 220500u);

    // both play what a whole decode does
    SoLoud::Wav whole;
    ASSERT_EQ(whole.load(path.c_str()), SoLoud::SO_NO_ERROR);
    NullSoloud reference;
    reference.play(whole);
    const std::vector<float> ref = reference.mix(kRate);
    for (SoLoud::WavStream* stream : std::vector<SoLoud::WavStream*>{&fromFile, &fromMemory}) {
        NullSoloud streamed;
        streamed.play(*stream);
        const std::vector<float> out = streamed.mix(kRate);
        EXPECT_LT(worstDifference(out, 0, ref, 0, kRate), 1e-6f);
        EXPECT_GT(*std::max_element(out.begin(), out.end()), 0.05f) << "not silence";
    }
}

// ---- S-2: a stream's seek (every loop wrap) lands on its frame ----------------------------------

TEST(SoloudStreamSeek, WavFlacAndMp3LoopsWrapToTheirLoopPoint) {
    // The seek tested the decoder union's Ogg member, which is set for every format, and handed
    // WAV, FLAC and MP3 decoders to stb_vorbis_seek: on that build the first wrap corrupts memory
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const fs::path wav = writeTemp("sweep.wav", wavBytes(1, kRate, [](unsigned int f, unsigned int) { return sweep(f); }));
    const std::vector<std::string> files = {wav.string(), fixture("sweep.flac"), fixture("sweep.mp3")};
    for (const std::string& file : files) {
        SCOPED_TRACE(file);
        ASSERT_EXIT({
            jasperengine::EngineWavStream stream;
            if (!stream.open(file)) std::exit(1);
            NullSoloud looping;
            const SoLoud::handle voice = looping.play(stream);
            looping.soloud.setLooping(voice, true);
            looping.soloud.setLoopPoint(voice, 0.25);
            looping.mix(3 * kRate);
            std::exit(0);
        }, ::testing::ExitedWithCode(0), "");

        jasperengine::EngineWavStream stream;
        ASSERT_TRUE(stream.open(file));
        SoLoud::Wav whole;
        ASSERT_EQ(whole.load(file.c_str()), SoLoud::SO_NO_ERROR);
        expectLoopWrapsToItsPoint(stream, whole, 0.25, 1e-5f);
    }
    fs::remove(wav);
}

TEST(SoloudStreamSeek, AnOggLoopWrapsToItsLoopPoint) {
    // The Ogg seek decoded the frame it sought for stb_vorbis_get_samples (which WavStream does
    // not use), so the next getAudio dropped it, and kept the frame it had before the seek: the
    // loop went on from the wrong place
    const std::string path = fixture("spaceEngineLow_003.ogg");
    jasperengine::EngineWavStream stream;
    ASSERT_TRUE(stream.open(path));
    SoLoud::Wav whole;
    ASSERT_EQ(whole.load(path.c_str()), SoLoud::SO_NO_ERROR);
    for (double point : {0.25, 1.0, 3.3}) {
        SCOPED_TRACE(point);
        expectLoopWrapsToItsPoint(stream, whole, point, 1e-4f);
    }
}

// ---- S-3: the whole-clip loaders stay within the clip -------------------------------------------

TEST(SoloudWavLoader, AnOggThatDecodesPastItsLastGranuleLoadsAtItsLength) {
    // stb_vorbis decodes this file a few hundred frames past its last page's granule, the length
    // Wav::loadogg allocates: the copy wrote them past mData (AddressSanitizer: heap overflow)
    SoLoud::Wav whole;
    ASSERT_EQ(whole.load(fixture("spaceEngineLow_003.ogg").c_str()), SoLoud::SO_NO_ERROR);
    EXPECT_EQ(whole.mSampleCount, 220500u);
    EXPECT_EQ(whole.mChannels, 1u);
}

TEST(SoloudWavLoader, FramesAShortReadLeavesAreSilent) {
    // a WAV whose data chunk says 2,048 frames and holds 1,000 (a cut file): dr_wav reads 1,000 and
    // loadwav copied every block whole, the frames after the read from the block before
    std::vector<unsigned char> bytes = wavBytes(1, 2048, [](unsigned int, unsigned int) { return 0.5f; });
    bytes.resize(44 + 1000 * 2);
    SoLoud::Wav cut;
    ASSERT_EQ(cut.loadMem(bytes.data(), (unsigned int)bytes.size(), true, false), SoLoud::SO_NO_ERROR);
    ASSERT_EQ(cut.mSampleCount, 2048u);
    EXPECT_NEAR(cut.mData[999], 0.5f, 1e-4f);
    for (unsigned int i = 1000; i < cut.mSampleCount; i++) {
        ASSERT_EQ(cut.mData[i], 0.0f) << "frame " << i << " was never decoded";
    }

    // a FLAC cut to two thirds of its bytes: STREAMINFO still counts the whole second
    std::vector<unsigned char> flac = readBytes(fixture("sweep.flac"));
    flac.resize(flac.size() * 2 / 3);
    SoLoud::Wav cutFlac;
    ASSERT_EQ(cutFlac.loadMem(flac.data(), (unsigned int)flac.size(), true, false), SoLoud::SO_NO_ERROR);
    ASSERT_EQ(cutFlac.mSampleCount, kRate);
    for (unsigned int i = kRate - 4096; i < kRate; i++) {
        ASSERT_EQ(cutFlac.mData[i], 0.0f) << "frame " << i << " is past what the cut file holds";
    }
}

// ---- S-5: a voice plays its last block to the end -----------------------------------------------

TEST(SoloudMix, AVoicePlaysItsLastBlockToTheEnd) {
    // 1,000 frames: the second 512-frame block holds the last 488. With 300-frame mix calls that
    // block is read in the call that ends at frame 600, and SoLoud stopped the voice there
    std::vector<unsigned char> bytes = wavBytes(1, 1000, [](unsigned int, unsigned int) { return 0.5f; });
    SoLoud::Wav clip;
    ASSERT_EQ(clip.loadMem(bytes.data(), (unsigned int)bytes.size(), true, false), SoLoud::SO_NO_ERROR);
    for (unsigned int call : {300u, 441u, 480u, 735u, 1024u}) {
        SCOPED_TRACE(call);
        NullSoloud mixer;
        const SoLoud::handle voice = mixer.play(clip);
        const std::vector<float> out = mixer.mix(4096, call);
        int heard = 0;
        for (float v : out) heard += std::fabs(v) > 0.1f;
        EXPECT_EQ(heard, 1000) << "every frame of the clip";
        EXPECT_FALSE(mixer.soloud.isValidVoiceHandle(voice)) << "and then it ends";
    }
}

// ---- a mix flushes denormals only while it runs -------------------------------------------------

// A thread that mixed (the main thread, on the null and deterministic drivers) computes denormals
// afterwards, as the noise kernels' determinism needs (T2 N2); a caller that flushes keeps flushing.
// Red before the patch: the first mix of the process left flush-to-zero on in its thread for good.
TEST(SoloudMix, LeavesTheCallersDenormalModes) {
    NullSoloud mixer;
    mixer.mix(2048);
    volatile float tiny = 1e-40f;
    volatile float half = 0.5f;
    EXPECT_NE(tiny * half, 0.0f) << "denormals are flushed in the thread that mixed";
#if defined(__SSE2__) || defined(_M_X64)
    const unsigned int modes = _mm_getcsr();
    _mm_setcsr(modes | 0x8040u);
    mixer.mix(2048);
    EXPECT_EQ(_mm_getcsr() & 0x8040u, 0x8040u) << "the caller's own flush-to-zero and denormals-are-zero";
    _mm_setcsr(modes);
#endif
}

// ---- S-6: a bus never reads ahead of its voices -------------------------------------------------

TEST(SoloudBus, AVoiceQueuedIntoABusIsMixedInTheNextCall) {
    // A bus reads its children's mix in 512-frame blocks and keeps what a call does not take: a
    // voice queued after a 300-frame call was first heard 212 frames later than one queued at a
    // block's start (SoLoud's linear resampler itself puts a frame between a block and its output)
    std::vector<unsigned char> bytes = wavBytes(1, kRate, [](unsigned int, unsigned int) { return 0.5f; });
    SoLoud::Wav clip;
    ASSERT_EQ(clip.loadMem(bytes.data(), (unsigned int)bytes.size(), true, false), SoLoud::SO_NO_ERROR);
    // the onset of the clip queued into a bus after a first mix call of `first` frames
    auto onsetAfter = [&clip](unsigned int first) {
        NullSoloud mixer;
        SoLoud::Bus bus;
        // the engine's buses (AudioMixer): never stopped by their end
        bus.setAutoStop(false);
        // (the bus goes first: its own destructor stops its voices)
        const SoLoud::handle busVoice = mixer.soloud.play(bus);
        mixer.soloud.setPanAbsolute(busVoice, 1.0f, 1.0f);
        mixer.mix(first, first);
        mixer.track(clip);
        bus.play(clip);
        return onset(mixer.mix(2048, 735));
    };
    const int atABlock = onsetAfter(512);
    ASSERT_GE(atABlock, 0);
    EXPECT_LE(atABlock, 2);
    for (unsigned int first : {300u, 735u, 1000u}) {
        EXPECT_EQ(onsetAfter(first), atABlock) << "after a first call of " << first << " frames";
    }
}
