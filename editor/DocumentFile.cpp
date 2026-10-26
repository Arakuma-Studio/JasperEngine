// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DocumentFile.h"
#include "util/ParticleMigrationReport.h"

#include "Stream.h"
#include "StreamSchema.h"
#include "util/FileUtils.h"
#include "util/SHA1.h"
#include "Out.h"
#include "StreamPreserved.h"

#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

bool DocumentFiles::readBytes(const fs::path& path, std::string& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

YAML::Node DocumentFiles::parse(const std::string& bytes, const std::string& displayPath) {
    try {
        return YAML::Load(bytes);
    } catch (const YAML::ParserException& e) {
        const int line = e.mark.line >= 0 ? e.mark.line + 1 : 0;
        const int column = e.mark.column >= 0 ? e.mark.column + 1 : 0;
        throw DocumentParseError(e.mark, e.msg, displayPath + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " + e.msg);
    }
}

DocumentFile DocumentFiles::read(const fs::path& path, DocumentKind kind) {
    std::string bytes;
    if (!readBytes(path, bytes))
        throw YAML::BadFile(path.string());
    return fromBytes(std::move(bytes), FileUtils::pathToUtf8(path), kind);
}

DocumentFile DocumentFiles::fromBytes(std::string bytes, const std::string& displayPath, DocumentKind kind) {
    DocumentFile document;
    document.bytes = std::move(bytes);
    document.root = parse(document.bytes, displayPath);
    document.sha1 = sha1(document.bytes);
    document.migration = StreamMigrations::migrate(kind, document.root);
    recordVersionIssues(kind, document.migration);
    ParticleMigrationReport::record(displayPath, document.migration.log.lines);
    return document;
}

void DocumentFiles::recordVersionIssues(DocumentKind kind, const MigrationResult& result) {
    if (!ScopedLoadReport::current())
        return;
    LoadReport* report = ScopedLoadReport::current();
    report->versionRead = result.versionRead;
    report->versionCurrent = result.versionCurrent;
    if (result.newer()) {
        LoadIssue issue;
        issue.kind = LoadIssueKind::NewerFormat;
        // everything this editor does not know of the newer format may be lost: one dropped item
        issue.fate = LoadIssueFate::Dropped;
        issue.field = StreamMigrations::versionKey(kind);
        issue.value = std::to_string(result.versionRead);
        issue.message = StreamMigrations::newerFormatMessage(kind, result.versionRead);
        recordLoadIssue(std::move(issue));
        return;
    }
    if (result.migrated()) {
        LoadIssue issue;
        issue.kind = LoadIssueKind::Migrated;
        issue.fate = LoadIssueFate::Info;
        issue.field = StreamMigrations::versionKey(kind);
        std::string applied;
        for (const std::string& name : result.applied)
            applied += (applied.empty() ? "" : ", ") + name;
        issue.message = std::string("read as ") + StreamMigrations::kindName(kind) + " format " +
                std::to_string(result.versionRead) + " (" + applied + "); the next save writes format " +
                std::to_string(result.versionCurrent);
        for (const std::string& line : result.log.lines)
            issue.message += "; " + line;
        recordLoadIssue(std::move(issue));
    }
}

std::string DocumentFiles::text(const YAML::Node& root) {
    // Trailing newline: git reports a file without one as changed as soon as anything is
    // appended to it
    return YAML::Dump(root) + "\n";
}

std::string DocumentFiles::sha1(const std::string& bytes) {
    return SHA1::hash(bytes);
}

std::string DocumentFiles::fileSha1(const fs::path& path) {
    std::string bytes;
    return readBytes(path, bytes) ? sha1(bytes) : std::string();
}

io::WriteResult DocumentFiles::write(const fs::path& path, const std::string& text) {
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Document;
    return io::writeFileAtomic(path, text, options);
}

std::string DocumentFiles::saveFailureMessage(const fs::path& path, const io::WriteResult& result) {
    return "Couldn't save \"" + FileUtils::pathToUtf8(path) + "\": " + io::describeWriteError(result) +
            ". Your changes are still in the editor.";
}

void DocumentFiles::overlayUnknownRootKeys(YAML::Node& encoded, const fs::path& path, RootKind kind) {
    std::string bytes;
    if (!encoded.IsMap() || !readBytes(path, bytes))
        return;
    YAML::Node current;
    try {
        current = YAML::Load(bytes);
    } catch (const YAML::Exception&) {
        return;
    }
    if (!current.IsMap())
        return;
    for (auto it = current.begin(); it != current.end(); ++it) {
        if (!it->first.IsScalar())
            continue;
        const std::string key = it->first.Scalar();
        if (StreamSchema::isKnownRootKey(kind, key) || encoded[key])
            continue;
        encoded[key] = it->second;
    }
}

std::string DocumentFiles::materialText(const YAML::Node& encodedMaterial, const fs::path& existingFile) {
    YAML::Node root = YAML::Clone(encodedMaterial);
    StreamMigrations::stampCurrentVersion(DocumentKind::Material, root);
    overlayUnknownRootKeys(root, existingFile, RootKind::Material);
    // no trailing newline: material files never had one
    return YAML::Dump(root);
}

LoadReport DocumentFiles::materialFileReport(const fs::path& path) {
    LoadReport report;
    report.documentPath = FileUtils::pathToUtf8(path.filename());
    std::error_code ec;
    if (!fs::exists(path, ec))
        return report;
    try {
        ScopedLoadReport scope(report);
        DocumentFile document = read(path, DocumentKind::Material);
        StreamLoad::reportRootKeys(document.root, RootKind::Material);
        StreamLoad::BlockScope block(nullptr, "", document.root, RootKind::Material);
        Stream::decodeMaterial(document.root);
    } catch (const std::exception&) {
        // unreadable: a new write replaces it, as before
    }
    return report;
}

SaveResult DocumentFiles::saveMaterial(const fs::path& path, const YAML::Node& encodedMaterial, DegradedSavePolicy policy, std::string* error) {
    if (policy == DegradedSavePolicy::Refuse) {
        const LoadReport report = materialFileReport(path);
        if (report.dropped() > 0) {
            if (error) {
                *error = "\"" + FileUtils::pathToUtf8(path.filename()) + "\" did not load completely; saving it would permanently remove " +
                        std::to_string(report.dropped()) + " value(s) this editor could not read";
            }
            Out::warning("Material not saved: %s", error ? error->c_str() : FileUtils::pathToUtf8(path).c_str());
            return SaveResult::RefusedDegraded;
        }
    }
    const io::WriteResult written = write(path, materialText(encodedMaterial, path));
    if (!written.ok()) {
        const std::string message = saveFailureMessage(path, written);
        if (error) *error = message;
        Out::error(message);
        return SaveResult::Failed;
    }
    return SaveResult::Saved;
}
