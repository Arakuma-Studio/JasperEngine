// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsepriteLocator.h"

#include "Platform.h"
#include "util/FileUtils.h"
#include "util/ShellEnv.h"
#include "vcs/ProcessExec.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <system_error>

#if defined(_WIN32)
#include <cwchar>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace jasperengine::editor::pixelart::aseprite {

namespace {

    constexpr const char* kPreferenceKey = "externalTools.aseprite.path";
    constexpr const char* kFix = "Install Aseprite from https://www.aseprite.org, or set its executable in "
                                 "Preferences > External Tools > Aseprite (Locate...)";

    std::string lower(std::string text){
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    std::string trim(const std::string& text){
        size_t begin = 0, end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        return text.substr(begin, end - begin);
    }

    std::vector<std::string> splitList(const std::string& value, char separator){
        std::vector<std::string> out;
        std::string current;
        for (char c : value) {
            if (c == separator) {
                if (!current.empty()) out.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.empty()) out.push_back(current);
        return out;
    }

    // Collects the existing executables in order, each once (a PATH entry often repeats an
    // install already found by its fixed location)
    class Collector {
    public:
        explicit Collector(const LocatorEnvironment& env): env(env) {}

        void add(const fs::path& executable, const char* source){
            // a relative path would resolve against the editor's working directory: launches pass
            // absolute paths only (§6.8)
            if (executable.empty() || !executable.is_absolute()) return;
            if (!env.isExecutable || !env.isExecutable(executable)) return;
            std::string key = executable.lexically_normal().string();
            if (env.os == "windows" || env.os == "macos") key = lower(key);   // case-insensitive file systems
            if (!seen.insert(key).second) return;
            out.push_back({executable, source, ""});
        }

        // A user-given path: on macOS the Browse dialog picks the .app bundle, whose program
        // lives inside it
        void addUserPath(const std::string& text, const char* source){
            if (text.empty()) return;
            const fs::path path = FileUtils::pathFromUtf8(text);
            if (env.os == "macos" && lower(path.extension().string()) == ".app") {
                add(path / "Contents" / "MacOS" / "aseprite", source);
                return;
            }
            add(path, source);
        }

        void addFromPath(std::initializer_list<const char*> names){
            const std::string value = env.env ? env.env("PATH") : std::string();
            for (const std::string& entry : splitList(value, env.os == "windows" ? ';' : ':')) {
                const fs::path dir = FileUtils::pathFromUtf8(entry);
                // relative entries ("." among them) would run whatever the working directory holds
                if (!dir.is_absolute()) continue;
                for (const char* name : names) add(dir / name, "path");
            }
        }

        std::vector<AsepriteInstall> out;

    private:
        const LocatorEnvironment& env;
        std::set<std::string> seen;
    };

    std::string envValue(const LocatorEnvironment& env, const char* name){
        return env.env ? env.env(name) : std::string();
    }

    void addWindows(const LocatorEnvironment& env, Collector& collect){
        const std::string programFiles = envValue(env, "ProgramFiles");
        const std::string programFilesX86 = envValue(env, "ProgramFiles(x86)");
        const std::string localAppData = envValue(env, "LOCALAPPDATA");
        if (!programFiles.empty()) collect.add(FileUtils::pathFromUtf8(programFiles) / "Aseprite" / "Aseprite.exe", "program_files");
        if (!programFilesX86.empty()) collect.add(FileUtils::pathFromUtf8(programFilesX86) / "Aseprite" / "Aseprite.exe", "program_files_x86");
        if (!localAppData.empty()) collect.add(FileUtils::pathFromUtf8(localAppData) / "Programs" / "Aseprite" / "Aseprite.exe", "local_app_data");

        if (!programFilesX86.empty()) {
            const fs::path steam = FileUtils::pathFromUtf8(programFilesX86) / "Steam";
            // the Steam root is library 0 even when the .vdf is missing or unreadable
            std::vector<fs::path> libraries = {steam};
            if (env.readFile) {
                for (const fs::path& library : steamLibraryFolders(env.readFile(steam / "steamapps" / "libraryfolders.vdf"))) libraries.push_back(library);
            }
            for (const fs::path& library : libraries) collect.add(library / "steamapps" / "common" / "Aseprite" / "Aseprite.exe", "steam");
        }
        collect.addFromPath({"Aseprite.exe", "aseprite.exe"});
    }

    void addMacos(const LocatorEnvironment& env, Collector& collect){
        const fs::path bundleProgram = fs::path("Aseprite.app") / "Contents" / "MacOS" / "aseprite";
        collect.add(fs::path("/Applications") / bundleProgram, "applications");
        if (!env.home.empty()) {
            collect.add(env.home / "Applications" / bundleProgram, "user_applications");
            collect.add(env.home / "Library" / "Application Support" / "Steam" / "steamapps" / "common" / "Aseprite" / bundleProgram, "steam");
        }
    }

    void addLinux(const LocatorEnvironment& env, Collector& collect){
        collect.addFromPath({"aseprite"});
        if (!env.home.empty()) {
            collect.add(env.home / ".steam" / "steam" / "steamapps" / "common" / "Aseprite" / "aseprite", "steam");
            collect.add(env.home / ".local" / "share" / "Steam" / "steamapps" / "common" / "Aseprite" / "aseprite", "steam");
        }
        collect.add(fs::path("/opt") / "aseprite" / "aseprite", "opt");
    }

    std::string systemOs(){
#if defined(_WIN32)
        return "windows";
#elif defined(__APPLE__)
        return "macos";
#else
        return "linux";
#endif
    }

    std::string systemEnv(const std::string& name){
#if defined(_WIN32)
        // the wide variant: a user name with non-ASCII letters is in LOCALAPPDATA and PATH
        const std::wstring wide = fs::path(FileUtils::pathFromUtf8(name)).wstring();
        const wchar_t* value = _wgetenv(wide.c_str());
        return value ? FileUtils::pathToUtf8(fs::path(value)) : std::string();
#else
        const char* value = std::getenv(name.c_str());
        return value ? std::string(value) : std::string();
#endif
    }

    bool systemIsExecutable(const fs::path& path){
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return false;
#if defined(_WIN32)
        return true;
#else
        return access(path.c_str(), X_OK) == 0;
#endif
    }

    std::string systemReadFile(const fs::path& path){
        // a libraryfolders.vdf is a few KiB: anything far larger is not one
        constexpr std::streamoff kLimit = std::streamoff(1) << 20;
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        std::string text;
        char buffer[4096];
        while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
            text.append(buffer, static_cast<size_t>(in.gcount()));
            if (static_cast<std::streamoff>(text.size()) > kLimit) return {};
        }
        return text;
    }

    PixelProblem notFound(const LocatorEnvironment& env){
        PixelProblem problem;
        problem.kind = "aseprite_not_found";
        problem.fix = kFix;
        if (!env.preferencePath.empty()) {
            problem.field = kPreferenceKey;
            problem.message = "Aseprite was not found: the path in Preferences (" + env.preferencePath +
                              ") is not an executable, and no install was found in the usual locations";
        } else {
            problem.message = "Aseprite was not found in Preferences, JASPER_ASEPRITE or the usual install locations";
        }
        return problem;
    }

}

LocatorEnvironment LocatorEnvironment::fromSystem(const std::string& preferencePath){
    LocatorEnvironment env;
    env.os = systemOs();
    env.preferencePath = preferencePath;
    env.env = systemEnv;
    env.isExecutable = systemIsExecutable;
    env.readFile = systemReadFile;
    env.home = ShellEnv::homeDirectory();
    return env;
}

std::vector<fs::path> steamLibraryFolders(const std::string& vdf){
    // KeyValues text: quoted strings (\\ and \" escaped), braces for nesting, // comments
    struct Token {
        bool brace;
        std::string text;
    };
    std::vector<Token> tokens;
    for (size_t i = 0; i < vdf.size();) {
        const char c = vdf[i];
        if (c == '"') {
            std::string text;
            i++;
            while (i < vdf.size() && vdf[i] != '"') {
                if (vdf[i] == '\\' && i + 1 < vdf.size()) i++;
                text += vdf[i++];
            }
            i++;
            tokens.push_back({false, text});
        } else if (c == '{' || c == '}') {
            tokens.push_back({true, std::string(1, c)});
            i++;
        } else if (c == '/' && i + 1 < vdf.size() && vdf[i + 1] == '/') {
            while (i < vdf.size() && vdf[i] != '\n') i++;
        } else {
            i++;
        }
    }

    // The current layout: "path" inside each numbered block ("libraryfolders" > "1" > "path"); the
    // old one (before 2021): a numbered key of the root block whose value is the path itself.
    // Other numbered keys (the app ids under "apps") sit deeper and are not libraries.
    std::vector<fs::path> out;
    int depth = 0;
    for (size_t i = 0; i < tokens.size();) {
        if (tokens[i].brace) {
            depth += tokens[i].text == "{" ? 1 : -1;
            i++;
            continue;
        }
        if (i + 1 >= tokens.size() || tokens[i + 1].brace) {   // a block's name
            i++;
            continue;
        }
        const std::string& key = tokens[i].text;
        const std::string& value = tokens[i + 1].text;
        const bool numbered = !key.empty() && std::all_of(key.begin(), key.end(), [](char k) { return std::isdigit(static_cast<unsigned char>(k)) != 0; });
        if (!value.empty() && ((depth == 2 && lower(key) == "path") || (depth == 1 && numbered))) out.push_back(FileUtils::pathFromUtf8(value));
        i += 2;
    }
    return out;
}

std::vector<AsepriteInstall> asepriteCandidates(const LocatorEnvironment& env){
    Collector collect(env);
    collect.addUserPath(env.preferencePath, "preference");
    collect.addUserPath(envValue(env, "JASPER_ASEPRITE"), "JASPER_ASEPRITE");
    if (env.os == "windows") addWindows(env, collect);
    else if (env.os == "macos") addMacos(env, collect);
    else addLinux(env, collect);
    return std::move(collect.out);
}

std::optional<AsepriteInstall> findAseprite(const LocatorEnvironment& env, PixelProblem* problem){
    std::vector<AsepriteInstall> candidates = asepriteCandidates(env);
    if (!candidates.empty()) return candidates.front();
    if (problem) *problem = notFound(env);
    return std::nullopt;
}

std::string asepriteVersion(const fs::path& executable){
    vcs::ProcessSpec spec;
    spec.exe = executable;
    spec.args = {"--version"};
    spec.timeoutMs = 3000;
    spec.maxOutputBytes = 64 << 10;
    const vcs::ProcessResult run = vcs::runProcess(spec);
    if (!run.ok()) return {};
    const std::string text = trim(run.out.empty() ? run.err : run.out);
    const std::string line = trim(text.substr(0, text.find('\n')));
    // another program at an Aseprite path is not confirmed
    return lower(line).find("aseprite") != std::string::npos ? line : std::string();
}

std::optional<AsepriteInstall> detectAseprite(const LocatorEnvironment& env, PixelProblem* problem){
    std::vector<AsepriteInstall> candidates = asepriteCandidates(env);
    if (candidates.empty()) {
        if (problem) *problem = notFound(env);
        return std::nullopt;
    }
    for (AsepriteInstall& install : candidates) {
        install.version = asepriteVersion(install.executable);
        if (!install.version.empty()) return install;
    }
    return candidates.front();
}

AsepriteLocator& AsepriteLocator::instance(){
    static AsepriteLocator locator;
    return locator;
}

AsepriteLocator::~AsepriteLocator(){
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending = false;
    }
    if (worker.joinable()) worker.join();
}

