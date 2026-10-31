// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GROWINGFILEREADER_H
#define GROWINGFILEREADER_H

#include "Export.h"
#include "MediaByteSource.h"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace jasperengine {

    // A file being downloaded, shared by its writer (MediaCache, on the main thread, from the
    // HTTP client's progress) and its readers (decoder threads). `available` bytes of partPath
    // can be read; once complete the whole file is finalPath (the download renamed it).
    class JASPERENGINE_API GrowingFileState {
    public:
        GrowingFileState(std::string partPath, std::string finalPath);

        // the writer
        void update(int64_t available, int64_t total);
        void complete(int64_t total);
        void fail(const std::string& error);

        int64_t getAvailable() const;
        int64_t getTotal() const;            // -1 unknown
        bool isComplete() const;
        bool isFailed() const;
        std::string getError() const;
        const std::string& getPartPath() const { return partPath; }
        const std::string& getFinalPath() const { return finalPath; }

    private:
        friend class GrowingFileReader;
        mutable std::mutex mutex;
        std::condition_variable changed;
        const std::string partPath;
        const std::string finalPath;
        int64_t available = 0;
        int64_t total = -1;
        bool done = false;
        bool failed = false;
        std::string error;
    };

    // Blocking positional reads over a GrowingFileState (docs/superpowers/specs/
    // 2026-10-31-video-mp4-design.md §6.6, progressive start): a read past what has arrived waits
    // until the bytes are there, the download fails (-1) or abort() (-1). Reads only ever see
    // bytes the writer reported, so a decoder never reads a hole.
    class JASPERENGINE_API GrowingFileReader : public MediaByteSource {
    public:
        // name: the URL, for messages
        GrowingFileReader(std::shared_ptr<GrowingFileState> state, std::string name);
        ~GrowingFileReader() override;

        int64_t readAt(int64_t offset, void* dst, size_t size) override;
        int64_t size() const override;
        const std::string& path() const override;
        bool isGrowing() const override;
        int64_t availableBytes() const override;
        void abort() override;

        // a reader that never blocks: reads past what has arrived end there (probes on the main
        // thread of a file still downloading)
        static std::unique_ptr<MediaByteSource> snapshot(std::shared_ptr<GrowingFileState> state, std::string name);

        // opens a file for reading that its writer may still append to and rename (Windows:
        // shared read/write/delete access, else fopen)
        static FILE* openShared(const std::string& systemPath);

    private:
        std::shared_ptr<GrowingFileState> state;
        std::string name;
        std::mutex fileMutex;
        FILE* file = nullptr;
        bool aborted = false;       // under state->mutex
    };

}

#endif //GROWINGFILEREADER_H
