// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Git lookup (PATH, then Windows' default install folders), the argv-only clone and the zip
// extraction with miniz follow OpenEngine's Apps/Editor/Source/Projects/ProjectAcquisition.cpp and
// ZipArchive.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License.
// See NOTICE.md. The entry, size and space checks (R34) and the staging are ours.

#include "hub/ProjectImporter.h"

#include "hub/HubPaths.h"
#include "hub/StagingFolders.h"
#include "vcs/ProcessExec.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "libs/miniz/miniz.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::hub;

namespace fs = std::filesystem;

namespace {

    // the Unix file type of an entry made on a Unix system (its external attributes' high half)
    constexpr unsigned kTypeMask = 0170000;
    constexpr unsigned kRegular = 0100000;
    constexpr unsigned kDirectory = 0040000;

    struct ZipFile {
        mz_zip_archive archive{};
        FILE* file = nullptr;
        bool open = false;
        ~ZipFile() {
            if (open) mz_zip_reader_end(&archive);
            if (file) std::fclose(file);
        }
    };

    FILE* openRead(const fs::path& path) {
#ifdef _WIN32
        return _wfopen(path.wstring().c_str(), L"rb");
#else
        return std::fopen(path.string().c_str(), "rb");
#endif
    }

    struct Sink {
        std::ofstream* out;
        const std::atomic<bool>* stop;
    };

    size_t writeEntry(void* opaque, mz_uint64, const void* data, size_t size) {
        Sink* sink = static_cast<Sink*>(opaque);
        if (sink->stop && sink->stop->load()) return 0;   // a cancel ends the extraction
        sink->out->write(static_cast<const char*>(data), (std::streamsize)size);
        return sink->out->good() ? size : 0;
    }

    // an archive's UTF-8 name as a path (Windows reads a narrow string as the ANSI code page)
    fs::path utf8Path(const char* name) {
        const std::string text(name);
        return fs::path(std::u8string(text.begin(), text.end()));
    }

    void removeQuietly(const fs::path& path) {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

}

bool importer::extractZip(const fs::path& zip, const fs::path& destination, const ZipLimits& limits, const ImportProgress& progress,
                          std::string& error) {
    ZipFile z;
    z.file = openRead(zip);
    if (!z.file) {
        error = "cannot open " + zip.string();
        return false;
    }
    std::error_code ec;
    const uint64_t size = fs::file_size(zip, ec);
    if (ec || !mz_zip_reader_init_cfile(&z.archive, z.file, size, 0)) {
        error = zip.filename().string() + " is not a zip archive";
        return false;
    }
    z.open = true;
    const mz_uint count = mz_zip_reader_get_num_files(&z.archive);
    if (count > limits.maxEntries) {
        error = "the archive has " + std::to_string(count) + " entries (at most " + std::to_string(limits.maxEntries) + ")";
        return false;
    }
    // every entry checked before anything is written
    uint64_t total = 0;
    std::vector<mz_zip_archive_file_stat> stats(count);
    for (mz_uint i = 0; i < count; i++) {
        if (!mz_zip_reader_file_stat(&z.archive, i, &stats[i])) {
            error = "entry " + std::to_string(i) + " cannot be read";
            return false;
        }
        const mz_zip_archive_file_stat& stat = stats[i];
        std::string why;
        if (!isSafeArchiveEntry(stat.m_filename, why)) {
            error = "refused: " + why;
            return false;
        }
        const unsigned type = (stat.m_external_attr >> 16) & kTypeMask;
        const bool unixMade = (stat.m_version_made_by >> 8) == 3;
        if (unixMade && type != 0 && type != kRegular && type != kDirectory) {
            error = std::string("refused: \"") + stat.m_filename + "\" is a symbolic link or a device file";
            return false;
        }
        total += stat.m_uncomp_size;
        if (total > limits.maxTotalBytes) {
            error = "the archive unpacks to more than " + std::to_string(limits.maxTotalBytes >> 30) + " GiB";
            return false;
        }
    }
    fs::create_directories(destination, ec);
    if (ec) {
        error = "cannot create " + destination.string() + ": " + ec.message();
        return false;
    }
    const fs::space_info space = fs::space(destination, ec);
    if (!ec && space.available < total) {
        error = "not enough free disk space (" + std::to_string(total >> 20) + " MiB needed, " + std::to_string(space.available >> 20) +
                " MiB free)";
        return false;
    }
    std::atomic<bool> stop{false};
    uint64_t done = 0;
    for (mz_uint i = 0; i < count; i++) {
        const mz_zip_archive_file_stat& stat = stats[i];
        const fs::path target = destination / utf8Path(stat.m_filename);
        if (stat.m_is_directory) {
            fs::create_directories(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + target.string();
            return false;
        }
        Sink sink{&out, &stop};
        if (!mz_zip_reader_extract_to_callback(&z.archive, i, &writeEntry, &sink, 0)) {
            error = stop ? std::string("cancelled") : std::string("cannot extract ") + stat.m_filename;
            return false;
        }
        // what the stream still holds: a full disk here would otherwise publish a short file
        out.close();
        if (!out) {
            error = "cannot write " + target.string();
            return false;
        }
        done += stat.m_uncomp_size;
        if (progress && !progress(total > 0 ? (float)((double)done / (double)total) : 1.0f, stat.m_filename)) {
            stop = true;
            error = "cancelled";
            return false;
        }
        hub::staging::holdForTest();
    }
    return true;
}

fs::path importer::findProjectRoot(const fs::path& root) {
    std::error_code ec;
    if (fs::exists(root / "project.yaml", ec)) return root;
    std::vector<fs::path> level{root};
    for (int depth = 1; depth <= 2; depth++) {
        std::vector<fs::path> next;
        for (const fs::path& dir : level) {
            std::vector<fs::path> children;
            for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
                if (entry.is_directory(ec) && !entry.is_symlink(ec)) children.push_back(entry.path());
            }
            std::sort(children.begin(), children.end());
            for (const fs::path& child : children) {
                if (fs::exists(child / "project.yaml", ec)) return child;
                next.push_back(child);
            }
        }
        level = std::move(next);
    }
    return fs::path();
}

bool importer::importZip(const fs::path& zip, const fs::path& parent, const ZipLimits& limits, const ImportProgress& progress,
                         fs::path& projectDir, std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) {
        error = "the target folder " + parent.string() + " does not exist";
        return false;
    }
    hub::staging::sweep({{parent}, {}});
    const std::string stem = zip.stem().string();
    hub::staging::Claim claim(parent, stem.empty() ? "Project" : stem);
    if (!claim.ok()) {
        error = claim.error();
        return false;
    }
    const fs::path staging = claim.folder();
    if (!extractZip(zip, staging, limits, progress, error)) {
        removeQuietly(staging);
        return false;
    }
    const fs::path root = findProjectRoot(staging);
    if (root.empty()) {
        removeQuietly(staging);
        error = "no project.yaml in the archive (looked two folders deep)";
        return false;
    }
    const std::string name = folderName(root == staging ? stem : root.filename().string());
    const fs::path target = parent / (name.empty() ? "Project" : name);
    const std::string problem = targetProblem(target);
    if (!problem.empty()) {
        removeQuietly(staging);
        error = problem;
        return false;
    }
    if (fs::exists(target, ec)) fs::remove(target, ec);   // an empty folder
    fs::rename(root, target, ec);
    if (ec) {
        removeQuietly(staging);
        error = "cannot move the project to " + target.string() + ": " + ec.message();
        return false;
    }
    removeQuietly(staging);
    projectDir = target;
    if (progress) progress(1.0f, "Imported to " + target.string());
    return true;
}