void AsepriteLocator::start(const std::string& preferencePath){
    std::lock_guard<std::mutex> lock(mutex);
    if (requested && preference == preferencePath) return;
    requested = true;
    preference = preferencePath;
    generation++;
    done = false;
    found.reset();
    missing = {};
    pending = true;
    // a running worker takes the new request when its current detection ends: never a join that
    // waits on a process here
    if (running) return;
    if (worker.joinable()) worker.join();   // it has left run(): returns at once
    running = true;
    worker = std::thread(&AsepriteLocator::run, this);
}

void AsepriteLocator::run(){
    for (;;) {
        std::string path;
        uint64_t request = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!pending) {
                running = false;
                return;
            }
            pending = false;
            path = preference;
            request = generation;
        }
        PixelProblem problem;
        std::optional<AsepriteInstall> install = detectAseprite(LocatorEnvironment::fromSystem(path), &problem);
        std::lock_guard<std::mutex> lock(mutex);
        if (request != generation) continue;
        found = std::move(install);
        missing = std::move(problem);
        done = true;
    }
}

bool AsepriteLocator::ready() const {
    std::lock_guard<std::mutex> lock(mutex);
    return done;
}

std::optional<AsepriteInstall> AsepriteLocator::result() const {
    std::lock_guard<std::mutex> lock(mutex);
    return found;
}

