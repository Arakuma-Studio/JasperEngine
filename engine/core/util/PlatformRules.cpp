// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlatformRules.h"

#include "Engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace jasperengine;

namespace {

    std::string lower(std::string value){
        for (char& c : value) c = (char)std::tolower((unsigned char)c);
        return value;
    }

    std::string upper(std::string value){
        for (char& c : value) c = (char)std::toupper((unsigned char)c);
        return value;
    }

    bool isAlpha(const std::string& value){
        return !value.empty() && std::all_of(value.begin(), value.end(), [](char c){ return std::isalpha((unsigned char)c) != 0; });
    }

    bool isDigits(const std::string& value){
        return !value.empty() && std::all_of(value.begin(), value.end(), [](char c){ return std::isdigit((unsigned char)c) != 0; });
    }

    // the subtags of a tag, '_' taken as '-'
    std::vector<std::string> subtags(const std::string& tag){
        std::vector<std::string> parts;
        std::string current;
        for (char c : tag){
            if (c == '-' || c == '_'){
                parts.push_back(current);
                current.clear();
            }else{
                current += c;
            }
        }
        parts.push_back(current);
        return parts;
    }

}

// ----------------------------------------------------------------------------- LocaleRules

std::string LocaleRules::normalize(const std::string& tag){
    // POSIX names carry a codeset and a modifier: language[_territory][.codeset][@modifier]
    std::string value = tag;
    const size_t cut = value.find_first_of(".@");
    if (cut != std::string::npos) value = value.substr(0, cut);
    while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
    while (!value.empty() && std::isspace((unsigned char)value.front())) value.erase(value.begin());
    if (value.empty()) return "";
    if (value == "C" || value == "POSIX") return "en-US";

    std::string out;
    bool first = true;
    for (const std::string& part : subtags(value)){
        if (part.empty()) continue;
        std::string piece;
        if (first){
            piece = lower(part);
        }else if (part.size() == 4 && isAlpha(part)){
            // script: title case (Hant)
            piece = lower(part);
            piece[0] = (char)std::toupper((unsigned char)piece[0]);
        }else if ((part.size() == 2 && isAlpha(part)) || (part.size() == 3 && isDigits(part))){
            piece = upper(part);
        }else{
            piece = lower(part);
        }
        if (!out.empty()) out += '-';
        out += piece;
        first = false;
    }
    return out;
}

std::string LocaleRules::languageCode(const std::string& tag){
    const std::vector<std::string> parts = subtags(normalize(tag));
    return parts.empty() ? std::string() : lower(parts[0]);
}

std::string LocaleRules::scriptCode(const std::string& tag){
    const std::vector<std::string> parts = subtags(normalize(tag));
    if (parts.size() > 1 && parts[1].size() == 4 && isAlpha(parts[1])) return parts[1];
    return "";
}

std::string LocaleRules::regionCode(const std::string& tag){
    const std::vector<std::string> parts = subtags(normalize(tag));
    for (size_t i = 1; i < parts.size() && i <= 2; i++){
        const std::string& part = parts[i];
        if ((part.size() == 2 && isAlpha(part)) || (part.size() == 3 && isDigits(part))) return upper(part);
        // the script comes before the region; anything else ends the search (variants)
        if (!(part.size() == 4 && isAlpha(part))) break;
    }
    return "";
}

