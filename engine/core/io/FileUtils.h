// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FILEUTILS_H
#define FILEUTILS_H

#include "Export.h"
#include <string>
#include <vector>

namespace jasperengine {

    // Whole-file helpers for game saves and data files (the Lua File.readText/writeText/...
    // functions, Json.readFile/writeFile). Paths:
    // - "data://save.json": the user data directory (System::getUserDataPath), writable;
    // - "asset://data/items.json", "lua://data/items.json": read-only (inside the APK on
    //   Android, inside the resource pack when the export packs them);
    // - a bare relative path ("saves/slot1.json") means "data://saves/slot1.json";
    // - an absolute path: reads on desktop and in the editor only, never writes;
    // - a ".." segment, a ':' inside a segment or an unknown "scheme://" is refused (saves stay
    //   portable); '\' separates like '/'.
    // Errors are short messages naming the path: "not found: data://save.json",
    // "permission denied: data://save.json", "read-only path: asset://a.json".
    // Thread-safe for distinct paths (no shared state).
    class JASPERENGINE_API FileUtils {
    public:
        // `path` as the engine path it stands for ("data://saves/slot1.json") in `normalized`
        // and the file system path in `systemPath`; false with `error` when the rules refuse it
        static bool resolve(const std::string& path, bool forWrite, std::string& normalized, std::string& systemPath, std::string& error);

        // The bytes of the file, as they are
        static bool readText(const std::string& path, std::string& out, std::string& error);
        // Replaces the file atomically (a temporary file next to it, flushed to disk, renamed
        // over it: a crash leaves the old file or the new one, never half of one), making the
        // missing parent folders; then System::syncFileSystem (the web build's IndexedDB)
        static bool writeText(const std::string& path, const std::string& text, std::string& error);
        // Adds to the end (not atomic), making the file and its missing parent folders
        static bool appendText(const std::string& path, const std::string& text, std::string& error);
        // A file or a folder is there
        static bool exists(const std::string& path);
        // Removes a file (never a folder); true when nothing is there
        static bool remove(const std::string& path, std::string& error);
        // Makes the folder and the missing ones above it; true when it is there already
        static bool makeDirs(const std::string& path, std::string& error);
        // Names of the files and folders in a data:// folder, sorted (byte order)
        static bool list(const std::string& dir, std::vector<std::string>& names, std::string& error);

        // File system level (no path rules; UTF-8 paths, on Windows too): `bytes` into
        // `systemPath` as writeText does (missing parent folders made), without the
        // syncFileSystem
        static bool writeFileAtomic(const std::string& systemPath, const std::string& bytes, std::string& error);
        // Desktop exports (System::getUserDataPath with JASPERENGINE_APP_IDENTIFIER): makes the
        // per-user data folder and, when it has no UserSettings.xml yet, copies the one an older
        // build kept in the working directory (logged). False (logged) when the folder can't be
        // made.
        static bool prepareUserDataFolder(const std::string& systemDir);
        // Renames (replacing what is at `toSystemPath`)
        static bool renameFile(const std::string& fromSystemPath, const std::string& toSystemPath, std::string& error);
    };

}

#endif // FILEUTILS_H