PixelProblem AsepriteLocator::problem() const {
    std::lock_guard<std::mutex> lock(mutex);
    return missing;
}

void AsepriteLocator::invalidate(){
    std::lock_guard<std::mutex> lock(mutex);
    requested = false;
    generation++;
    done = false;
    found.reset();
    missing = {};
}

bool openInAseprite(const AsepriteInstall& install, const std::vector<fs::path>& files, PixelProblem* problem){
    auto fail = [&](const char* kind, std::string field, std::string message, std::string fix) {
        if (problem) *problem = {kind, std::move(field), std::move(message), std::move(fix)};
        return false;
    };
    if (install.executable.empty() || !install.executable.is_absolute())
        return fail("aseprite_not_found", kPreferenceKey, "no absolute Aseprite executable to open the files with", kFix);
    std::vector<std::string> argv = {FileUtils::pathToUtf8(install.executable)};
    for (const fs::path& file : files) {
        // a relative path would open relative to Aseprite's working directory, not the project
        if (!file.is_absolute())
            return fail("invalid_settings", "files", "'" + FileUtils::pathToUtf8(file) + "' is not an absolute path", "pass the file's absolute path");
        argv.push_back(FileUtils::pathToUtf8(file));
    }
    std::string why;
    if (!Platform::launchDetached(argv, &why))
        return fail("aseprite_not_found", kPreferenceKey, "Aseprite could not start (" + why + ")", kFix);
    return true;
}

}
