// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT)
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// From OpenEngine's Engine/Modules/AssetCore/Include/AssetCore/SharedFileRead.h (NOTICE.md),
// plus a FILE* form for File::open.

#ifndef JASPERENGINE_SHAREDFILEREADER_H
#define JASPERENGINE_SHAREDFILEREADER_H

#include "Export.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine {

    // Read access to files another program may replace while we read them (S05 R17).
    //
    // On Windows every open here grants FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    // so an external safe-save (write a temp file, then MoveFileEx(REPLACE_EXISTING) over the
    // target: Photoshop, VS Code, most DCC tools) never fails against a handle the engine
    // holds: the replace succeeds and the read keeps consuming the displaced old bytes; the
    // file watcher then delivers the new content as its own change. fopen and std::ifstream
    // cannot express this (they never share delete). Elsewhere it is a plain read-only open.
    class JASPERENGINE_API SharedFileReader {
    public:
        SharedFileReader() = default;
        explicit SharedFileReader(const std::filesystem::path& path);
        ~SharedFileReader();

        SharedFileReader(SharedFileReader&& other) noexcept;
        SharedFileReader& operator=(SharedFileReader&& other) noexcept;
        SharedFileReader(const SharedFileReader&) = delete;
        SharedFileReader& operator=(const SharedFileReader&) = delete;

        bool open(const std::filesystem::path& path);
        void close();
        bool isOpen() const;

        // Current file size; -1 when unavailable
        int64_t size() const;
        // Absolute seek from the start of the file
        bool seekTo(uint64_t offset);
        // Bytes read (0 at EOF); -1 on error
        int64_t read(void* dst, uint64_t bytes);

        // A read-only binary stdio stream on the file with the same sharing (fclose it);
        // nullptr when it cannot be opened. `utf8Path` is UTF-8.
        static FILE* openStream(const char* utf8Path);

        // The whole file; false when it cannot be opened or a read fails (an empty file is
        // a success with empty output)
        static bool readAll(const std::filesystem::path& path, std::vector<uint8_t>& out);
        static bool readAll(const std::filesystem::path& path, std::string& out);

    private:
#ifdef _WIN32
        void* handle = nullptr;   // HANDLE; nullptr when closed
#else
        int fd = -1;
#endif
    };

}

#endif // JASPERENGINE_SHAREDFILEREADER_H