fs::path importer::findGit(const std::string& configured, std::string& why) {
    vcs::ExecutableLookup lookup;
    lookup.configured = configured;
    fs::path git = vcs::findExecutable("git", lookup, &why);
#ifdef _WIN32
    if (git.empty()) {
        // Git for Windows' default places, when PATH does not have it
        for (const char* variable : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
            const char* base = std::getenv(variable);
            if (!base) continue;
            for (const char* relative : {"Git\\cmd\\git.exe", "Programs\\Git\\cmd\\git.exe"}) {
                std::error_code ec;
                const fs::path candidate = fs::path(base) / relative;
                if (fs::exists(candidate, ec)) return candidate;
            }
        }
    }
#endif
    return git;
}

float importer::gitProgress(const std::string& line) {
    const size_t percent = line.find('%');
    if (percent == std::string::npos || percent == 0) return -1.0f;
    size_t start = percent;
    while (start > 0 && std::isdigit((unsigned char)line[start - 1])) start--;
    if (start == percent) return -1.0f;
    return (float)std::atoi(line.substr(start, percent - start).c_str()) / 100.0f;
}

std::string importer::repositoryName(const std::string& url) {
    std::string path = url;
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    const size_t cut = path.find_last_of("/:");
    std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".git") == 0) name.resize(name.size() - 4);
    return folderName(name);
}

