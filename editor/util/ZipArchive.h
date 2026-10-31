// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Zip archives through miniz (libs/miniz; docs/specs/S30-shipping.md D8): the player runtimes
// the editor installs, the APKs and bundles it assembles.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct ZipEntryInfo {
        std::string name;          // '/'-separated, as stored
        uint64_t size = 0;
        uint64_t compressedSize = 0;
        bool directory = false;
        bool executable = false;   // a Unix mode with an exec bit
        bool stored = false;       // not compressed
    };

    class ZipReader {
    public:
        ZipReader();
        ~ZipReader();
        ZipReader(const ZipReader&) = delete;
        ZipReader& operator=(const ZipReader&) = delete;

        bool open(const std::filesystem::path& path, std::string& error);
        const std::vector<ZipEntryInfo>& entries() const { return list; }
        bool read(const std::string& name, std::vector<uint8_t>& out, std::string& error);
        // every entry below `destination` (made); refuses absolute, "..", "\" and empty-part
        // names before writing anything
        bool extractAll(const std::filesystem::path& destination, std::string& error);

        // the legal-path rule of archive names (empty when legal)
        static std::string checkName(const std::string& name);

    private:
        void* archive = nullptr;
        std::vector<ZipEntryInfo> list;
    };

    class ZipWriter {
    public:
        ZipWriter();
        ~ZipWriter();
        ZipWriter(const ZipWriter&) = delete;
        ZipWriter& operator=(const ZipWriter&) = delete;

        bool open(const std::filesystem::path& path, std::string& error);
        // store: no compression (an APK's native libraries and resources.arsc)
        bool add(const std::string& name, const void* data, size_t size, bool store, std::string& error);
        bool addFile(const std::string& name, const std::filesystem::path& file, bool store, std::string& error);
        bool finish(std::string& error);

    private:
        void* archive = nullptr;
        bool opened = false;
    };

}
