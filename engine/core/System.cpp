// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "System.h"
#include "PlatformConfig.h"
#include "io/UserSettingsStore.h"
#include "util/Base64.h"
#include "util/NumberText.h"
#include "Log.h"
#include "log/LogSinks.h"
#include "util/PlatformRules.h"
#include "Screen.h"
#include <limits>
#include <mutex>
#include <set>
#include <stdlib.h>
#include <algorithm>
#include <thread>

using namespace jasperengine;

System* System::systemInstance = nullptr;
ApplicationInfo System::applicationInfo;

System& System::instance(){
    if (!systemInstance) {
        // Need to be printf here, Log class uses instance()
        printf("System instance not set. Engine must call System::setSystemInstance() during initialization.\n");
        abort();
    }

    return *systemInstance;
}

bool System::hasInstance(){
    return systemInstance != nullptr;
}

EarlySystemScope::EarlySystemScope(System* system){
    if (!System::systemInstance) {
        System::systemInstance = system;
        installed = system;
    }
}

EarlySystemScope::~EarlySystemScope(){
    // the platform may have set its own meanwhile: only ours is taken back
    if (installed && System::systemInstance == installed) System::systemInstance = nullptr;
}

void System::setSystemInstance(System* system){
    systemInstance = system;
}

sg_environment System::getSokolEnvironment(){
    sg_environment env = {};

    env.defaults.sample_count = getSampleCount();
    env.defaults.color_format = SG_PIXELFORMAT_RGBA8;
    env.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;

    return env;
}

sg_swapchain System::getSokolSwapchain(){
    sg_swapchain swapchain = { 0 };

    swapchain.width = getScreenWidth();
    swapchain.height = getScreenHeight();
    swapchain.sample_count = getSampleCount();
    swapchain.color_format = SG_PIXELFORMAT_RGBA8;
    swapchain.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;

    // we just assume here that the GL framebuffer is always 0
    swapchain.gl.framebuffer = 0;

    return swapchain;
}

void System::setMouseCursor(CursorType type){

}

void System::setMouseMode(MouseMode mode){

}

void System::setMousePosition(float x, float y){

}

int System::getSampleCount(){
    return 1;
}

bool System::isTouchDevice(){
    return false;
}

void System::showVirtualKeyboard(std::wstring text){

}

void System::hideVirtualKeyboard(){

}

void System::setClipboardText(const std::string& utf8){
    clipboardText = utf8;
}

std::string System::getClipboardText(){
    return clipboardText;
}

void System::setTextInputArea(float x, float y, float width, float height){

}

bool System::isFullscreen(){
    return false;
}

void System::requestFullscreen(){

}

void System::exitFullscreen(){

}

bool System::isWindowMaximized(){
    return false;
}

void System::maximizeWindow(){

}

void System::restoreWindow(){

}

void System::setWindowSize(int width, int height){

}

bool System::isWindowResizable(){
    return false;
}

void System::setWindowResizable(bool resizable){

}

void System::setWindowTitle(const std::string& title){

}

void System::quit(){
    Log::warn("quit() is not supported on this platform");
}

bool System::openURL(const std::string& url){
    const std::string problem = UrlRules::check(url);
    if (!problem.empty()){
        Log::error("System.openURL(\"%s\"): %s", url.c_str(), problem.c_str());
        return false;
    }
    return platformOpenURL(url);
}

bool System::platformOpenURL(const std::string& url){
    Log::warn("System.openURL is not supported on this platform (%s)", url.c_str());
    return false;
}

std::vector<std::string> System::getPreferredLocales(){
    return {"en-US"};
}

std::string System::getLocale(){
    const std::vector<std::string> locales = getPreferredLocales();
    for (const std::string& locale : locales){
        const std::string normalized = LocaleRules::normalize(locale);
        if (!normalized.empty()) return normalized;
    }
    return "en-US";
}

std::string System::getLanguageCode(){
    return LocaleRules::languageCode(getLocale());
}

std::string System::getRegionCode(){
    return LocaleRules::resolveRegion(getRegionOverride(), getLocale());
}

std::string System::getRegionOverride(){
    return "";
}

SystemLanguage System::getSystemLanguage(){
    return LocaleRules::toSystemLanguage(getLocale());
}

void System::setApplicationInfo(const ApplicationInfo& info){
    applicationInfo = info;
}

const ApplicationInfo& System::getApplicationInfo(){
    return applicationInfo;
}

std::string System::getAppVersion(){
    return applicationInfo.version;
}

int64_t System::getAppBuild(){
    return std::max<int64_t>(0, applicationInfo.build);
}

std::string System::getAppIdentifier(){
    return applicationInfo.identifier;
}

std::string System::getAppName(){
    return applicationInfo.name;
}

std::string System::getOperatingSystem(){
    return "Unknown";
}

std::string System::getOperatingSystemVersion(){
    return "";
}

int System::getAndroidApiLevel(){
    return 0;
}