SystemLanguage LocaleRules::toSystemLanguage(const std::string& tag){
    const std::string normalized = normalize(tag);
    const std::string language = languageCode(normalized);
    if (language.empty()) return SystemLanguage::Unknown;

    if (language == "zh" || language == "zho" || language == "chi"){
        const std::string script = lower(scriptCode(normalized));
        const std::string region = regionCode(normalized);
        if (script == "hant") return SystemLanguage::ChineseTraditional;
        if (script == "hans") return SystemLanguage::ChineseSimplified;
        if (region == "TW" || region == "HK" || region == "MO") return SystemLanguage::ChineseTraditional;
        return SystemLanguage::ChineseSimplified;
    }

    struct Entry { const char* code; SystemLanguage language; };
    // ISO 639-1 codes, with the 639-2 ones platforms report for a few, and the deprecated
    // codes Android and Java still use (iw, in)
    static const Entry entries[] = {
        {"af", SystemLanguage::Afrikaans}, {"afr", SystemLanguage::Afrikaans},
        {"ar", SystemLanguage::Arabic}, {"ara", SystemLanguage::Arabic},
        {"eu", SystemLanguage::Basque}, {"eus", SystemLanguage::Basque}, {"baq", SystemLanguage::Basque},
        {"be", SystemLanguage::Belarusian}, {"bel", SystemLanguage::Belarusian},
        {"bg", SystemLanguage::Bulgarian}, {"bul", SystemLanguage::Bulgarian},
        {"ca", SystemLanguage::Catalan}, {"cat", SystemLanguage::Catalan},
        {"cs", SystemLanguage::Czech}, {"ces", SystemLanguage::Czech}, {"cze", SystemLanguage::Czech},
        {"da", SystemLanguage::Danish}, {"dan", SystemLanguage::Danish},
        {"nl", SystemLanguage::Dutch}, {"nld", SystemLanguage::Dutch}, {"dut", SystemLanguage::Dutch},
        {"en", SystemLanguage::English}, {"eng", SystemLanguage::English},
        {"et", SystemLanguage::Estonian}, {"est", SystemLanguage::Estonian},
        {"fo", SystemLanguage::Faroese}, {"fao", SystemLanguage::Faroese},
        {"fi", SystemLanguage::Finnish}, {"fin", SystemLanguage::Finnish},
        {"fr", SystemLanguage::French}, {"fra", SystemLanguage::French}, {"fre", SystemLanguage::French},
        {"de", SystemLanguage::German}, {"deu", SystemLanguage::German}, {"ger", SystemLanguage::German},
        {"el", SystemLanguage::Greek}, {"ell", SystemLanguage::Greek}, {"gre", SystemLanguage::Greek},
        {"he", SystemLanguage::Hebrew}, {"iw", SystemLanguage::Hebrew}, {"heb", SystemLanguage::Hebrew},
        {"hu", SystemLanguage::Hungarian}, {"hun", SystemLanguage::Hungarian},
        {"is", SystemLanguage::Icelandic}, {"isl", SystemLanguage::Icelandic}, {"ice", SystemLanguage::Icelandic},
        {"id", SystemLanguage::Indonesian}, {"in", SystemLanguage::Indonesian}, {"ind", SystemLanguage::Indonesian},
        {"it", SystemLanguage::Italian}, {"ita", SystemLanguage::Italian},
        {"ja", SystemLanguage::Japanese}, {"jpn", SystemLanguage::Japanese},
        {"ko", SystemLanguage::Korean}, {"kor", SystemLanguage::Korean},
        {"lv", SystemLanguage::Latvian}, {"lav", SystemLanguage::Latvian},
        {"lt", SystemLanguage::Lithuanian}, {"lit", SystemLanguage::Lithuanian},
        {"nb", SystemLanguage::Norwegian}, {"nn", SystemLanguage::Norwegian}, {"no", SystemLanguage::Norwegian},
        {"nob", SystemLanguage::Norwegian}, {"nno", SystemLanguage::Norwegian}, {"nor", SystemLanguage::Norwegian},
        {"pl", SystemLanguage::Polish}, {"pol", SystemLanguage::Polish},
        {"pt", SystemLanguage::Portuguese}, {"por", SystemLanguage::Portuguese},
        {"ro", SystemLanguage::Romanian}, {"ron", SystemLanguage::Romanian}, {"rum", SystemLanguage::Romanian},
        {"mo", SystemLanguage::Romanian},
        {"ru", SystemLanguage::Russian}, {"rus", SystemLanguage::Russian},
        {"sr", SystemLanguage::SerboCroatian}, {"hr", SystemLanguage::SerboCroatian}, {"bs", SystemLanguage::SerboCroatian},
        {"sh", SystemLanguage::SerboCroatian}, {"srp", SystemLanguage::SerboCroatian}, {"hrv", SystemLanguage::SerboCroatian},
        {"bos", SystemLanguage::SerboCroatian},
        {"sk", SystemLanguage::Slovak}, {"slk", SystemLanguage::Slovak}, {"slo", SystemLanguage::Slovak},
        {"sl", SystemLanguage::Slovenian}, {"slv", SystemLanguage::Slovenian},
        {"es", SystemLanguage::Spanish}, {"spa", SystemLanguage::Spanish},
        {"sv", SystemLanguage::Swedish}, {"swe", SystemLanguage::Swedish},
        {"th", SystemLanguage::Thai}, {"tha", SystemLanguage::Thai},
        {"tr", SystemLanguage::Turkish}, {"tur", SystemLanguage::Turkish},
        {"uk", SystemLanguage::Ukrainian}, {"ukr", SystemLanguage::Ukrainian},
        {"vi", SystemLanguage::Vietnamese}, {"vie", SystemLanguage::Vietnamese},
        {"hi", SystemLanguage::Hindi}, {"hin", SystemLanguage::Hindi},
    };
    for (const Entry& entry : entries){
        if (language == entry.code) return entry.language;
    }
    return SystemLanguage::Unknown;
}

