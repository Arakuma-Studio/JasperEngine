// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef USERSETTINGS_H
#define USERSETTINGS_H

#include "Export.h"
#include <string>
#include "io/Data.h"

namespace jasperengine {
    class JASPERENGINE_API UserSettings {
    public:

        static bool getBoolForKey(const char *key);
        static int getIntegerForKey(const char *key);
        static long getLongForKey(const char *key);
        static float getFloatForKey(const char *key);
        static double getDoubleForKey(const char *key);
        static std::string getStringForKey(const char *key);
        static Data getDataForKey(const char *key);

        static bool getBoolForKey(const char *key, bool defaultValue);
        static int getIntegerForKey(const char *key, int defaultValue);
        static long getLongForKey(const char *key, long defaultValue);
        static float getFloatForKey(const char *key, float defaultValue);
        static double getDoubleForKey(const char *key, double defaultValue);
        static std::string getStringForKey(const char *key, const std::string& defaultValue);
        static Data getDataForKey(const char *key, const Data& defaultValue);

        static void setBoolForKey(const char *key, bool value);
        static void setIntegerForKey(const char *key, int value);
        static void setLongForKey(const char *key, long value);
        static void setFloatForKey(const char *key, float value);
        static void setDoubleForKey(const char *key, double value);
        static void setStringForKey(const char* key, const std::string& value);
        static void setDataForKey(const char *key, Data& value);

        static void removeKey(const char *key);

        // The key is stored (an empty value too)
        static bool hasKey(const char *key);
        // Same as removeKey (Unity's name); nothing happens when the key is not there
        static void deleteKey(const char *key);
        // Every key of this game goes
        static void deleteAll();
        // Writes what is unsaved now. Without it the values still persist: desktop and the editor
        // write a second after a change and on app pause, shutdown and Play stop; Android, iOS and
        // the web stores persist by themselves (save() makes Android's write synchronous)
        static void save();

        // Engine::runFrame, once a frame: the desktop store's write-behind
        static void tick();

    };
}


#endif //USERSETTINGS_H
