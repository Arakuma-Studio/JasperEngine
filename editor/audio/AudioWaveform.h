// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AudioAnalysis.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Waveform overviews of sound files: COLUMNS columns of {min, max} per channel (int16, full
    // scale 32767) of the decoded file, cached by the SHA-1 of its bytes as
    // <project>/.jasperengine/cache/audio/<sha1>-wave-v1.bin beside the analysis cache. The
    // Inspector's clip view and the Project window's thumbnails read them.
    //
    // ofFile and ofBytes are synchronous (any thread: the thumbnail worker calls them). get is for
    // windows: it never decodes on the calling thread; a miss queues the file for one worker thread
    // (the newest request first), which wakes the editor loop when the overview is in.
    class AudioWaveform {
    public:
        static constexpr int VERSION = 1;
        static constexpr int COLUMNS = 1024;
        // overviews get() keeps (an overview of a stereo file is 8 KB)
        static constexpr size_t KEPT = 512;

        struct Data {
            unsigned int channels = 0;
            unsigned int sampleRate = 0;
            uint64_t frames = 0;
            // channel c's column i: peaks[(c * COLUMNS + i) * 2] its min, + 1 its max
            std::vector<int16_t> peaks;

            bool empty() const { return channels == 0 || peaks.size() != (size_t)channels * COLUMNS * 2; }
            int16_t min(unsigned int channel, int column) const { return peaks[((size_t)channel * COLUMNS + column) * 2]; }
            int16_t max(unsigned int channel, int column) const { return peaks[((size_t)channel * COLUMNS + column) * 2 + 1]; }
            double seconds() const { return sampleRate > 0 ? (double)frames / sampleRate : 0.0; }
        };

        // the overview of a decoded signal: column i spans frames [i x frames / COLUMNS,
        // (i + 1) x frames / COLUMNS), at least one frame (a file shorter than COLUMNS frames
        // repeats its frames)
        static Data compute(const AudioAnalysis::Signal& signal);

        // <cacheDirectory>/<sha1>-wave-v1.bin
        static std::filesystem::path cacheFile(const std::filesystem::path& cacheDirectory, const std::string& sha1);
        static bool read(const std::filesystem::path& cacheDirectory, const std::string& sha1, Data& out);
        // through io::writeFileAtomic, a regenerable cache
        static bool write(const std::filesystem::path& cacheDirectory, const std::string& sha1, const Data& data);
        // the bytes' overview: the cache file, else a decode written to it
        static bool ofBytes(const std::string& bytes, const std::filesystem::path& cacheDirectory, Data& out, std::string& error);
        // a file's: its SHA-1 remembered by path, size and modification time, then as ofBytes
        static bool ofFile(const std::filesystem::path& file, const std::filesystem::path& cacheDirectory, Data& out, std::string& error);

        struct Result {
            // queued or being made: draw nothing (or the file's icon) yet
            bool pending = true;
            std::shared_ptr<const Data> data;
            std::string error;
        };
        // A project's file (absolute). Main thread.
        static Result get(const Project* project, const std::filesystem::path& file);
        // drops every overview and the queue (a job running then is dropped when it ends)
        static void clear();
    };

}