std::string System::getDeviceModel(){
    return "PC";
}

DeviceType System::getDeviceType(){
    return isTouchDevice() ? DeviceType::Handheld : DeviceType::Desktop;
}

int System::getSystemMemorySize(){
    return 0;
}

std::string System::getGpuRendererName(){
    return "";
}

int System::getProcessorCount(){
    return std::max(1u, std::thread::hardware_concurrency());
}

bool System::isEditor(){
    return false;
}

NetworkReachability System::getInternetReachability(){
    return NetworkReachability::NotReachable;
}

bool System::vibrate(int milliseconds){
    return false;
}

bool System::hapticFeedback(HapticFeedback type){
    return false;
}

float System::getScreenDpi(){
    return 0.0f;
}

float System::getRefreshRate(){
    return 60.0f;
}

void System::setKeepScreenOn(bool keepOn){

}

void System::applyOrientationPolicy(ScreenOrientation requested, uint8_t allowedMask){

}

ScreenOrientation System::getScreenOrientation(){
    return PlatformRules::orientationFromSize(getScreenWidth(), getScreenHeight());
}

DeviceOrientation System::getDeviceOrientation(){
    return DeviceOrientation::Unknown;
}

void System::applyFramePacing(const FramePacing& pacing){

}

int System::getMaxSwapInterval(){
    return 1;
}

bool System::canDisableVSync(){
    return true;
}

