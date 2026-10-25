// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// An output stream for code that writes a file piece by piece (templates, generated files):
// the text collects in memory and replaces the file in one io::writeFileAtomic when the
// stream goes away, or at commit(). A crash never leaves half a file (docs/data-safety.md).

#include "io/AtomicFile.h"

#include <filesystem>
#include <sstream>
#include <string>

namespace jasperengine::editor {

    class AtomicFileStream : public std::ostringstream {
    public:
        explicit AtomicFileStream(std::filesystem::path path, io::Durability durability = io::Durability::Document):
                std::ostringstream(std::ios::out | std::ios::binary), path(std::move(path)), durability(durability) {}

        ~AtomicFileStream() override {
            commit();
        }

        AtomicFileStream(const AtomicFileStream&) = delete;
        AtomicFileStream& operator=(const AtomicFileStream&) = delete;

        // Writes now (once); false when the file could not be replaced
        bool commit() {
            if (!committed) {
                committed = true;
                io::AtomicWriteOptions options;
                options.durability = durability;
                result = io::writeFileAtomic(path, str(), options);
            }
            return result.ok();
        }

        const io::WriteResult& writeResult() const { return result; }

    private:
        std::filesystem::path path;
        io::Durability durability;
        bool committed = false;
        io::WriteResult result;
    };

}
