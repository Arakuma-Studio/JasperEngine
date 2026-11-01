// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// What SoundPool lets SoLoud decode (AudioFileCheck): files whose headers SoLoud trusts without a
// bound. Each load that could take a process down (a stack overflow, an allocation of gigabytes, a
// crash in stb_vorbis's cleanup) runs in a death test's child, with its address space limited
// where that is possible (Linux, no sanitizer), so a regression fails the test and not the machine.

#include <gtest/gtest.h>

#include "pool/SoundPool.h"

#include "soloud.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

using namespace jasperengine;

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

    void putU32(std::vector<unsigned char>& bytes, size_t at, uint32_t value) {
        for (int i = 0; i < 4; i++) bytes[at + i] = (unsigned char)(value >> (8 * i));
    }

    // a 16-bit PCM WAV of `channels` x `frames` at 0.25
    std::vector<unsigned char> wavBytes(unsigned int channels, unsigned int frames) {
        std::vector<unsigned char> out(44 + (size_t)frames * channels * 2, 0);
        std::memcpy(&out[0], "RIFF", 4);
        putU32(out, 4, (uint32_t)(out.size() - 8));
        std::memcpy(&out[8], "WAVEfmt ", 8);
        putU32(out, 16, 16);
        out[20] = 1;
        out[22] = (unsigned char)channels;
        putU32(out, 24, kRate);
        putU32(out, 28, kRate * channels * 2);
        out[32] = (unsigned char)(channels * 2);
        out[34] = 16;
        std::memcpy(&out[36], "data", 4);
        putU32(out, 40, frames * channels * 2);
        for (size_t i = 44; i + 1 < out.size(); i += 2) {
            const int16_t v = 8192;
            out[i] = (unsigned char)(v & 0xFF);
            out[i + 1] = (unsigned char)(v >> 8);
        }
        return out;
    }

    // Ogg's page CRC (polynomial 0x04C11DB7, not reflected, from 0), over the page with its CRC field zero
    uint32_t oggCrc(const unsigned char* data, size_t size) {
        uint32_t crc = 0;
        for (size_t i = 0; i < size; i++) {
            crc ^= (uint32_t)data[i] << 24;
            for (int bit = 0; bit < 8; bit++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
        return crc;
    }

    // the Kenney Ogg with its comment header counting `comments` comments, the page CRC rewritten
    std::vector<unsigned char> oggWithCommentCount(uint32_t comments) {
        std::vector<unsigned char> bytes = readBytes(fixture("spaceEngineLow_003.ogg"));
        // the second page holds the comment header ("\x03vorbis", vendor length and text, count)
        const size_t second = 27 + bytes[26] + bytes[27];
        EXPECT_EQ(std::memcmp(&bytes[second], "OggS", 4), 0);
        const size_t segments = bytes[second + 26];
        size_t pageSize = 27 + segments;
        for (size_t s = 0; s < segments; s++) pageSize += bytes[second + 27 + s];
        const size_t packet = second + 27 + segments;
        EXPECT_EQ(std::memcmp(&bytes[packet], "\x03vorbis", 7), 0);
        const uint32_t vendor = (uint32_t)bytes[packet + 7] | ((uint32_t)bytes[packet + 8] << 8) |
                                ((uint32_t)bytes[packet + 9] << 16) | ((uint32_t)bytes[packet + 10] << 24);
        putU32(bytes, packet + 11 + vendor, comments);
        putU32(bytes, second + 22, 0);
        putU32(bytes, second + 22, oggCrc(&bytes[second], pageSize));
        return bytes;
    }

    fs::path writeTemp(const std::string& name, const std::vector<unsigned char>& bytes) {
        const fs::path path = fs::temp_directory_path() / ("jasper-audiocheck-test-" + name);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        return path;
    }

    // a death test's child: an address space of what it uses plus 1 GiB (the allocations of a
    // bad header fail instead of taking the machine's memory). Not under a sanitizer, which
    // reserves its shadow memory up front.
    void limitMemory() {
#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
        std::ifstream statm("/proc/self/statm");
        unsigned long long pages = 0;
        statm >> pages;
        rlimit limit;
        limit.rlim_cur = limit.rlim_max = (rlim_t)(pages * 4096ull + (1ull << 30));
        setrlimit(RLIMIT_AS, &limit);
#endif
    }

    // The child's exit code: 0 when the pool refused the file (threw), 2 when it loaded it, and
    // when `played` the load was then mixed for a second on a SoLoud of its own
    int loadInChild(const std::string& id, const std::string& path, bool play) {
        limitMemory();
        std::shared_ptr<SoLoud::AudioSource> sound;
        try {
            sound = SoundPool::decode(id, path);
        } catch (const std::exception&) {
            return 0;
        }
        if (play && sound) {
            SoLoud::Soloud soloud;
            soloud.init(0, SoLoud::Soloud::NULLDRIVER, kRate, 512, 2);
            soloud.play(*sound);
            std::vector<float> out(1024 * 2);
            for (unsigned int i = 0; i < kRate / 1024; i++) soloud.mix(out.data(), 1024);
            soloud.deinit();
        }
        return 2;
    }

    const char* const kLoadTypes[] = {"decompress_on_load", "compressed_in_memory", "streaming"};

}

// ---- S-4: more channels than SoLoud's buffers hold -----------------------------------------------

TEST(AudioFileLoads, AWavOfMoreThanEightChannelsIsRefused) {
    // Wav::loadwav and WavStream decode 512 frames at a time into float[512 * 8]: 12 channels
    // overflow that stack buffer
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const fs::path path = writeTemp("twelve.wav", wavBytes(12, 4096));
    for (const char* type : kLoadTypes) {
        SCOPED_TRACE(type);
        const std::string id = path.string() + "#" + type;
        EXPECT_EXIT(std::exit(loadInChild(id, path.string(), true)), ::testing::ExitedWithCode(0), "");
    }
    fs::remove(path);
}

// ---- a WAV written to a pipe: its sizes say 0xFFFFFFFF -------------------------------------------

TEST(AudioFileLoads, APipeWrittenWavLoadsAtTheFramesItHolds) {
    // ffmpeg and sox write a WAV to a pipe with RIFF and data sizes of 0xFFFFFFFF: SoLoud took the
    // data size as the length (12 hours of 16-bit stereo, 8 GB decoded) of a 1 s file
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::vector<unsigned char> bytes = wavBytes(1, kRate);
    putU32(bytes, 4, 0xFFFFFFFFu);
    putU32(bytes, 40, 0xFFFFFFFFu);
    const fs::path path = writeTemp("pipe.wav", bytes);
    for (const char* type : kLoadTypes) {
        SCOPED_TRACE(type);
        const std::string id = path.string() + "#" + type;
        // its length, in the child: 0 when it is the second the file holds
        EXPECT_EXIT({
            limitMemory();
            try {
                std::shared_ptr<SoLoud::AudioSource> sound = SoundPool::decode(id, path.string());
                const double length = SoundPool::getLength(sound.get());
                std::exit(std::fabs(length - 1.0) < 1e-6 ? 0 : 3);
            } catch (const std::exception&) {
                std::exit(4);
            }
        }, ::testing::ExitedWithCode(0), "");
    }
    fs::remove(path);
}

// ---- an Ogg comment header that asks for gigabytes ------------------------------------------------

TEST(AudioFileLoads, AnOggCommentHeaderBeyondItsPacketIsRefused) {
    // stb_vorbis allocates the comment list at the count the header states before it reads it
    // (2^31 - 1 comments: 16 GB), and its cleanup after that allocation fails reads the null list
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const fs::path path = writeTemp("comments.ogg", oggWithCommentCount(0x7FFFFFFFu));
    for (const char* type : kLoadTypes) {
        SCOPED_TRACE(type);
        const std::string id = path.string() + "#" + type;
        EXPECT_EXIT(std::exit(loadInChild(id, path.string(), false)), ::testing::ExitedWithCode(0), "");
    }
    fs::remove(path);
}