bool importer::importGit(const std::string& url, const fs::path& parent, const std::string& configuredGit, const ImportProgress& progress,
                         fs::path& projectDir, std::string& error) {
    std::string why;
    if (!isAcceptedGitUrl(url, why)) {
        error = why;
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) {
        error = "the target folder " + parent.string() + " does not exist";
        return false;
    }
    const fs::path git = findGit(configuredGit, why);
    if (git.empty()) {
        error = "git was not found (" + why + "): install Git or set it in Preferences > Version Control";
        return false;
    }
    const std::string name = repositoryName(url);
    const fs::path target = parent / (name.empty() ? "Project" : name);
    const std::string problem = targetProblem(target);
    if (!problem.empty()) {
        error = problem;
        return false;
    }
    hub::staging::sweep({{parent}, {}});
    hub::staging::Claim claim(parent, name.empty() ? "Project" : name);
    if (!claim.ok()) {
        error = claim.error();
        return false;
    }
    const fs::path staging = claim.folder();
    std::atomic<bool> stop{false};
    auto run = [&](const std::vector<std::string>& args, const fs::path& cwd, float base, float span, int& exitCode) {
        vcs::ProcessSpec spec;
        spec.exe = git;
        spec.args = args;
        spec.cwd = cwd;
        // never a credential prompt on a terminal the editor does not have
        spec.env = {{"GIT_TERMINAL_PROMPT", "0"}};
        vcs::ProcessStream process;
        vcs::ProcessStream::Callbacks callbacks;
        std::mutex lines;
        callbacks.stderrLine = [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(lines);
            const float p = gitProgress(line);
            if (progress && !progress(p >= 0.0f ? base + span * p : -1.0f, line)) stop = true;
        };
        callbacks.stdoutLine = callbacks.stderrLine;
        std::string startError;
        if (!process.start(spec, callbacks, startError)) {
            error = startError;
            return false;
        }
        process.closeStdin();
        while (!process.wait(50, exitCode)) {
            if (stop) {
                process.kill();
                process.wait(-1, exitCode);
                error = "cancelled";
                return false;
            }
        }
        return true;
    };
    int exitCode = 0;
    if (!run({"clone", "--depth", "1", "--progress", "--", url, staging.string()}, parent, 0.0f, 0.9f, exitCode)) {
        removeQuietly(staging);
        return false;
    }
    if (exitCode != 0) {
        removeQuietly(staging);
        error = "git clone failed (exit " + std::to_string(exitCode) + ")";
        return false;
    }
    hub::staging::holdForTest();
    // LFS objects when the project uses LFS and git-lfs is there
    std::ifstream attributes(staging / ".gitattributes");
    std::stringstream text;
    if (attributes) text << attributes.rdbuf();
    if (text.str().find("filter=lfs") != std::string::npos) {
        std::string lfsWhy;
        vcs::ExecutableLookup lookup;
        if (vcs::findExecutable("git-lfs", lookup, &lfsWhy).empty()) {
            if (progress) progress(-1.0f, "warning: the project uses Git LFS but git-lfs is not installed: its large files are pointers");
        } else if (!run({"lfs", "pull"}, staging, 0.9f, 0.1f, exitCode) || exitCode != 0) {
            if (error == "cancelled") {
                removeQuietly(staging);
                return false;
            }
            if (progress) progress(-1.0f, "warning: git lfs pull failed (exit " + std::to_string(exitCode) + ")");
            error.clear();
        }
    }
    const fs::path root = findProjectRoot(staging);
    if (root.empty()) {
        removeQuietly(staging);
        error = "the repository has no project.yaml (looked two folders deep)";
        return false;
    }
    if (root != staging) {
        // the project is a subfolder: only it moves into place
        fs::rename(root, target, ec);
        removeQuietly(staging);
    } else {
        if (fs::exists(target, ec)) fs::remove(target, ec);
        fs::rename(staging, target, ec);
    }
    if (ec) {
        removeQuietly(staging);
        error = "cannot move the project to " + target.string() + ": " + ec.message();
        return false;
    }
    projectDir = target;
    if (progress) progress(1.0f, "Imported to " + target.string());
    return true;
}

// ---- ProjectImporter

ProjectImporter::~ProjectImporter() {
    cancel();
    if (worker.joinable()) worker.join();
}

bool ProjectImporter::isZipSource(const std::string& source) {
    std::string lower = source;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0 && lower.find("://") == std::string::npos;
}

bool ProjectImporter::start(const std::string& source, const fs::path& parent, const std::string& configuredGit, std::string& startError) {
    if (running()) {
        startError = "an import is running";
        return false;
    }
    reset();
    active = true;
    worker = std::thread([this, source, parent, configuredGit]() {
        auto progress = [this](float f, const std::string& line) {
            if (f >= 0.0f) fraction = f;
            if (!line.empty()) {
                std::lock_guard<std::mutex> lock(mutex);
                log.push_back(line);
                if (log.size() > 400) log.erase(log.begin(), log.begin() + 200);
            }
            return !cancelled.load();
        };
        fs::path dir;
        std::string why;
        const bool success = isZipSource(source) ? importer::importZip(source, parent, ZipLimits(), progress, dir, why)
                                                 : importer::importGit(source, parent, configuredGit, progress, dir, why);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ok = success;
            failure = why;
            result = dir;
        }
        finished = true;
    });
    return true;
}

void ProjectImporter::cancel() {
    cancelled = true;
}

std::vector<std::string> ProjectImporter::logTail(size_t lines) const {
    std::lock_guard<std::mutex> lock(mutex);
    const size_t from = log.size() > lines ? log.size() - lines : 0;
    return std::vector<std::string>(log.begin() + (std::ptrdiff_t)from, log.end());
}

std::string ProjectImporter::error() const {
    std::lock_guard<std::mutex> lock(mutex);
    return failure;
}

fs::path ProjectImporter::projectDir() const {
    std::lock_guard<std::mutex> lock(mutex);
    return result;
}

void ProjectImporter::reset() {
    if (worker.joinable()) worker.join();
    active = false;
    finished = false;
    cancelled = false;
    fraction = 0.0f;
    std::lock_guard<std::mutex> lock(mutex);
    ok = false;
    log.clear();
    failure.clear();
    result.clear();
}
