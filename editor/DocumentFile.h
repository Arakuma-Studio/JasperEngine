// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Reading and writing the editor's YAML documents (scenes, prefabs, project.yaml, .material)
// the data-safe way (docs/data-safety.md): a parse error names file:line:col and the file is
// never written over; migrations run before any decode; the bytes' SHA-1 is kept for crash
// recovery; every write is an atomic Document write.

#include "LoadReport.h"
#include "StreamMigrations.h"
#include "io/AtomicFile.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace jasperengine::editor {

    enum class RootKind;  // StreamSchema.h

    // A YAML syntax error: what() is "<file>:<line>:<column>: <message>". Still a
    // YAML::ParserException, so the loaders' existing catch blocks see it.
    class DocumentParseError : public YAML::ParserException {
    public:
        DocumentParseError(const YAML::Mark& mark, const std::string& message, std::string text):
                YAML::ParserException(mark, message), text(std::move(text)) {}
        const char* what() const noexcept override { return text.c_str(); }
    private:
        std::string text;
    };

    struct DocumentFile {
        YAML::Node root;
        std::string bytes;
        std::string sha1;
        MigrationResult migration;
    };

    class DocumentFiles {
    public:
        // Reads, parses and migrates a document. Throws DocumentParseError for a syntax error
        // and YAML::BadFile when it cannot be read. With a report installed, the migrations and
        // a newer format are recorded into it (recordVersionIssues).
        static DocumentFile read(const std::filesystem::path& path, DocumentKind kind);
        // The same from bytes already read (a crash backup's)
        static DocumentFile fromBytes(std::string bytes, const std::string& displayPath, DocumentKind kind);
        // Parses text; a syntax error becomes a DocumentParseError naming displayPath
        static YAML::Node parse(const std::string& bytes, const std::string& displayPath);
        // Migrated (info) and NewerFormat (dropped: the save is gated) issues of a load
        static void recordVersionIssues(DocumentKind kind, const MigrationResult& result);

        // The text a document is written as (a trailing newline, like every save before)
        static std::string text(const YAML::Node& root);
        static std::string sha1(const std::string& bytes);
        // The SHA-1 of a file's bytes, empty when it cannot be read
        static std::string fileSha1(const std::filesystem::path& path);
        static bool readBytes(const std::filesystem::path& path, std::string& bytes);

        // An atomic Document write of a document's text
        static io::WriteResult write(const std::filesystem::path& path, const std::string& text);
        // `Couldn't save "<path>": <reason>. Your changes are still in the editor.`
        static std::string saveFailureMessage(const std::filesystem::path& path, const io::WriteResult& result);

        // R28: a whole-file document (project.yaml, .material) keeps the root keys of its current
        // file that this editor does not know, after the encoded ones
        static void overlayUnknownRootKeys(YAML::Node& encoded, const std::filesystem::path& path, RootKind kind);

        // The text a material is written as: formatVersion first, the existing file's unknown
        // root keys kept (R28)
        static std::string materialText(const YAML::Node& encodedMaterial, const std::filesystem::path& existingFile);
        // Writes a .material file. An existing file that does not read completely (an enum value
        // this editor does not name, a newer format) is not written over unless the loss was
        // accepted: RefusedDegraded, with why in error.
        static SaveResult saveMaterial(const std::filesystem::path& path, const YAML::Node& encodedMaterial,
                DegradedSavePolicy policy = DegradedSavePolicy::Refuse, std::string* error = nullptr);
        // The dropped issues of reading a material file as it is now
        static LoadReport materialFileReport(const std::filesystem::path& path);
    };

}
