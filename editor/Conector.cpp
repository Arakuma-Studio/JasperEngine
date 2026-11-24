// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ObjectManager.h"
#include "Conector.h"

#include "editor/Out.h"
#include "util/EditorFingerprint.h"
#include "util/ScriptStatus.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "native/NativeBridge.h"

#include <iostream>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace jasperengine;

editor::Conector::Conector(){
}

editor::Conector::~Conector(){
    disconnect();
}

void* editor::Conector::loadSharedLibrary(const std::string& libPath) {
    #ifdef _WIN32
        HMODULE libHandle = LoadLibrary(libPath.c_str());
        if (!libHandle) {
            Out::error("Failed to load library: %s (Error code: %lu)", libPath.c_str(), GetLastError());
            return nullptr;
        }
        return libHandle;
    #else
        void* libHandle = dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!libHandle) {
            Out::error("Failed to load library: %s (Error: %s)", libPath.c_str(), dlerror());
            return nullptr;
        }
        return libHandle;
    #endif
}

void editor::Conector::unloadSharedLibrary(void* libHandle) {
    if (!libHandle) {
        return;
    }
    #ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(libHandle));
    #else
        dlclose(libHandle);
    #endif
    std::cout << "Library unloaded successfully.\n";
}



namespace {
    // A folder only this user can write: $XDG_RUNTIME_DIR/jasperengine_hotreload, else
    // <temp>/jasperengine_hotreload-<uid> made 0700 and checked (lstat: a real folder, ours, 0700);
    // "" when it can't be had. Windows: the user's own temporary folder.
    fs::path privateStagingDir() {
        std::error_code ec;
    #ifdef _WIN32
        const fs::path temp = fs::temp_directory_path(ec);
        return ec || temp.empty() ? fs::path() : temp / "jasperengine_hotreload";
    #else
        fs::path dir;
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (runtime && *runtime && fs::is_directory(runtime, ec)) {
            dir = fs::path(runtime) / "jasperengine_hotreload";
        } else {
            const fs::path temp = fs::temp_directory_path(ec);
            if (ec || temp.empty()) return fs::path();
            dir = temp / ("jasperengine_hotreload-" + std::to_string(static_cast<unsigned long long>(::geteuid())));
        }
        ::mkdir(dir.c_str(), 0700);
        struct stat st;
        if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 077) != 0) {
            jasperengine::editor::Out::warning("Not staging the hot-reload copy in %s: it is not a private folder of this user (a link, someone else's, or "
                         "open to others).", dir.string().c_str());
            return fs::path();
        }
        return dir;
    #endif
    }
}

void* editor::Conector::findSymbol(void* handle, const char* name) const {
    if (!handle) return nullptr;
    #ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
    #else
        dlerror();
        void* found = dlsym(handle, name);
        dlerror();
        return found;
    #endif
}

void* editor::Conector::symbol(size_t generation, const char* name) const {
    return generation < libraries.size() ? findSymbol(libraries[generation].handle, name) : nullptr;
}