std::vector<std::string> LocaleRules::parseLanguageList(const std::string& list){
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= list.size()){
        const size_t end = list.find(':', start);
        const std::string item = normalize(list.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string LocaleRules::resolveRegion(const std::string& regionOverride, const std::string& tag){
    std::string region = upper(regionOverride);
    region.erase(std::remove_if(region.begin(), region.end(), [](char c){ return std::isspace((unsigned char)c) != 0; }), region.end());
    if (!region.empty()) return region;
    return regionCode(tag);
}

const char* LocaleRules::systemLanguageName(SystemLanguage language){
    static const char* names[] = {
        "Afrikaans", "Arabic", "Basque", "Belarusian", "Bulgarian", "Catalan", "Chinese", "Czech", "Danish",
        "Dutch", "English", "Estonian", "Faroese", "Finnish", "French", "German", "Greek", "Hebrew", "Hungarian",
        "Icelandic", "Indonesian", "Italian", "Japanese", "Korean", "Latvian", "Lithuanian", "Norwegian", "Polish",
        "Portuguese", "Romanian", "Russian", "SerboCroatian", "Slovak", "Slovenian", "Spanish", "Swedish", "Thai",
        "Turkish", "Ukrainian", "Vietnamese", "ChineseSimplified", "ChineseTraditional", "Unknown", "Hindi"
    };
    const int index = (int)language;
    if (index < 0 || index >= (int)(sizeof(names) / sizeof(names[0]))) return "Unknown";
    return names[index];
}

// ----------------------------------------------------------------------------- OsRules

std::string OsRules::androidOperatingSystem(const std::string& release, int sdkInt, const std::string& display, const std::string& incremental){
    return "Android OS " + release + " / API-" + std::to_string(sdkInt) + " (" + display + "/" + incremental + ")";
}

int64_t OsRules::parseBuildNumber(const std::string& version){
    size_t i = 0;
    while (i < version.size() && !std::isdigit((unsigned char)version[i])) i++;
    int64_t value = 0;
    bool any = false;
    for (; i < version.size() && std::isdigit((unsigned char)version[i]); i++){
        any = true;
        if (value > (INT64_MAX - 9) / 10) break;
        value = value * 10 + (version[i] - '0');
    }
    return any ? value : 0;
}

std::string OsRules::windowsOperatingSystem(unsigned major, unsigned minor, unsigned build, bool is64Bit){
    const char* name = "10";
    if (major == 10 && build >= 22000) name = "11";
    else if (major == 6 && minor == 3) name = "8.1";
    else if (major == 6 && minor == 2) name = "8";
    else if (major == 6 && minor == 1) name = "7";
    return std::string("Windows ") + name + " (" + std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(build) + ")" + (is64Bit ? " 64bit" : "");
}

// ----------------------------------------------------------------------------- UrlRules

std::string UrlRules::check(const std::string& url){
    if (url.empty()) return "the URL is empty";
    for (unsigned char c : url){
        if (c < 0x20 || c == 0x7f) return "the URL has control characters";
    }
    // RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
    const size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0 || !std::isalpha((unsigned char)url[0])){
        return "the URL has no scheme (write https://...)";
    }
    for (size_t i = 1; i < colon; i++){
        const unsigned char c = (unsigned char)url[i];
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return "the URL has no scheme (write https://...)";
    }
    // one letter is a Windows drive ("C:\\Windows\\...exe"), a local program for ShellExecute
    if (colon < 2) return "the URL has no scheme (\"" + url.substr(0, colon + 1) + "\" is a drive letter; write https://...)";
    const std::string scheme = lower(url.substr(0, colon));
    if (scheme == "javascript" || scheme == "data" || scheme == "file" || scheme == "vbscript"){
        return scheme + ": URLs are not opened (they run code or read local files)";
    }
    // Windows protocol handlers that run programs, installers, scripts or searches of local files
    static const char* launchers[] = {
        "ms-msdt", "search-ms", "search", "shell", "ms-appinstaller", "ms-officecmd", "ms-cxh", "ms-cxh-full",
        "ms-word", "ms-excel", "ms-powerpoint", "ms-access", "ms-publisher", "ms-visio",
        "ms-project", "ms-infopath", "ms-spd", "ms-officeapp", "ms-rd", "ms-distribution", "ms-gamingoverlay",
        "ms-inputapp", "ms-quick-assist", "ms-windows-store-install", "msi", "hcp", "its", "mk", "ms-its",
        "res", "ldap", "mhtml", "jar", "vbs", "wscript", "cscript", "powershell", "cmd", "telnet", "ssh"
    };
    for (const char* launcher : launchers){
        if (scheme == launcher) return scheme + ": URLs are not opened (they start programs or installers)";
    }
    return "";
}

// ----------------------------------------------------------------------------- PlatformRules

bool PlatformRules::isFixedOrientation(ScreenOrientation orientation){
    return orientation == ScreenOrientation::Portrait || orientation == ScreenOrientation::PortraitUpsideDown ||
           orientation == ScreenOrientation::LandscapeLeft || orientation == ScreenOrientation::LandscapeRight;
}

bool PlatformRules::isOrientation(int value){
    return value >= (int)ScreenOrientation::Portrait && value <= (int)ScreenOrientation::AutoRotation;
}

const char* PlatformRules::orientationName(ScreenOrientation orientation){
    switch (orientation){
        case ScreenOrientation::Portrait: return "portrait";
        case ScreenOrientation::PortraitUpsideDown: return "portrait_upside_down";
        case ScreenOrientation::LandscapeLeft: return "landscape_left";
        case ScreenOrientation::LandscapeRight: return "landscape_right";
        case ScreenOrientation::AutoRotation: return "auto_rotation";
    }
    return "portrait";
}

bool PlatformRules::parseOrientationName(const std::string& name, ScreenOrientation& out){
    const std::string value = lower(name);
    for (int i = (int)ScreenOrientation::Portrait; i <= (int)ScreenOrientation::AutoRotation; i++){
        if (value == orientationName((ScreenOrientation)i)){
            out = (ScreenOrientation)i;
            return true;
        }
    }
    return false;
}

uint8_t PlatformRules::effectiveMask(ScreenOrientation requested, uint8_t allowedMask){
    if (isFixedOrientation(requested)) return orientationBit(requested);
    const uint8_t mask = allowedMask & ALL_ORIENTATIONS;
    return mask ? mask : DEFAULT_ALLOWED_ORIENTATIONS;
}

ScreenOrientation PlatformRules::resolveOrientation(ScreenOrientation current, ScreenOrientation requested, uint8_t allowedMask){
    if (isFixedOrientation(requested)) return requested;
    const uint8_t mask = effectiveMask(ScreenOrientation::AutoRotation, allowedMask);
    if (isFixedOrientation(current) && (mask & orientationBit(current))) return current;
    for (ScreenOrientation candidate : {ScreenOrientation::Portrait, ScreenOrientation::LandscapeLeft,
                                        ScreenOrientation::LandscapeRight, ScreenOrientation::PortraitUpsideDown}){
        if (mask & orientationBit(candidate)) return candidate;
    }
    return ScreenOrientation::Portrait;
}

ScreenOrientation PlatformRules::orientationFromSize(int width, int height){
    return width >= height ? ScreenOrientation::LandscapeLeft : ScreenOrientation::Portrait;
}

DeviceOrientation PlatformRules::deviceOrientationOf(ScreenOrientation orientation){
    switch (orientation){
        case ScreenOrientation::Portrait: return DeviceOrientation::Portrait;
        case ScreenOrientation::PortraitUpsideDown: return DeviceOrientation::PortraitUpsideDown;
        case ScreenOrientation::LandscapeLeft: return DeviceOrientation::LandscapeLeft;
        case ScreenOrientation::LandscapeRight: return DeviceOrientation::LandscapeRight;
        default: return DeviceOrientation::Unknown;
    }
}

int PlatformRules::androidRequestedOrientation(ScreenOrientation requested, uint8_t allowedMask, bool* unsupported){
    if (unsupported) *unsupported = false;
    switch (requested){
        case ScreenOrientation::Portrait: return ANDROID_PORTRAIT;
        case ScreenOrientation::PortraitUpsideDown: return ANDROID_REVERSE_PORTRAIT;
        case ScreenOrientation::LandscapeLeft: return ANDROID_LANDSCAPE;
        case ScreenOrientation::LandscapeRight: return ANDROID_REVERSE_LANDSCAPE;
        default: break;
    }

    const uint8_t mask = effectiveMask(ScreenOrientation::AutoRotation, allowedMask);
    const uint8_t P = orientationBit(ScreenOrientation::Portrait);
    const uint8_t PU = orientationBit(ScreenOrientation::PortraitUpsideDown);
    const uint8_t LL = orientationBit(ScreenOrientation::LandscapeLeft);
    const uint8_t LR = orientationBit(ScreenOrientation::LandscapeRight);
    if (mask == P) return ANDROID_PORTRAIT;
    if (mask == PU) return ANDROID_REVERSE_PORTRAIT;
    if (mask == LL) return ANDROID_LANDSCAPE;
    if (mask == LR) return ANDROID_REVERSE_LANDSCAPE;
    if (mask == (P | PU)) return ANDROID_USER_PORTRAIT;
    if (mask == (LL | LR)) return ANDROID_USER_LANDSCAPE;
    if (mask == (P | LL | LR)) return ANDROID_USER;
    if (mask == (P | PU | LL | LR)) return ANDROID_FULL_USER;
    if (unsupported) *unsupported = true;
    return ANDROID_FULL_USER;
}

const char* PlatformRules::androidManifestOrientation(int activityInfoOrientation){
    switch (activityInfoOrientation){
        case ANDROID_LANDSCAPE: return "landscape";
        case ANDROID_PORTRAIT: return "portrait";
        case ANDROID_USER: return "user";
        case ANDROID_REVERSE_LANDSCAPE: return "reverseLandscape";
        case ANDROID_REVERSE_PORTRAIT: return "reversePortrait";
        case ANDROID_USER_LANDSCAPE: return "userLandscape";
        case ANDROID_USER_PORTRAIT: return "userPortrait";
        case ANDROID_FULL_USER: return "fullUser";
        default: return "unspecified";
    }
}

ScreenOrientation PlatformRules::orientationFromRotation(int rotationDegrees, bool naturalPortrait){
    int r = ((rotationDegrees % 360) + 360) % 360;
    if (!naturalPortrait) r = (r + 90) % 360;
    // nearest quarter
    r = ((r + 45) / 90 % 4) * 90;
    switch (r){
        case 90: return ScreenOrientation::LandscapeLeft;
        case 180: return ScreenOrientation::PortraitUpsideDown;
        case 270: return ScreenOrientation::LandscapeRight;
        default: return ScreenOrientation::Portrait;
    }
}

DeviceOrientation PlatformRules::deviceOrientationFromDegrees(int degrees, bool naturalPortrait, DeviceOrientation previous){
    if (degrees < 0) return DeviceOrientation::Unknown;
    int d = degrees % 360;
    // the listener's degrees turn clockwise (Display.getRotation counter-clockwise): a device
    // whose rotation 0 is landscape is a quarter turn back from portrait
    if (!naturalPortrait) d = (d + 270) % 360;

    // OrientationEventListener: 90 when the device's left side is up (turned clockwise),
    // 270 when its right side is up (turned counter-clockwise: Unity's LandscapeLeft)
    auto centerOf = [](DeviceOrientation orientation) -> int {
        switch (orientation){
            case DeviceOrientation::Portrait: return 0;
            case DeviceOrientation::LandscapeRight: return 90;
            case DeviceOrientation::PortraitUpsideDown: return 180;
            case DeviceOrientation::LandscapeLeft: return 270;
            default: return -1;
        }
    };
    auto distance = [](int a, int b){
        const int diff = std::abs(a - b) % 360;
        return std::min(diff, 360 - diff);
    };

    const int previousCenter = centerOf(previous);
    if (previousCenter >= 0 && distance(d, previousCenter) <= 45 + 10) return previous;

    const int bucket = ((d + 45) / 90) % 4;
    switch (bucket){
        case 1: return DeviceOrientation::LandscapeRight;
        case 2: return DeviceOrientation::PortraitUpsideDown;
        case 3: return DeviceOrientation::LandscapeLeft;
        default: return DeviceOrientation::Portrait;
    }
}

unsigned PlatformRules::iosOrientationMask(ScreenOrientation orientation){
    switch (orientation){
        case ScreenOrientation::Portrait: return 1u << 1;            // UIInterfaceOrientationMaskPortrait
        case ScreenOrientation::PortraitUpsideDown: return 1u << 2;  // ...PortraitUpsideDown
        case ScreenOrientation::LandscapeLeft: return 1u << 3;       // ...LandscapeRight
        case ScreenOrientation::LandscapeRight: return 1u << 4;      // ...LandscapeLeft
        default: return 0;
    }
}

unsigned PlatformRules::iosSupportedMask(ScreenOrientation requested, uint8_t allowedMask){
    const uint8_t mask = effectiveMask(requested, allowedMask);
    unsigned out = 0;
    for (ScreenOrientation o : {ScreenOrientation::Portrait, ScreenOrientation::PortraitUpsideDown,
                                ScreenOrientation::LandscapeLeft, ScreenOrientation::LandscapeRight}){
        if (mask & orientationBit(o)) out |= iosOrientationMask(o);
    }
    return out;
}

ScreenOrientation PlatformRules::orientationFromIos(int interfaceOrientation){
    switch (interfaceOrientation){
        case 2: return ScreenOrientation::PortraitUpsideDown;
        case 3: return ScreenOrientation::LandscapeLeft;   // UIInterfaceOrientationLandscapeRight
        case 4: return ScreenOrientation::LandscapeRight;  // UIInterfaceOrientationLandscapeLeft
        default: return ScreenOrientation::Portrait;
    }
}

int PlatformRules::iosInterfaceOrientation(ScreenOrientation orientation){
    switch (orientation){
        case ScreenOrientation::PortraitUpsideDown: return 2;
        case ScreenOrientation::LandscapeLeft: return 3;
        case ScreenOrientation::LandscapeRight: return 4;
        default: return 1;
    }
}

std::vector<std::string> PlatformRules::iosPlistOrientations(ScreenOrientation defaultOrientation, uint8_t allowedMask){
    auto plistName = [](ScreenOrientation o) -> const char* {
        switch (o){
            case ScreenOrientation::PortraitUpsideDown: return "UIInterfaceOrientationPortraitUpsideDown";
            case ScreenOrientation::LandscapeLeft: return "UIInterfaceOrientationLandscapeRight";
            case ScreenOrientation::LandscapeRight: return "UIInterfaceOrientationLandscapeLeft";
            default: return "UIInterfaceOrientationPortrait";
        }
    };
    std::vector<std::string> out;
    if (isFixedOrientation(defaultOrientation)){
        out.push_back(plistName(defaultOrientation));
        return out;
    }
    const uint8_t mask = effectiveMask(ScreenOrientation::AutoRotation, allowedMask);
    for (ScreenOrientation o : {ScreenOrientation::Portrait, ScreenOrientation::LandscapeLeft,
                                ScreenOrientation::LandscapeRight, ScreenOrientation::PortraitUpsideDown}){
        if (mask & orientationBit(o)) out.push_back(plistName(o));
    }
    return out;
}

FramePacing PlatformRules::computeFramePacing(Platform platform, int targetFrameRate, int vSyncCount,
                                              double refreshRate, int maxSwapInterval, bool canDisableVSync){
    FramePacing pacing;
    const double refresh = refreshRate > 0.0 ? refreshRate : 60.0;

    if (platform == Platform::Android || platform == Platform::iOS){
        const int wanted = targetFrameRate > 0 ? targetFrameRate : 60;
        const int maxFps = std::max(1, (int)std::lround(refresh));
        pacing.mobileFps = std::clamp(wanted, 1, maxFps);
        pacing.swapInterval = 1;
        pacing.limiterFps = 0.0;
        return pacing;
    }

    if (platform == Platform::Web){
        pacing.swapInterval = 1;
        pacing.limiterFps = targetFrameRate > 0 ? (double)targetFrameRate : 0.0;
        return pacing;
    }

    if (vSyncCount > 0){
        if (vSyncCount <= std::max(1, maxSwapInterval)){
            pacing.swapInterval = vSyncCount;
            pacing.limiterFps = 0.0;
        }else{
            pacing.swapInterval = 1;
            pacing.limiterFps = refresh / (double)vSyncCount;
        }
    }else{
        pacing.swapInterval = canDisableVSync ? 0 : 1;
        pacing.limiterFps = targetFrameRate > 0 ? (double)targetFrameRate : 0.0;
    }
    return pacing;
}

double PlatformRules::hiddenUpdateFps(int targetFrameRate, int vSyncCount, double refreshRate){
    constexpr double MAX_HIDDEN_FPS = 60.0;
    const double refresh = refreshRate > 0.0 ? refreshRate : 60.0;
    double fps = MAX_HIDDEN_FPS;
    if (vSyncCount > 0)
        fps = refresh / (double)vSyncCount;
    else if (targetFrameRate > 0)
        fps = (double)targetFrameRate;
    return std::clamp(fps, 1.0, MAX_HIDDEN_FPS);
}