char System::getDirSeparator(){
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

std::string System::getAssetPath(){
    return "";
}

std::string System::getUserDataPath(){
    return getAssetPath();
}

std::string System::getCachePath(){
    return getUserDataPath();
}

std::string System::getCrashReportPath(){
    const std::string userData = getUserDataPath();
    return userData.empty() ? std::string() : userData + "/crash";
}

std::string System::getLuaPath(){
    return getAssetPath();
}

std::string System::getShaderPath(){
    return getAssetPath() + "/" + "shaders";
}

std::string System::getContentPath(){
    const std::string& root = platformConfig().contentRoot;
    if (!root.empty())
        return root;
    const std::string assets = getAssetPath();
    const size_t slash = assets.find_last_of("/\\");
    if (slash == std::string::npos)
        return assets.empty() ? "" : ".";
    return slash == 0 ? "/" : assets.substr(0, slash);
}

std::string System::getImportPath(){
    return "";
}

FILE* System::platformFopen(const char* fname, const char* mode){
    return fopen(fname, mode);
}

bool System::syncFileSystem(){
    return true;
}

bool System::flushFileSystem(){
    return true;
}

bool System::isUserDataLoaded(){
    return true;
}

void System::platformLog(const int type, const char *fmt, va_list args){
    const char* priority;

    if (type == D_LOG_VERBOSE){
        priority = "VERBOSE";
    }else if (type == D_LOG_DEBUG){
        priority = "DEBUG";
    }else if (type == D_LOG_WARN){
        priority = "WARN";
    }else if (type == D_LOG_ERROR){
        priority = "ERROR";
    }

    if ((type == D_LOG_VERBOSE) || (type == D_LOG_DEBUG) || (type == D_LOG_WARN) || (type == D_LOG_ERROR))
        printf("(%s): ", priority);

    vprintf(fmt, args);
    printf("\n");
}

void System::installLogSinks(){
    PlatformLogOptions options;
    // the build's Player > Logging (PlatformDefaults.h), then a prebuilt player's (game.json)
    if (platformConfig().builtLogFile >= 0)
        options.fileLog = platformConfig().builtLogFile != 0;
    if (platformConfig().logFile >= 0)
        options.fileLog = platformConfig().logFile != 0;
    if (options.fileLog) {
        options.userDataPath = getUserDataPath();
    }
    installPlatformLogSinks(options);
}

// ---- UserSettings: the base implementation is the XML store of desktop exports and the
// editor (UserSettingsStore). Typed values are text in it, written the same in every locale
// (NumberText); a value that does not read as the type asked for gives the default (warned
// once per key), never an exception. The typed getters go through hasKey/getStringForKey, so a
// platform overriding only those (the web) keeps these rules.

namespace {

    std::mutex warnedKeysMutex;
    std::set<std::string> warnedKeys;

    void warnBadValue(const char* key, const std::string& value, const char* type){
        {
            std::scoped_lock lock(warnedKeysMutex);
            if (!warnedKeys.insert(key).second) return;
        }
        Log::warn("UserSettings: the value \"%s\" of key \"%s\" is not %s; the default is used "
                  "(read a key with the type it was written with)", value.c_str(), key, type);
    }

    // Older builds wrote floats with the decimal point of the player's locale ("0,500000")
    std::string dottedNumber(const std::string& text){
        const size_t comma = text.find(',');
        if (comma == std::string::npos || text.find(',', comma + 1) != std::string::npos ||
                text.find('.') != std::string::npos)
            return text;
        std::string dotted = text;
        dotted[comma] = '.';
        return dotted;
    }

}

System::~System(){
    UserSettingsStore::instance().systemDestroyed(this);
}

bool System::hasKey(const char *key){
    return key && UserSettingsStore::instance().has(key);
}

void System::removeAllKeys(){
    UserSettingsStore::instance().clear();
}

void System::saveUserSettings(){
    UserSettingsStore::instance().save();
}

bool System::getBoolForKey(const char *key, bool defaultValue){
    if (!hasKey(key)) return defaultValue;
    const std::string value = getStringForKey(key, "");
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    warnBadValue(key, value, "a boolean");
    return defaultValue;
}

int System::getIntegerForKey(const char *key, int defaultValue){
    if (!hasKey(key)) return defaultValue;
    const std::string value = getStringForKey(key, "");
    long long number = 0;
    if (NumberText::parseInteger(value, number) && number >= std::numeric_limits<int>::min() &&
            number <= std::numeric_limits<int>::max())
        return static_cast<int>(number);
    warnBadValue(key, value, "a 32-bit integer");
    return defaultValue;
}

long System::getLongForKey(const char *key, long defaultValue){
    if (!hasKey(key)) return defaultValue;
    const std::string value = getStringForKey(key, "");
    long long number = 0;
    if (NumberText::parseInteger(value, number) && number >= std::numeric_limits<long>::min() &&
            number <= std::numeric_limits<long>::max())
        return static_cast<long>(number);
    warnBadValue(key, value, "an integer");
    return defaultValue;
}

float System::getFloatForKey(const char *key, float defaultValue){
    if (!hasKey(key)) return defaultValue;
    const std::string value = getStringForKey(key, "");
    float number = 0.0f;
    if (NumberText::parseFloat(dottedNumber(value), number))
        return number;
    warnBadValue(key, value, "a float");
    return defaultValue;
}

double System::getDoubleForKey(const char *key, double defaultValue){
    if (!hasKey(key)) return defaultValue;
    const std::string value = getStringForKey(key, "");
    double number = 0.0;
    if (NumberText::parseDouble(dottedNumber(value), number))
        return number;
    warnBadValue(key, value, "a number");
    return defaultValue;
}

Data System::getDataForKey(const char *key, const Data& defaultValue){
    if (!hasKey(key)) return defaultValue;
    const std::string ret = getStringForKey(key, "");
    if (ret.empty())
        return defaultValue;

    std::vector<unsigned char> decodedData = Base64::decode(ret);
    if (decodedData.empty())
        return defaultValue;

    return Data(&decodedData[0], (unsigned int)decodedData.size(), true, true);
}

std::string System::getStringForKey(const char *key, const std::string& defaultValue){
    std::string value;
    if (key && UserSettingsStore::instance().get(key, value))
        return value;   // a stored empty string too
    return defaultValue;
}

void System::setBoolForKey(const char *key, bool value){
    setStringForKey(key, value ? "true" : "false");
}

void System::setIntegerForKey(const char *key, int value){
    setStringForKey(key, std::to_string(value));
}

void System::setLongForKey(const char *key, long value){
    setStringForKey(key, std::to_string(value));
}

void System::setFloatForKey(const char *key, float value){
    setStringForKey(key, NumberText::formatFloat(value));
}

void System::setDoubleForKey(const char *key, double value){
    setStringForKey(key, NumberText::formatDouble(value));
}

void System::setDataForKey(const char *key, Data& value){
    if (value.getMemPtr()) {
        setStringForKey(key, Base64::encode(value.getMemPtr(), value.length()));
    }else{
        Log::error("No data to add for key: %s", key ? key : "(null)");
    }
}

void System::setStringForKey(const char* key, const std::string& value){
    if (!key || !key[0]){
        Log::error("UserSettings: a key must be a non-empty string");
        return;
    }
    UserSettingsStore::instance().set(key, value);
}

void System::removeKey(const char *key){
    if (key) UserSettingsStore::instance().remove(key);
}

void System::initializeCrazyGamesSDK(){
    Log::error("Cannot initialize CrazyGames SDK in this system");
}

void System::showCrazyGamesAd(const std::string& type){
    Log::error("Cannot show CrazyGames ad in this system");
}

void System::happytimeCrazyGames(){
}

void System::gameplayStartCrazyGames(){
}

void System::gameplayStopCrazyGames(){
}

void System::loadingStartCrazyGames(){
}

void System::loadingStopCrazyGames(){
}

bool System::getSimulatedSafeArea(EdgeInsets& insets){
    const char* text = getenv("JASPERENGINE_SIMULATE_SAFE_AREA");
    if (!text || text[0] == '\0') return false;
    if (!Screen::parseSimulatedSafeArea(text, insets)){
        Log::warn("JASPERENGINE_SIMULATE_SAFE_AREA=\"%s\" is not \"top,right,bottom,left\" (integer pixels); ignored", text);
        return false;
    }
    return true;
}