bool editor::Conector::loadGeneration(const fs::path& buildPath, const std::string& libName, Library& out){
    lastCheck = LoadCheck();

    #ifdef _WIN32
        const std::string ext = ".dll";
    #elif defined(__APPLE__)
        const std::string ext = ".dylib";
    #else
        const std::string ext = ".so";
    #endif
    const std::string libPath = libName + ext;

    fs::path fullLibPath = buildPath / fs::path(libPath);
    if (!fs::exists(fullLibPath)) {
        lastCheck.message = "Library file not found: " + libPath;
        Out::error("%s", lastCheck.message.c_str());
        return false;
    }

    // The toolchain fingerprint, read from the file before anything of it runs (a mismatched
    // library's static initializers would already crash in dlopen)
    const ToolchainFingerprint& host = editorToolchainFingerprint();
    const ScriptLibraryCheck before = checkScriptLibrary(fullLibPath, host);
    lastCheck.mismatches = before.mismatches;
    ScriptStatus::get().setFingerprint(before.ok, before.mismatches);
    if (!before.ok) {
        lastCheck.refused = true;
        lastCheck.message = before.message;
        Out::error("%s", before.message.c_str());
        return false;
    }
    if (!before.message.empty()) {
        lastCheck.message = before.message;
        Out::warning("%s", before.message.c_str());
    }

    // Load a freshly-named COPY of the built library rather than the build
    // output directly. The dynamic loader identifies a library by its file
    // identity and dlclose()/FreeLibrary() are not guaranteed to unmap it, so
    // re-loading the same path after a rebuild can hand back the OLD code that
    // is still resident in the process (the reason a fresh rebuild only takes
    // effect after restarting the editor). On Windows LoadLibrary also keeps the
    // .dll locked, blocking the next build from overwriting it. A unique filename
    // per load forces the loader to map the new file every time.
    static std::atomic<unsigned long long> loadCounter{0};
    const std::string uniqueName = libName + "_" + std::to_string(loadCounter++) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ext;

    // Stage the copy in the build dir, falling back to the system temp dir if
    // that is not writable (disk full, permissions, read-only mount, ...). We
    // deliberately never fall back to loading fullLibPath directly: that path is
    // identity-cached by the loader and would silently re-run stale code, the
    // exact "build succeeded, Play runs old code" bug this staging prevents.
    std::error_code ec;
    std::vector<fs::path> stagingDirs;
    stagingDirs.push_back(buildPath / "hotreload");
    // the fallback outside the project: a private per-user folder (another user can't plant the
    // library we load there)
    const fs::path privateDir = privateStagingDir();
    if (!privateDir.empty()) {
        stagingDirs.push_back(privateDir);
    }

    // the in-project staging folder is the editor's build tree (.jasperengine/build/hotreload), refused
    // to agents by the deny-by-default guard: named here, on whichever thread connects
    safewrite::EditorInternal ownStaging(buildPath / "hotreload");
    fs::path copyPath;
    for (const fs::path& dir : stagingDirs) {
        std::error_code dirEc;
        // (in a write scope: beneath its root)
        FileUtils::createDirectories(dir, dirEc);

        // Best-effort sweep of copies left behind by previous sessions/crashes. Copies this
        // Play still has mapped (earlier generations) stay. Never through a link: a staging
        // folder that is a link (or, inside the project, one resolving outside it) is not
        // swept, and each entry is removed as an entry beneath the folder (util/SafeWrite.h)
        std::error_code linkEc;
        const bool folderIsLink = fs::is_symlink(fs::symlink_status(dir, linkEc));
        const bool inProject = !stagingProjectRoot.empty() && safewrite::isAtOrBelow(dir, stagingProjectRoot);
        const bool sweep = !folderIsLink && (!inProject || safewrite::isInsideResolved(dir, stagingProjectRoot));
        if (sweep) {
            const fs::path realDir = safewrite::resolvedPath(dir);
            for (const auto& entry : fs::directory_iterator(dir, dirEc)) {
                bool inUse = false;
                for (const Library& library : libraries) inUse = inUse || library.copy == entry.path();
                if (inUse) continue;
                std::string ignored;
                safewrite::removeFileBeneath(realDir, entry.path().filename(), ignored);
            }
        }

        fs::path candidate = dir / uniqueName;
        std::error_code cpEc;
        FileUtils::copyPath(fullLibPath, candidate, true, cpEc);
        if (!cpEc) {
            copyPath = candidate;
            break;
        }
        Out::warning("Could not stage hot-reload copy in %s (%s)", dir.string().c_str(), cpEc.message().c_str());
    }

    if (copyPath.empty()) {
        lastCheck.message = "Failed to stage a loadable copy of the script library; aborting to avoid running stale code";
        Out::error("%s", lastCheck.message.c_str());
        return false;
    }

    void* handle = loadSharedLibrary(copyPath.string());
    if (!handle) {
        std::error_code rmEc;
        fs::remove(copyPath, rmEc);
        lastCheck.message = "Failed to load the script library " + copyPath.filename().string();
        return false;
    }

    // ...and the export, compared after loading: both must match
    using FingerprintFn = const ToolchainFingerprint* (*)();
    auto exported = reinterpret_cast<FingerprintFn>(findSymbol(handle, "jasper_scriptToolchainFingerprint"));
    const ToolchainFingerprint* libraryFingerprint = exported ? exported() : nullptr;
    ScriptLibraryCheck after;
    if (!libraryFingerprint) {
        after.ok = false;
        after.message = "Refused script library: it exports no toolchain fingerprint (jasper_scriptToolchainFingerprint), rebuild it";
    } else {
        after = checkFingerprint(*libraryFingerprint, host);
    }
    if (!after.ok) {
        unloadSharedLibrary(handle);
        std::error_code rmEc;
        fs::remove(copyPath, rmEc);
        lastCheck.refused = true;
        lastCheck.message = after.message;
        lastCheck.mismatches = after.mismatches;
        ScriptStatus::get().setFingerprint(false, after.mismatches);
        Out::error("%s", after.message.c_str());
        return false;
    }

    out.handle = handle;
    out.copy = copyPath;
    lastCheck.ok = true;
    return true;
}

