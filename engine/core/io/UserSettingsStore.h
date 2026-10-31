// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef USERSETTINGSSTORE_H
#define USERSETTINGSSTORE_H

#include "Export.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace jasperengine {

    class System;

    // The UserSettings (PlayerPrefs) store of desktop exports and the editor: the System base
    // class keeps its keys here, while Android (SharedPreferences), Apple (NSUserDefaults) and
    // the web (localStorage) have their own. One file, data://UserSettings.xml, read once (the
    // first use) and kept in memory: getters never touch the disk and setters only mark the
    // store changed. Engine::runFrame calls tick(), which writes once the oldest unsaved change
    // is a second old (WRITE_DELAY); save() (UserSettings.save, app pause, shutdown, the editor's
    // Play stop) writes at once. Writes are atomic (UserSettings.xml.tmp renamed over it); a
    // failed one is logged once per session and tried again a second later.
    //
    // File format (as before; files of older builds read the same):
    //   <?xml version="1.0" encoding="UTF-8"?>
    //   <userSettings>
    //       <Minigame_Money>120</Minigame_Money>
    //       <entry key="best score (easy)">31</entry>      a key that is no XML name
    //       <blob encoding="base64">AAEC</blob>            a value XML text cannot hold exactly
    //   </userSettings>
    // Keys are written sorted. Thread-safe (a mutex); the Lua API is main-thread anyway.
    class JASPERENGINE_API UserSettingsStore {
    public:
        static constexpr double WRITE_DELAY = 1.0;

        static UserSettingsStore& instance();

        bool get(const std::string& key, std::string& value);
        bool has(const std::string& key);
        void set(const std::string& key, const std::string& value);
        void remove(const std::string& key);
        void clear();
        // Every key and its stored text
        std::map<std::string, std::string> entries();

        // Writes now when something is unsaved; false when that write failed
        bool save();
        // The write-behind: writes when the oldest unsaved change is WRITE_DELAY old
        void tick();
        // save(), then forget what was read: the next use reads the file again (the editor
        // switching projects, a test changing the data directory)
        void unload();
        // Called by ~System: what is unsaved goes to that System's file
        void systemDestroyed(const System* system);

        bool hasUnsavedChanges() const { return dirty.load(std::memory_order_acquire); }
        // The file it reads and writes ("" before the first use)
        std::string getFilePath();

        // Tests: seconds of a fake clock (null: the steady clock)
        void setClock(std::function<double()> clock);

    private:
        UserSettingsStore() = default;

        void ensureLoaded();
        void load();
        bool writeLocked();
        void markDirty();
        double now() const;

        std::mutex mutex;
        bool loaded = false;
        const System* loadedFor = nullptr;
        std::string filePath;
        std::map<std::string, std::string> values;
        std::atomic<bool> dirty{false};
        double dirtySince = 0.0;
        bool writeErrorLogged = false;
        bool unreadable = false;
        std::function<double()> clock;
    };

}

#endif // USERSETTINGSSTORE_H
