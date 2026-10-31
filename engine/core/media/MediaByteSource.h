// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MEDIABYTESOURCE_H
#define MEDIABYTESOURCE_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    // The bytes of a media file for the probes and the platform decoders (their custom I/O:
    // FFmpeg's AVIOContext, Media Foundation's IMFByteStream, Android's AMediaDataSource), so
    // assets in the resource pack, Android assets, user files and files still downloading
    // (GrowingFileReader) all play the same way. Positional and thread-safe: the decoders of a
    // source's video and its audio read it at once from the worker.
    class JASPERENGINE_API MediaByteSource {
    public:
        virtual ~MediaByteSource() = default;

        // up to `size` bytes at `offset` into dst: the count read (fewer only at the end, 0 past
        // it), -1 when the source failed or was aborted. A growing source blocks until the bytes
        // are there, the download failed, or abort()
        virtual int64_t readAt(int64_t offset, void* dst, size_t size) = 0;
        // the total size; -1 while a growing source does not know it yet
        virtual int64_t size() const = 0;
        // as given (an asset path, data://..., an absolute path); used in messages
        virtual const std::string& path() const = 0;
        // a path the platform's own APIs can open (a file on disk), "" when there is none (a
        // packed asset, memory, a file still downloading)
        virtual std::string systemPath() const { return std::string(); }
        // the bytes are still arriving (a download in progress)
        virtual bool isGrowing() const { return false; }
        // bytes readable now without blocking
        virtual int64_t availableBytes() const { return size(); }
        // reads blocked now and later fail (-1): the source's owner is going
        virtual void abort() {}

        // the file at an engine path (FileData: assets, the resource pack, data://, absolute
        // paths, Android assets); null with `error` when it cannot be opened
        static std::unique_ptr<MediaByteSource> openFile(const std::string& path, std::string& error);
        // an open stdio file, owned from here on (host files outside the engine's file system:
        // the editor's importer); null when it has no size
        static std::unique_ptr<MediaByteSource> openStdio(FILE* file, const std::string& name);
        // bytes in memory (the tests, extracted pack entries)
        static std::unique_ptr<MediaByteSource> openMemory(std::vector<uint8_t> bytes, const std::string& name);
        // reads the whole source (capped at maxBytes; false when larger or unreadable)
        static bool readAll(MediaByteSource& source, std::vector<uint8_t>& out, size_t maxBytes);
    };

}

#endif //MEDIABYTESOURCE_H