bool editor::Conector::connect(const fs::path& buildPath, std::string libName, const fs::path& projectRoot){
    disconnect();
    // the project whose staging folder may be swept (only beneath it, never through a link)
    stagingProjectRoot = projectRoot;
    Library library;
    if (!loadGeneration(buildPath, libName, library)) {
        if (lastCheck.refused) ScriptStatus::get().setCpp(CppScriptState::Refused, {}, lastCheck.message);
        return false;
    }
    libraries.push_back(library);
    current = 0;
    makeLatestCurrent();
    Out::info("Successfully connected to library");
    return true;
}

bool editor::Conector::connectNext(const fs::path& buildPath, const std::string& libName){
    if (libraries.empty()) return connect(buildPath, libName);
    if (libraries.size() >= MAX_GENERATIONS) {
        lastCheck = LoadCheck();
        lastCheck.message = "This Play has loaded " + std::to_string(MAX_GENERATIONS) + " script libraries: stop Play to reload again";
        return false;
    }
    Library library;
    if (!loadGeneration(buildPath, libName, library)) return false;
    libraries.push_back(library);
    return true;
}

void editor::Conector::makeLatestCurrent(){
    if (libraries.empty()) return;
    current = libraries.size() - 1;
    void* handle = libraries[current].handle;

    // GameObjects a script instantiates or destroys start and stop their C++ scripts
    // through these (generated with initScripts)
    using EntityScriptsFunc = ObjectManager::ScriptHook;
    EntityScriptsFunc initEntityScriptsFn = reinterpret_cast<EntityScriptsFunc>(findSymbol(handle, "initEntityScripts"));
    EntityScriptsFunc cleanupEntityScriptsFn = reinterpret_cast<EntityScriptsFunc>(findSymbol(handle, "cleanupEntityScripts"));
    if (!initEntityScriptsFn || !cleanupEntityScriptsFn) {
        Out::warning("The script library has no initEntityScripts/cleanupEntityScripts: C++ scripts of instantiated GameObjects will not run");
    }
    ObjectManager::setCppScriptHooks(initEntityScriptsFn, cleanupEntityScriptsFn);

    // C++ scripts of GameObjects DontDestroyOnLoad / MoveGameObjectToScene move (the
    // ScriptBase ones follow without it)
    ObjectManager::ScriptMoveHook moveEntityScriptsFn = reinterpret_cast<ObjectManager::ScriptMoveHook>(findSymbol(handle, "moveEntityScripts"));
    ObjectManager::setCppScriptMoveHook(moveEntityScriptsFn);
}

void editor::Conector::disconnect(){
    // the code behind them goes with the library
    ObjectManager::setCppScriptHooks(nullptr, nullptr);
    ObjectManager::setCppScriptMoveHook(nullptr);

    // C++ native plugins (plugins/native/cpp) are the library's code
    NativeBridge::clearCppPlugins();
    // every generation of this Play, newest first
    for (auto it = libraries.rbegin(); it != libraries.rend(); ++it) {
        unloadSharedLibrary(it->handle);
        // Safe to unlink even if the OS kept the library mapped: on POSIX the
        // inode outlives the directory entry, and on Windows this is best-effort.
        std::error_code ec;
        fs::remove(it->copy, ec);
    }
    if (!libraries.empty()) Out::info("Disconnected from library successfully");
    libraries.clear();
    current = 0;
}

void editor::Conector::init(Scene* scene){
    if (libraries.empty()) {
        Out::error("Cannot execute: Not connected to library");
        return;
    }

    using InitScriptsFunc = void (*)(Scene*);
    InitScriptsFunc initScriptsFn = reinterpret_cast<InitScriptsFunc>(findSymbol(libraries[current].handle, "initScripts"));
    if (!initScriptsFn) {
        Out::error("Failed to find function 'initScripts' in the library");
        return;
    }
    try {
        initScriptsFn(scene);
    } catch (const std::exception& e) {
        Out::error("Exception in initScripts(): %s", e.what());
    } catch (...) {
        Out::error("Unknown exception in initScripts()");
    }
}

void editor::Conector::cleanup(Scene* scene){
    if (libraries.empty()) {
        Out::error("Cannot cleanup: Not connected to library");
        return;
    }

    using CleanupScriptsFunc = void (*)(Scene*);
    CleanupScriptsFunc cleanupScriptsFn = reinterpret_cast<CleanupScriptsFunc>(findSymbol(libraries[current].handle, "cleanupScripts"));
    if (!cleanupScriptsFn) {
        Out::warning("Cleanup function not found in library");
        return;
    }
    try {
        cleanupScriptsFn(scene);
    } catch (const std::exception& e) {
        Out::error("Exception in cleanupScripts(): %s", e.what());
    } catch (...) {
        Out::error("Unknown exception in cleanupScripts()");
    }
}

bool editor::Conector::isLibraryConnected() const {
    return !libraries.empty();
}
